// chrome.cpp
#include "chrome.h"

#include <dwmapi.h>
#include <windowsx.h>

namespace lc::chrome {

namespace { int g_titleBarPx = kTitleBarHeight; }

void SetTitleBarHeight(int px) { if (px > 0) g_titleBarPx = px; }
int  TitleBarHeight() { return g_titleBarPx; }

namespace {
int g_noDragX0 = 0;
int g_noDragX1 = 0;
}

void SetNoDragSpan(int x0, int x1) {
    g_noDragX0 = x0;
    g_noDragX1 = x1;
}

bool Maximized(HWND hwnd) {
    WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    return wp.showCmd == SW_SHOWMAXIMIZED;
}

void Initialise(HWND hwnd) {
    // A one-pixel top margin is enough for DWM to draw the drop shadow and
    // run the snap animations. Zero margins would lose both.
    MARGINS m{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &m);

    // Dark title-bar hint, for the brief moment before our own paint lands
    // and for the taskbar thumbnail.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                          &dark, sizeof(dark));

    // Force the frame to be recalculated now that we intercept WM_NCCALCSIZE.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOOWNERZORDER);
}

static LRESULT HitTest(HWND hwnd, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    RECT win;
    GetWindowRect(hwnd, &win);

    const int x = pt.x - win.left;
    const int y = pt.y - win.top;
    const int w = win.right - win.left;
    const int h = win.bottom - win.top;

    // A maximised window has no edges to grab.
    if (!Maximized(hwnd)) {
        const int b = kResizeBorder;
        const bool left   = x < b;
        const bool right  = x >= w - b;
        const bool top    = y < b;
        const bool bottom = y >= h - b;

        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
    }

    // The title strip, minus the buttons and minus whatever the UI has
    // reserved, drags the window. Returning HTCAPTION also gives
    // double-click-to-maximise and the system menu on right-click, for free.
    if (y < g_titleBarPx && x < w - kCaptionButtonsWidth) {
        const bool inReserved = (g_noDragX1 > g_noDragX0) &&
                                (x >= g_noDragX0) && (x < g_noDragX1);
        if (!inReserved) return HTCAPTION;
    }

    return HTCLIENT;
}

static void AdjustMaximizedRect(HWND hwnd, RECT* rect) {
    // A maximised window is deliberately sized larger than the monitor by the
    // frame thickness. Without compensating, the top and sides of our content
    // would sit off-screen.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!mon) return;
    MONITORINFO mi{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(mon, &mi)) return;
    *rect = mi.rcWork;
}

bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (msg) {

    case WM_NCCALCSIZE: {
        if (!wp) return false;
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);

        if (Maximized(hwnd)) {
            AdjustMaximizedRect(hwnd, &p->rgrc[0]);
        }
        // Leaving the rect otherwise untouched makes the client area cover
        // the whole window, which is what removes the frame.
        *result = 0;
        return true;
    }

    case WM_NCHITTEST:
        *result = HitTest(hwnd, lp);
        return true;

    case WM_NCACTIVATE:
        // Suppress the default non-client repaint, which would flash the
        // stock frame when focus changes.
        *result = DefWindowProcW(hwnd, msg, wp, -1);
        return true;

    case WM_GETMINMAXINFO: {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(mon, &mi)) {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mm->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
            mm->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
            mm->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
            mm->ptMinTrackSize.x = 480;
            mm->ptMinTrackSize.y = 360;
        }
        *result = 0;
        return true;
    }

    default:
        return false;
    }
}

} // namespace lc::chrome
