// perfmon.cpp
#include "perfmon.h"

#include <windowsx.h>

#include <algorithm>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace lc {

namespace {
constexpr int kW = 176;
constexpr int kH = 86;
// Grown only when the meters are shown, so switching them off gives back
// the space rather than leaving a gap.
constexpr int kMeterH = 52;

// Along the top or bottom the readout lays itself out sideways.
//
// A tall block clinging to the top edge eats a strip of the screen the full
// depth of the panel, which is exactly the part of the workspace most likely
// to matter. Spread along the edge it is the same information in a fraction
// of the vertical space. On the left or right there is no such pressure, so
// the compact block stays.
// Widened for the sensitivity readout.
//
// Squeezing it into the existing width would have left the trace about a
// hundred pixels across, which is too few columns to read anything from --
// and the graph is the part of this that earns its space.
constexpr int kBarW    = 736;
constexpr int kBarH    = 30;
constexpr int kBarMetH = 16;

static bool isHorizontal(int edge) { return edge == 0 || edge == 2; }

// Creation, placement and painting all have to agree on the size, so it is
// worked out in one place.
static int windowHeight(bool meters) { return meters ? kH + kMeterH : kH; }
static int layoutW(int edge, bool meters) {
    return isHorizontal(edge) ? kBarW : kW;
}
static int layoutH(int edge, bool meters) {
    return isHorizontal(edge) ? (meters ? kBarH + kBarMetH : kBarH)
                              : windowHeight(meters);
}
constexpr int kMargin = 0;   // flush against the edge, not near it

const COLORREF kBg     = RGB(0x12, 0x15, 0x1A);
const COLORREF kText   = RGB(0xE6, 0xE9, 0xEE);
const COLORREF kDim    = RGB(0x8B, 0x93, 0xA1);
const COLORREF kAccent = RGB(0x4F, 0xC7, 0xAC);
const COLORREF kWarn   = RGB(0xE8, 0xA8, 0x4C);
}

PerfMonitor::~PerfMonitor() { destroy(); }

LRESULT CALLBACK PerfMonitor::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = (PerfMonitor*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_NCHITTEST:
        // The whole window is a drag handle. There is nothing to click on it.
        return HTCAPTION;

    case WM_EXITSIZEMOVE:
        if (self) {
            // Snap to whichever corner it was dropped nearest, so it always
            // ends up tidy against an edge rather than floating.
            RECT r{};
            GetWindowRect(h, &r);
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(MONITORINFO)};
            GetMonitorInfoW(mon, &mi);
            // Nearest edge, and the position along it is kept rather than
            // rounded to a preset. Dropping it two-thirds along the top edge
            // should leave it two-thirds along the top edge.
            const float cx = (float)(r.left + r.right) * 0.5f;
            const float cy = (float)(r.top + r.bottom) * 0.5f;
            const float l = (float)mi.rcMonitor.left,  rgt = (float)mi.rcMonitor.right;
            const float t = (float)mi.rcMonitor.top,   bot = (float)mi.rcMonitor.bottom;

            const float dLeft   = cx - l;
            const float dRight  = rgt - cx;
            const float dTop    = cy - t;
            const float dBottom = bot - cy;

            float best = dTop;
            int   edge = 0;
            if (dRight  < best) { best = dRight;  edge = 1; }
            if (dBottom < best) { best = dBottom; edge = 2; }
            if (dLeft   < best) { best = dLeft;   edge = 3; }

            const float u = (edge == 0 || edge == 2)
                          ? (cx - l) / (rgt - l)
                          : (cy - t) / (bot - t);
            // Pull to the ends, so a corner is easy to hit on purpose. The
            // exact end of an edge is a one-pixel target otherwise.
            float uu = u;
            if (uu < 0.08f) uu = 0.0f;
            else if (uu > 0.92f) uu = 1.0f;
            self->snapToEdge(edge, uu, true);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        if (self) self->paint(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // painted in full below; erasing first only flickers
    }
    return DefWindowProcW(h, m, w, l);
}

bool PerfMonitor::create(HINSTANCE inst, Log& log) {
    destroy();
    inst_ = inst;
    log_  = &log;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc   = Proc;
        wc.hInstance     = inst;
        wc.lpszClassName = L"LoopcorePerfMon";
        wc.hCursor       = LoadCursor(nullptr, IDC_SIZEALL);
        if (!RegisterClassExW(&wc)) {
            log.error("Could not register the performance monitor window class.");
            return false;
        }
        registered = true;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"LoopcorePerfMon", L"", WS_POPUP,
        0, 0, (int)(layoutW(edge_, meters_) * dpiScale_),
        (int)(layoutH(edge_, meters_) * dpiScale_),
        nullptr, nullptr, inst, nullptr);
    if (!hwnd_) {
        log.error("Could not create the performance monitor window.");
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // Uniform alpha: translucent, and GDI text still renders correctly.
    SetLayeredWindowAttributes(hwnd_, 0, 225, LWA_ALPHA);

    // Same reason as the overlay: never let this appear in the captured
    // frame the model is about to look at.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);

    // Sized for whichever display it sits on, which need not be the one the
    // panel is on. Two monitors at different scalings is the normal case for
    // this window, not an edge case.
    refreshScale();

    font_ = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontSmall_ = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    snapToEdge(edge_, edgeT_, false);
    return true;
}

void PerfMonitor::destroy() {
    if (font_)      { DeleteObject(font_); font_ = nullptr; }
    if (fontSmall_) { DeleteObject(fontSmall_); fontSmall_ = nullptr; }
    if (hwnd_)      { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    visible_ = false;
}

namespace {

// Enumerated fresh each time rather than cached: displays get plugged in,
// unplugged and rearranged, and a stale list places the window off-screen.
// Resolved once, kept for the life of the process.
//
// LoadLibrary increments a reference count that these call sites never
// released, and the ones on a display or scale change run repeatedly. The
// module is a system DLL that stays loaded regardless, so the leak is a
// counter rather than memory -- but a handle taken on a timer and never
// given back is still wrong, and resolving once is simpler than balancing
// each call.
static bool DpiForMonitor(HMONITOR mon, UINT& dx, UINT& dy) {
    using PFN_Mon = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static PFN_Mon fn = [] () -> PFN_Mon {
        HMODULE m = LoadLibraryA("shcore.dll");
        return m ? (PFN_Mon)GetProcAddress(m, "GetDpiForMonitor") : nullptr;
    }();
    dx = dy = 96;
    return fn && SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy));
}

std::vector<MONITORINFO> enumerateDisplays() {
    std::vector<MONITORINFO> out;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* v = (std::vector<MONITORINFO>*)lp;
            MONITORINFO mi{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(mon, &mi)) {
                // Raw enumeration order, deliberately.
                //
                // This used to hoist the primary to the front so that index
                // zero meant something obvious. Capture enumerates the same
                // monitors without reordering, so the two lists disagreed
                // about what any index meant -- and the display setting is
                // resolved against capture's list and then applied to this
                // one. On a machine where the primary does not happen to
                // enumerate first, the readout landed on the wrong screen
                // and only picking one by hand made them agree.
                //
                // An index is only useful if everything means the same thing
                // by it, so nothing reorders now.
                v->push_back(mi);
            }
            return TRUE;
        }, (LPARAM)&out);
    return out;
}

} // namespace

int PerfMonitor::DisplayCount() {
    return (int)enumerateDisplays().size();
}

std::string PerfMonitor::DisplayName(int index) {
    const auto all = enumerateDisplays();
    if (index < 0 || index >= (int)all.size()) return "display";
    const RECT& r = all[index].rcMonitor;
    char b[64];
    snprintf(b, sizeof(b), "%d x %d%s", (int)(r.right - r.left),
             (int)(r.bottom - r.top),
             (all[index].dwFlags & MONITORINFOF_PRIMARY) ? "  primary" : "");
    return b;
}

void PerfMonitor::setDisplay(int index) {
    if (display_ == index) return;
    display_ = index;
    if (!hwnd_) return;
    // The new display may be scaled differently, so the size is recomputed
    // before the window is placed on it.
    refreshScale();
    snapToEdge(edge_, edgeT_, false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PerfMonitor::setUserScale(float s) {
    s = std::clamp(s, 0.6f, 2.5f);
    if (std::fabs(s - userScale_) < 0.01f) return;
    userScale_ = s;
    if (hwnd_) {
        refreshScale();
        snapToEdge(edge_, edgeT_, false);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PerfMonitor::refreshScale() {
    const auto all = enumerateDisplays();
    float sc = 1.0f;
    if (display_ >= 0 && display_ < (int)all.size()) {
        HMONITOR mon = MonitorFromPoint(
            POINT{all[display_].rcMonitor.left + 8,
                  all[display_].rcMonitor.top + 8}, MONITOR_DEFAULTTOPRIMARY);
        UINT dx = 96, dy = 96;
        DpiForMonitor(mon, dx, dy);
        if (dx) sc = std::clamp((float)dx / 96.0f, 1.0f, 3.0f);
    }
    dpiScale_ = sc * userScale_;
}

void PerfMonitor::setMeters(bool on) {
    if (meters_ == on) return;
    meters_ = on;
    if (hwnd_) {
        // Re-placed rather than merely resized: the anchor is an edge, and a
        // window that grew downward off a bottom edge would hang off it.
        snapToEdge(edge_, edgeT_, false);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PerfMonitor::setCaptureVisible(bool v) {
    // Applied unconditionally rather than only on a change.
    //
    // The cached flag survives a window being destroyed and recreated, but
    // the new window starts with default affinity -- so the two silently
    // disagree and the setting appears to work sometimes and not others.
    // The call is cheap; caching it was a false economy.
    if (!hwnd_) return;
    captureVisible_ = v;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    SetWindowDisplayAffinity(hwnd_, v ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
}

void PerfMonitor::setVisible(bool v) {
    // Switching it on rebuilds, always -- not only when the handle is dead.
    //
    // Twice now the handle has been perfectly valid while the readout showed
    // nothing, so a rebuild conditional on a dead handle never fired and
    // toggling it off and on did nothing at all. That is the first thing
    // anyone tries, and it has to work whatever went wrong.
    //
    // A window creation costs a fraction of a millisecond and happens only
    // when a person clicks a switch, so there is no reason to be clever
    // about when to skip it.
    if (v && !visible_ && inst_ && log_) {
        HINSTANCE inst = inst_;
        Log* log = log_;
        // hwnd_ is deliberately left alone: create() calls destroy(), which
        // is what closes the old window. Clearing it first would hand
        // destroy() a null and leak a live window every time this ran --
        // which, now that it runs on every toggle rather than only on a dead
        // handle, would be every time.
        if (!create(inst, *log)) return;
        log->info("The performance readout was rebuilt.");
        visible_ = false;          // create() leaves it hidden
    }

    if (!hwnd_ || v == visible_) return;
    ShowWindow(hwnd_, v ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = v;
}

void PerfMonitor::snapToEdge(int edge, float t, bool animate) {
    if (!hwnd_) return;
    const bool wasHorizontal = isHorizontal(edge_);
    edge_  = std::clamp(edge, 0, 3);
    edgeT_ = std::clamp(t, 0.0f, 1.0f);
    // Crossing between a side and the top or bottom changes the shape, so
    // the contents have to be redrawn rather than merely moved.
    if (wasHorizontal != isHorizontal(edge_))
        InvalidateRect(hwnd_, nullptr, FALSE);

    // The chosen display, not whichever one the window happens to be over.
    // Deriving it from the window means the two disagree the moment it is
    // dragged, and the placement then fights the setting.
    const auto all = enumerateDisplays();
    MONITORINFO mi{sizeof(MONITORINFO)};
    if (display_ >= 0 && display_ < (int)all.size()) {
        mi = all[display_];
    } else {
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(mon, &mi)) return;
    }

    // rcMonitor, not rcWork: flush against the screen edge means the screen,
    // not whatever the taskbar left over.
    const RECT& m = mi.rcMonitor;
    // Real pixels for placement: logical units are only for drawing.
    const int   w     = (int)(layoutW(edge_, meters_) * dpiScale_);
    const int   h     = (int)(layoutH(edge_, meters_) * dpiScale_);
    const float spanX = (float)(m.right - m.left - w);
    const float spanY = (float)(m.bottom - m.top - h);

    switch (edge_) {
    case 1:  dstX_ = (float)(m.right - w);  dstY_ = m.top + spanY * edgeT_; break;
    case 2:  dstX_ = m.left + spanX * edgeT_; dstY_ = (float)(m.bottom - h); break;
    case 3:  dstX_ = (float)m.left;         dstY_ = m.top + spanY * edgeT_; break;
    default: dstX_ = m.left + spanX * edgeT_; dstY_ = (float)m.top;         break;
    }

    if (!animate) {
        curX_ = dstX_;
        curY_ = dstY_;
        animating_ = false;
        SetWindowPos(hwnd_, nullptr, (int)curX_, (int)curY_, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
        return;
    }

    RECT r{};
    if (!GetWindowRect(hwnd_, &r)) {
        // No usable starting point, so there is nothing to animate from.
        curX_ = dstX_;
        curY_ = dstY_;
        animating_ = false;
        SetWindowPos(hwnd_, nullptr, (int)curX_, (int)curY_, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
        return;
    }

    curX_ = (float)r.left;
    curY_ = (float)r.top;

    // Kept within reach of the virtual desktop.
    //
    // A window rect read while a display was being reconfigured -- which a
    // game switching resolution or entering exclusive fullscreen does --
    // can come back far outside any monitor. The animation then starts from
    // there and, since nothing bounds it, the window can spend a long time
    // travelling across empty coordinate space or simply settle somewhere
    // nothing is drawn. It reads as the readout vanishing.
    {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw > 0 && vh > 0) {
            const float slack = 400.0f;
            if (curX_ < vx - slack || curX_ > vx + vw + slack ||
                curY_ < vy - slack || curY_ > vy + vh + slack) {
                curX_ = dstX_;
                curY_ = dstY_;
            }
        }
    }

    animating_ = true;
    lastAnim_ = now_ns();
}

void PerfMonitor::reassert() {
    // Put back on top, and shown again if something hid it.
    //
    // A game taking exclusive fullscreen tears down the topmost band that
    // every other window relies on, and the readout simply stops being
    // drawn even though nothing here changed. It cannot be prevented, so it
    // is corrected: the state is checked a few times a second and restored
    // when it has been lost.
    if (!visible_) return;

    static int64_t lastCheck = 0;
    const int64_t now = now_ns();
    if (now - lastCheck < 1'000'000'000LL) return;
    lastCheck = now;

    // A handle that is no longer a window means the window is gone, not
    // merely hidden.
    //
    // Checking IsWindowVisible on a dead handle answers false and the old
    // code then called ShowWindow on it, which does nothing -- so a readout
    // that had actually been destroyed stayed missing for the rest of the
    // session while everything here reported that it was fine. Rebuilding is
    // the only thing that recovers it, and doing so is cheap enough to be
    // worth attempting rather than diagnosing.
    if (!hwnd_ || !IsWindow(hwnd_)) {
        // Copied out first: create() calls destroy(), which clears these.
        HINSTANCE inst = inst_;
        Log* log = log_;
        const bool wasVisible = visible_;
        hwnd_ = nullptr;
        if (inst && log && create(inst, *log)) {
            setVisible(wasVisible);
            log->warn("The performance readout had disappeared and was "
                      "rebuilt. Something outside loopcore destroyed it.");
        }
        return;
    }

    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // No paint watchdog here any more.
    //
    // There was one: it timed how long since paint() last ran and rebuilt
    // the window if it went quiet. It worked, but it was treating the
    // symptom -- rebuilding a perfectly healthy window on a timer because
    // the check that should have caught the fault was asking the wrong
    // question. The z-order test below asks the right one, so there is
    // nothing left for a watchdog to catch.
    // Whether it is actually in front, not whether it holds the flag.
    //
    // This used to test WS_EX_TOPMOST and skip the re-insert when the flag
    // was set. That was the wrong question, and it is the reason the readout
    // kept vanishing.
    //
    // WS_EX_TOPMOST is a membership, not a position. Many windows hold it at
    // once -- the game, its own overlay, loopcore's bounding-box overlay, a
    // chat client -- and within that band they are ordered by whoever called
    // SetWindowPos(HWND_TOPMOST) most recently. So when a game raises itself,
    // this window ends up behind it while keeping the flag, and every check
    // reports it as healthy: IsWindow yes, IsWindowVisible yes, style still
    // topmost. Nothing re-raises it, and ShowWindow does not change z-order,
    // which is why switching it off and on never helped either.
    //
    // The real question is what is actually on top at this point on screen.
    // WindowFromPoint answers it, and it is a cheap lookup rather than a
    // z-order change -- so the cost the old version was avoiding is only
    // paid when the window has genuinely been covered.
    RECT wr{};
    bool covered = false;
    if (GetWindowRect(hwnd_, &wr)) {
        const POINT probe{ (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
        const HWND atPoint = WindowFromPoint(probe);
        // Our own child windows count as us.
        covered = atPoint && atPoint != hwnd_ &&
                  GetAncestor(atPoint, GA_ROOT) != hwnd_;
    }

    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) == 0 || covered)
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER);
}

void PerfMonitor::animate() {
    reassert();
    if (!hwnd_ || !animating_) return;
    // Repainted as it moves: SetWindowPos alone relocates a layered window
    // without redrawing its contents, so the graph would freeze mid-glide.
    InvalidateRect(hwnd_, nullptr, FALSE);

    const int64_t now = now_ns();
    float dt = (float)((now - lastAnim_) / 1e9);
    lastAnim_ = now;
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;

    // Exponential approach: fast at first, easing in as it arrives, and
    // frame-rate independent so it looks the same however often this runs.
    const float a = 1.0f - expf(-14.0f * dt);
    curX_ += (dstX_ - curX_) * a;
    curY_ += (dstY_ - curY_) * a;

    // A destination outside every display means the placement was computed
    // from a monitor rectangle that has since changed. Recomputing is
    // cheaper than leaving the window somewhere nobody can see it.
    if (!MonitorFromPoint(POINT{(int)dstX_ + 4, (int)dstY_ + 4},
                          MONITOR_DEFAULTTONULL)) {
        snapToEdge(edge_, edgeT_, false);
        return;
    }

    if (std::fabs(dstX_ - curX_) < 0.6f && std::fabs(dstY_ - curY_) < 0.6f) {
        curX_ = dstX_;
        curY_ = dstY_;
        animating_ = false;
    }
    // Moved, not re-stacked: the z-order is only touched by reassert(), and
    // only when it has actually been lost. This runs every animation frame.
    SetWindowPos(hwnd_, nullptr, (int)(curX_ + 0.5f), (int)(curY_ + 0.5f),
                 (int)(layoutW(edge_, meters_) * dpiScale_),
                 (int)(layoutH(edge_, meters_) * dpiScale_),
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
}

void PerfMonitor::update(const PerfSnapshot& s) {
    if (!hwnd_ || !visible_) return;
    snap_ = s;

    // Slow average, so the arrow reflects a real trend and not the last
    // couple of frames.
    if (s.loopP50 > 0.0)
        avg_ = (avg_ <= 0.0) ? s.loopP50 : avg_ * 0.98 + s.loopP50 * 0.02;

    // 10 Hz is plenty for something a human reads, and keeps this off the
    // budget it is supposed to be measuring.
    // One sample per redraw, which is a fixed 10 Hz. That makes the span an
    // honest number of seconds no matter what the loop is doing.
    const int64_t now = now_ns();
    // Ten times a second at rest, sixty while it is moving.
    //
    // A fixed 10 Hz is plenty for numbers that change slowly, and far too
    // few for a window sliding across the screen -- which is why the glide
    // looked like it was stepping rather than moving.
    const int64_t interval = animating_ ? 16'000'000LL : 100'000'000LL;
    if (now - lastPaint_ < interval) return;
    lastPaint_ = now;

    // History lives in the shared trace now, written by the frame path on a
    // wall clock. Sampling it again here would only add a second, coarser
    // copy of the same data.
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PerfMonitor::paint(HDC dc) {
    const int winW = layoutW(edge_, meters_);
    const int winH = layoutH(edge_, meters_);

    // The whole drawing is scaled once, here, rather than every coordinate
    // in it being multiplied. Logical units stay the numbers the layout was
    // written in, and the viewport maps them onto real pixels.
    if (dpiScale_ != 1.0f) {
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc, winW, winH, nullptr);
        SetViewportExtEx(dc, (int)(winW * dpiScale_), (int)(winH * dpiScale_),
                         nullptr);
        SetViewportOrgEx(dc, 0, 0, nullptr);
    }

    // Background and border, common to both shapes.
    {
        RECT rc{0, 0, winW, winH};
        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x28, 0x2D, 0x36));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, winW, winH);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }

    SetBkMode(dc, TRANSPARENT);
    if (isHorizontal(edge_)) paintHorizontal(dc);
    else                     paintVertical(dc);
}

void PerfMonitor::paintHorizontal(HDC dc) {
    char buf[128];

    // One row of readings, then the graph and meters sharing the row below.
    // Everything the tall layout shows, in a third of the height.
    const int yText = 5;

    SelectObject(dc, font_);
    SetTextColor(dc, snap_.dropPct > 5.0 ? kWarn : kAccent);
    snprintf(buf, sizeof(buf), "%.2f ms", snap_.loopP50);
    TextOutA(dc, 10, yText - 1, buf, (int)strlen(buf));

    SelectObject(dc, fontSmall_);

    SetTextColor(dc, kDim);
    snprintf(buf, sizeof(buf), "p99 %.1f", snap_.loopP99);
    TextOutA(dc, 84, yText + 2, buf, (int)strlen(buf));

    SetTextColor(dc, kText);
    snprintf(buf, sizeof(buf), "%.0f Hz", snap_.cadenceHz);
    TextOutA(dc, 148, yText + 2, buf, (int)strlen(buf));

    SetTextColor(dc, snap_.dropPct > 5.0 ? kWarn : kDim);
    snprintf(buf, sizeof(buf), "%.1f%% drop", snap_.dropPct);
    TextOutA(dc, 202, yText + 2, buf, (int)strlen(buf));

    if (snap_.fgFps > 1.0f) {
        SetTextColor(dc, snap_.stride > 1 ? kWarn : kDim);
        if (snap_.stride > 1)
            snprintf(buf, sizeof(buf), "%.0f fps 1/%d", snap_.fgFps, snap_.stride);
        else
            snprintf(buf, sizeof(buf), "%.0f fps", snap_.fgFps);
        TextOutA(dc, 262, yText + 2, buf, (int)strlen(buf));
    }

    SetTextColor(dc, snap_.detections > 0 ? kText : kDim);
    snprintf(buf, sizeof(buf), "%d tgt", snap_.detections);
    TextOutA(dc, 336, yText + 2, buf, (int)strlen(buf));

    SetTextColor(dc, snap_.topScore > 0.0f ? kAccent : kDim);
    snprintf(buf, sizeof(buf), "%.2f", snap_.topScore);
    TextOutA(dc, 384, yText + 2, buf, (int)strlen(buf));

    {
        HBRUSH db = CreateSolidBrush(snap_.engaged ? kAccent
                                                   : RGB(0x35, 0x3B, 0x45));
        RECT dr{424, yText + 4, 431, yText + 11};
        FillRect(dc, &dr, db);
        DeleteObject(db);
        SetTextColor(dc, snap_.engaged ? kAccent : kDim);
        TextOutA(dc, 436, yText + 2, snap_.engaged ? "on" : "idle",
                 snap_.engaged ? 2 : 4);
    }

    // A camera while frames are being written.
    //
    // Drawn as a few rectangles rather than text: the readout is glanced at,
    // not read, and a shape registers where the word "capture" would need
    // focusing on.
    if (snap_.capturing) {
        const int cx = 536, cy = yText + 9;
        HBRUSH cb = CreateSolidBrush(kWarn);
        RECT body{cx - 7, cy - 5, cx + 7, cy + 5};
        RECT bump{cx - 3, cy - 8, cx + 1, cy - 4};
        FillRect(dc, &bump, cb);
        FillRect(dc, &body, cb);
        DeleteObject(cb);
        HBRUSH lens = CreateSolidBrush(kBg);
        RECT hole{cx - 2, cy - 2, cx + 2, cy + 2};
        FillRect(dc, &hole, lens);
        DeleteObject(lens);
    }

    // Distance scaling, shown only while it is actually reducing anything.
    // At full sensitivity there is nothing to report and the space is better
    // left empty than filled with "100%".
    if (snap_.sensScale >= 0.0f && snap_.sensScale < 0.995f) {
        SetTextColor(dc, snap_.sensScale < 0.5f ? kWarn : kDim);
        snprintf(buf, sizeof(buf), "%.0f%% sens", snap_.sensScale * 100.0f);
        TextOutA(dc, 474, yText + 2, buf, (int)strlen(buf));
    }

    // The graph fills the right of the top row, where there is width to
    // spare, so the trace reads better here than it can in the tall layout.
    {
        const int gx = 556, gy = 4, gw = kBarW - gx - 10, gh = 20;
        HBRUSH gb = CreateSolidBrush(RGB(0x1A, 0x1E, 0x25));
        RECT gr{gx, gy, gx + gw, gy + gh};
        FillRect(dc, &gr, gb);
        DeleteObject(gb);

        // From the same wall-clock trace the tall layout and the Debug tab
        // read, so all three agree and none stretches with the frame rate.
        if (trace_) {
            trace_->sample(spanSec_, gw / 2, cols_);
            float lo = 1e30f, hi = -1e30f;
            for (const auto& c : cols_)
                if (c.has) { lo = std::min(lo, c.mn); hi = std::max(hi, c.mx); }
            if (lo <= hi) {
                if (hi - lo < 0.25f) {
                    const float m = (lo + hi) * 0.5f;
                    lo = m - 0.125f; hi = m + 0.125f;
                }
                const float sx = (float)gw /
                                 (float)std::max<size_t>(1, cols_.size());
                std::vector<POINT> poly;
                poly.reserve(cols_.size());
                for (size_t i = 0; i < cols_.size(); ++i) {
                    if (!cols_[i].has) continue;
                    poly.push_back({gx + (int)(i * sx),
                                    gy + gh - 1 -
                                    (int)((cols_[i].mean - lo) / (hi - lo) *
                                          (gh - 2))});
                }
                if (poly.size() > 1) {
                    HPEN gp = CreatePen(PS_SOLID, 1,
                                        snap_.dropPct > 5.0 ? kWarn : kAccent);
                    HGDIOBJ opn = SelectObject(dc, gp);
                    Polyline(dc, poly.data(), (int)poly.size());
                    SelectObject(dc, opn);
                    DeleteObject(gp);
                }
            }
        }
    }

    // Meters run along the bottom strip, side by side rather than stacked.
    if (meters_) {
        const int my = kBarH + 3;
        const int barH = 4;
        int mx = 10;
        const int cellW = 150;

        auto meter = [&](const char* label, float pct, COLORREF fill) {
            if (pct < 0.0f) return;
            SetTextColor(dc, RGB(0x8A, 0x92, 0x9C));
            TextOutA(dc, mx, my - 4, label, (int)strlen(label));

            const int bx = mx + 30;
            const int bw = cellW - 74;
            HBRUSH bg2 = CreateSolidBrush(RGB(0x1A, 0x1E, 0x25));
            RECT br{bx, my, bx + bw, my + barH};
            FillRect(dc, &br, bg2);
            DeleteObject(bg2);

            const int filled = (int)(bw * std::clamp(pct, 0.0f, 100.0f) / 100.0f);
            if (filled > 0) {
                HBRUSH fb = CreateSolidBrush(fill);
                RECT fr{bx, my, bx + filled, my + barH};
                FillRect(dc, &fr, fb);
                DeleteObject(fb);
            }

            char t[16];
            snprintf(t, sizeof(t), "%.0f%%", pct);
            SetTextColor(dc, RGB(0x5C, 0x64, 0x71));
            TextOutA(dc, bx + bw + 4, my - 4, t, (int)strlen(t));
            mx += cellW;
        };

        meter("cpu",  snap_.cpuPct,  snap_.cpuPct  > 75.0f ? kWarn : kAccent);
        meter("gpu",  snap_.gpuPct,  snap_.gpuPct  > 90.0f ? kWarn : kAccent);
        meter("vram", snap_.vramPct, snap_.vramPct > 90.0f ? kWarn : kAccent);

        if (snap_.gpuMaxMHz > 0) {
            const bool low = snap_.gpuMHz < snap_.gpuMaxMHz * 3 / 4;
            SetTextColor(dc, (snap_.throttled || low) ? kWarn
                                                      : RGB(0x5C, 0x64, 0x71));
            snprintf(buf, sizeof(buf), "%d/%d MHz  %d C",
                     snap_.gpuMHz, snap_.gpuMaxMHz, snap_.gpuTempC);
            TextOutA(dc, mx + 4, my - 4, buf, (int)strlen(buf));
        }
    }
}

void PerfMonitor::paintVertical(HDC dc) {
    char buf[128];

    // Headline: the loop time, coloured by whether frames are being dropped.
    SelectObject(dc, font_);
    SetTextColor(dc, snap_.dropPct > 5.0 ? kWarn : kAccent);
    snprintf(buf, sizeof(buf), "%.2f ms", snap_.loopP50);
    TextOutA(dc, 9, 5, buf, (int)strlen(buf));

    SelectObject(dc, fontSmall_);
    SetTextColor(dc, kDim);
    snprintf(buf, sizeof(buf), "p99 %.1f", snap_.loopP99);
    TextOutA(dc, 104, 8, buf, (int)strlen(buf));

    SetTextColor(dc, kText);
    snprintf(buf, sizeof(buf), "%.0f Hz", snap_.cadenceHz);
    TextOutA(dc, 9, 24, buf, (int)strlen(buf));

    SetTextColor(dc, snap_.dropPct > 5.0 ? kWarn : kDim);
    snprintf(buf, sizeof(buf), "%.1f%% drop", snap_.dropPct);
    TextOutA(dc, 56, 24, buf, (int)strlen(buf));

    // What the application in front is managing. Marked when inference is
    // being thinned, because that is the setting doing it rather than the
    // machine.
    if (snap_.fgFps > 1.0f) {
        SetTextColor(dc, snap_.stride > 1 ? kWarn : kDim);
        if (snap_.stride > 1)
            snprintf(buf, sizeof(buf), "%.0f fps  1/%d", snap_.fgFps, snap_.stride);
        else
            snprintf(buf, sizeof(buf), "%.0f fps", snap_.fgFps);
        TextOutA(dc, 118, 24, buf, (int)strlen(buf));
    }

    // Distance scaling replaces the confidence figure on the target row
    // rather than adding a line.
    //
    // The panel height is fixed, so an extra row would push the graph and
    // the meters past the bottom edge. Confidence is the least useful of the
    // three things on that row -- the target count and the engaged state
    // both answer questions this one does not -- and it is only displaced
    // while scaling is actually reducing something.
    // A camera on the title row, where there is space in the tall panel.
    if (snap_.capturing) {
        const int cx = 158, cy = 12;
        HBRUSH cb = CreateSolidBrush(kWarn);
        RECT body{cx - 6, cy - 4, cx + 6, cy + 4};
        RECT bump{cx - 3, cy - 7, cx + 1, cy - 3};
        FillRect(dc, &bump, cb);
        FillRect(dc, &body, cb);
        DeleteObject(cb);
        HBRUSH lens = CreateSolidBrush(kBg);
        RECT hole{cx - 2, cy - 2, cx + 2, cy + 2};
        FillRect(dc, &hole, lens);
        DeleteObject(lens);
    }

    if (snap_.sensScale >= 0.0f && snap_.sensScale < 0.995f) {
        SetTextColor(dc, snap_.sensScale < 0.5f ? kWarn : kDim);
        snprintf(buf, sizeof(buf), "%.0f%% sens", snap_.sensScale * 100.0f);
        TextOutA(dc, 50, 38, buf, (int)strlen(buf));
    } else if (snap_.topScore > 0.0f) {
        SetTextColor(dc, kAccent);
        snprintf(buf, sizeof(buf), "%.2f", snap_.topScore);
        TextOutA(dc, 56, 38, buf, (int)strlen(buf));
    }

    // Targets, confidence and engagement share one row. Engagement is a dot
    // rather than a word: it is a yes or no, and a word was costing a whole
    // line to say so.
    SetTextColor(dc, snap_.detections > 0 ? kText : kDim);
    snprintf(buf, sizeof(buf), "%d tgt", snap_.detections);
    TextOutA(dc, 9, 38, buf, (int)strlen(buf));

    {
        HBRUSH db = CreateSolidBrush(snap_.engaged ? kAccent
                                                   : RGB(0x35, 0x3B, 0x45));
        RECT dr{104, 40, 111, 47};
        FillRect(dc, &dr, db);
        DeleteObject(db);
        SetTextColor(dc, snap_.engaged ? kAccent : kDim);
        TextOutA(dc, 115, 38, snap_.engaged ? "on" : "idle",
                 snap_.engaged ? 2 : 4);
    }

    // The trace, from the same wall-clock series the Debug tab reads, so the
    // two agree and neither stretches with the frame rate.
    const int gx = 8, gy = 56, gw = kW - 16, gh = 24;
    {
        HBRUSH gb = CreateSolidBrush(RGB(0x1A, 0x1E, 0x25));
        RECT gr{gx, gy, gx + gw, gy + gh};
        FillRect(dc, &gr, gb);
        DeleteObject(gb);
    }

    if (trace_) {
        trace_->sample(spanSec_, gw / 2, cols_);
        float lo = 1e30f, hi = -1e30f;
        for (const auto& c : cols_)
            if (c.has) { lo = std::min(lo, c.mn); hi = std::max(hi, c.mx); }
        if (lo <= hi) {
            if (hi - lo < 0.25f) {
                const float m = (lo + hi) * 0.5f;
                lo = m - 0.125f; hi = m + 0.125f;
            }
            const float sx = (float)gw / (float)std::max<size_t>(1, cols_.size());

            // The spread as faint vertical ticks, the mean as a line over
            // them: a value alternating between one and eight looks
            // different from one steady at four.
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(0x2A, 0x3A, 0x3A));
            HGDIOBJ ob = SelectObject(dc, bp);
            for (size_t i = 0; i < cols_.size(); ++i) {
                if (!cols_[i].has) continue;
                const int x = gx + (int)(i * sx);
                const int y0 = gy + gh - 1 -
                    (int)((cols_[i].mx - lo) / (hi - lo) * (gh - 2));
                const int y1 = gy + gh - 1 -
                    (int)((cols_[i].mn - lo) / (hi - lo) * (gh - 2));
                MoveToEx(dc, x, y0, nullptr);
                LineTo(dc, x, y1 + 1);
            }
            SelectObject(dc, ob);
            DeleteObject(bp);

            std::vector<POINT> poly;
            poly.reserve(cols_.size());
            for (size_t i = 0; i < cols_.size(); ++i) {
                if (!cols_[i].has) continue;
                poly.push_back({gx + (int)(i * sx),
                                gy + gh - 1 -
                                (int)((cols_[i].mean - lo) / (hi - lo) * (gh - 2))});
            }
            if (poly.size() > 1) {
                HPEN gp = CreatePen(PS_SOLID, 1,
                                    snap_.dropPct > 5.0 ? kWarn : kAccent);
                HGDIOBJ opn = SelectObject(dc, gp);
                Polyline(dc, poly.data(), (int)poly.size());
                SelectObject(dc, opn);
                DeleteObject(gp);
            }

            SetTextColor(dc, RGB(0x5C, 0x64, 0x71));
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%.1f", hi);
            TextOutA(dc, gx + 2, gy - 1, lbl, (int)strlen(lbl));
        }
    }

    // --- machine load ------------------------------------------------------
    if (meters_) {
        const int mx = 8, mw = kW - 16;
        int my = gy + gh + 7;

        auto meter = [&](const char* label, float pct, COLORREF fill,
                         const char* rightText) {
            if (pct < 0.0f) return;
            const int barH = 4;
            const int lblW = 28;

            SetTextColor(dc, RGB(0x8A, 0x92, 0x9C));
            TextOutA(dc, mx, my - 4, label, (int)strlen(label));

            const int bx = mx + lblW;
            const int bw = mw - lblW - 34;

            HBRUSH bg = CreateSolidBrush(RGB(0x1A, 0x1E, 0x25));
            RECT br{bx, my, bx + bw, my + barH};
            FillRect(dc, &br, bg);
            DeleteObject(bg);

            const int filled = (int)(bw * std::clamp(pct, 0.0f, 100.0f) / 100.0f);
            if (filled > 0) {
                HBRUSH fb = CreateSolidBrush(fill);
                RECT fr{bx, my, bx + filled, my + barH};
                FillRect(dc, &fr, fb);
                DeleteObject(fb);
            }

            SetTextColor(dc, RGB(0x5C, 0x64, 0x71));
            TextOutA(dc, bx + bw + 4, my - 4, rightText, (int)strlen(rightText));
            my += 11;
        };

        char t[24];
        if (snap_.cpuPct >= 0.0f) {
            snprintf(t, sizeof(t), "%.0f%%", snap_.cpuPct);
            meter("cpu", snap_.cpuPct, snap_.cpuPct > 75.0f ? kWarn : kAccent, t);
        }
        if (snap_.gpuPct >= 0.0f) {
            snprintf(t, sizeof(t), "%.0f%%", snap_.gpuPct);
            meter("gpu", snap_.gpuPct, snap_.gpuPct > 90.0f ? kWarn : kAccent, t);
        }
        if (snap_.vramPct >= 0.0f) {
            snprintf(t, sizeof(t), "%.0f%%", snap_.vramPct);
            meter("vram", snap_.vramPct, snap_.vramPct > 90.0f ? kWarn : kAccent, t);
        }

        if (snap_.gpuMaxMHz > 0) {
            const bool low = snap_.gpuMHz < snap_.gpuMaxMHz * 3 / 4;
            SetTextColor(dc, (snap_.throttled || low) ? kWarn
                                                      : RGB(0x5C, 0x64, 0x71));
            char c[48];
            snprintf(c, sizeof(c), "%d/%d MHz  %d C",
                     snap_.gpuMHz, snap_.gpuMaxMHz, snap_.gpuTempC);
            TextOutA(dc, mx, my - 3, c, (int)strlen(c));
        }
    }
}

} // namespace lc
