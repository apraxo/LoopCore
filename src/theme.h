// theme.h -- palette, style, and the custom widgets the panel is built from.
#pragma once

#include "imgui.h"

#include <string>
#include <vector>

#include "common.h"

namespace lc::ui {

// --------------------------------------------------------------- palette
// Near-black base with a single teal accent. Everything structural is a grey;
// the accent is reserved for state that matters (active tab, filled track,
// running indicator), so it never competes with itself.
namespace col {
// Runtime values, not constants: ApplyTheme swaps the whole palette.
extern ImU32 base, panel, raised, hover, border, track;
extern ImU32 text, textDim, textFaint;
extern ImU32 accent, accentDim, warn, danger, close;
}

// Fonts, loaded in ApplyTheme. Null if the system font was unavailable.
extern ImFont* fontUI;
extern ImFont* fontSmall;
extern ImFont* fontTitle;

enum Theme {
    // Eight, which is what fits across the picker row without the swatches
    // running into the label. Graphite and Ice were the two nearest
    // neighbours of what remains, so they made way.
    ThemeMidnight = 0, ThemeAmber, ThemeIndigo, ThemeCrimson,
    ThemeViolet, ThemeForest, ThemeMatte, ThemeDaylight,
    ThemeCount
};
const char* ThemeName(int t);

// How surfaces are drawn, independent of the palette.
//
// The colour theme says what hue things are; this says whether a card looks
// printed flat, lit from above, pressed out of the panel, or made of glass.
// They are separate because someone may want a dark palette with any of
// them, and combining the two into one list would multiply the options
// without adding any.
enum UiStyle {
    StyleFlat = 0,   // the original: a border and a fill
    StyleRaised,     // skeuomorphic, lit from above with a drop shadow
    StyleSoft,       // neumorphic, pressed out of the panel it sits on
    StyleGlass,      // translucent, with a bright rim
    StyleCount
};
const char* StyleName(int s);
void  SetUiStyle(int s);
int   UiStyleNow();

// The two colours a theme is recognisable by, for a swatch.
void ThemeSwatch(int t, ImU32& background, ImU32& accent);

// A row of tappable swatches. Returns true when the selection changed.
bool ThemePicker(const char* label, int* current);

// Safe to call repeatedly; re-applies the palette and style.
void ApplyTheme(int theme = 0);

// Rebuilds the fonts and rescales the style for a given display scale, where
// 1.0 is 96 dpi. Called at startup and whenever the panel moves to a display
// with different scaling.
//
// The process is per-monitor DPI aware, which means every coordinate it
// deals with is a real pixel rather than one Windows has scaled behind its
// back. That is what keeps capture, the cursor and the overlay agreeing --
// but it also means the panel has to do its own scaling, or it comes out
// physically smaller as the display scale goes up.
void  SetUiScale(float scale);
float UiScale();

// When false, Hint() draws nothing. Controls still carry hover tooltips, so
// the explanations stay reachable without occupying permanent space.
extern bool showHints;

// ---------------------------------------------------------------- widgets

// A row of tabs. `current` is the selected index; returns true if it changed.
// `outWidth` receives the total pixel width, which the caller needs so the
// window chrome can stop treating that strip as a drag handle.
bool TabRow(const char* const* labels, int count, int* current,
            float height = 34.0f, float* outWidth = nullptr,
            float padding = 18.0f);

// Small uppercase section label with a hairline rule.
void SectionHeader(const char* text);

// Draws a faint bracket around a run of controls that belong to the toggle
// above them.
//
// Call BeginGroupBox after the toggle, EndGroupBox after its settings. The
// outline is drawn on End, because the height is only known then.
void BeginGroupBox();
void EndGroupBox();

// A spinning mark for work whose duration cannot be known.
//
// TensorRT reports nothing at all during some build phases, so a progress bar
// there would either sit still or lie. A spinner claims only that something
// is happening, which is the honest amount to claim.
void Spinner(float size = 22.0f);

// A bordered, collapsible group. Always pair with EndCard, including when
// BeginCard returns false.
// `width` of 0 fills the available space; a layout that positions cards
// itself must pass the column width, or every card spans the whole panel.
bool BeginCard(const char* title, bool defaultOpen = true, float width = 0.0f);
void EndCard();

// Height the card just drawn occupied. Measuring this from cursor movement
// does not work: the card is a child window, so the cursor afterwards says
// nothing useful about how tall it was.
float LastCardHeight();

// Fold state, so it can be restored from the config rather than reset every
// launch.
void SetCardOpen(const char* title, bool open);
// Fold state is stored against an explicit key rather than the window's own
// storage, so it survives the card's child window and cannot be confused
// between two cards that happen to share a title.
void SetCardOpenKeyed(const char* key, bool open);
bool GetCardOpenKeyed(const char* key, bool defaultOpen);
// The key BeginCard should use for the card it is about to draw.
void SetCardKeyPrefix(const char* prefix);
bool GetCardOpen(const char* title, bool defaultOpen);

// Two-column card layout. Cards between Begin and Next go left, cards
// between Next and End go right. Not nestable.
void BeginColumns(float gap = 12.0f);

// State of the header of the card most recently begun. The layout owns
// dragging, because only it knows where the other cards are.
bool LastHeaderHeld();
bool LastHeaderPressed();
float LastHeaderTop();
void NextColumn();
void EndColumns();

// All row widgets share one layout: label on the left, control on the right.
// `tip` shows on hover regardless of the showHints setting.
bool SliderInt(const char* label, int* v, int lo, int hi,
               int step = 0, const char* fmt = "%d", const char* tip = nullptr);
bool SliderFloat(const char* label, float* v, float lo, float hi,
                 const char* fmt = "%.2f", const char* tip = nullptr);

// A confidence slider whose track is coloured by how usable the value is:
// red at both extremes, accent through the middle. The shape of the problem
// is that both too low and too high are bad, which a plain track cannot say.
bool SliderConfidence(const char* label, float* v, const char* tip = nullptr);

// A slider whose track is green only near `sweet` and reddens away from it,
// so the usable range is visible instead of described in a paragraph.
// A slider whose track is not linear: the first `split` of its length
// covers lo..mid, the rest covers mid..hi.
//
// For settings where the useful range is bunched at one end and the far end
// is occasionally needed. A linear track either wastes three quarters of its
// travel or makes the common range impossible to set precisely.
bool SliderPiecewise(const char* label, float* v, float lo, float mid,
                     float hi, float split, const char* fmt,
                     const char* tip = nullptr);

bool SliderSweetSpot(const char* label, int* v, int lo, int hi, int step,
                     int sweet, float sweetWidth, const char* fmt,
                     const char* tip = nullptr);

// The opening animation: the mark draws itself, then the panel fades in.
// Returns true while it is still running.
bool DrawSplash();

// Transient message, drawn near the bottom of the panel.
void Toast(const char* text, double seconds = 3.0);
void DrawToast();

// Pill switch. Reads better than a checkbox at this density.
bool Toggle(const char* label, bool* v, const char* tip = nullptr);

// Dropdown sized to the right-hand column.
bool ComboRow(const char* label, int* v, const char* const items[], int count,
              const char* tip = nullptr);

// Click, then press any key or mouse button. Escape cancels.
bool KeybindRow(const char* label, int* vk, const char* tip = nullptr);

// Human-readable name for a virtual key code.
const char* KeyName(int vk);

// Filled accent button.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
// Outlined button.
bool GhostButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Help / minimise / maximise / close, drawn as glyphs rather than a font.
// Returns 1, 2, 3, or 4 respectively when one is pressed, else 0.
int CaptionButtons(bool isMaximized);

// Small coloured status pill, e.g. RUNNING / STOPPED.
// `spinning` turns the dot into a rotating arc, for a state that is work in
// progress rather than a condition.
// `glow` rims the pill and lights it, for a state worth spotting across the
// room rather than reading.
// `ring` is drawn around the pill when non-zero, in its own colour. It is
// separate from `colour` so a state can be marked without recolouring the
// whole thing.
void StatusPill(const char* text, ImU32 colour, bool spinning = false,
                ImU32 ring = 0);

// A green that stays legible whatever the theme is.
//
// The running indicator used the theme's accent, which is orange in Amber --
// the same colour as building. Two states that mean opposite things then
// looked identical. This picks between a bright and a deep green by how light
// the background is, because no single green clears both: bright is 8.3:1 on
// a dark panel and 2.1:1 on a light one, deep is the reverse.
ImU32 OkGreen();

// A small camera glyph, for showing that frames are being written.
//
// Drawn rather than loaded: it has to sit inside a title bar at whatever
// scale the display is set to, and a handful of rectangles scales cleanly
// where a bitmap would not.
void CameraGlyph(ImDrawList* dl, ImVec2 centre, float size, ImU32 colour);

// The loopcore mark, drawn as vectors so it stays crisp at any size.
// Proportions mirror tools/make_icon.py -- change one, change both.
// `size` is the full width of the square it occupies.
// `hoverT` runs 0 to 1 while the pointer is over the mark, and drives a
// small animation. Pass 0 for the static version.
void DrawLogo(ImDrawList* dl, ImVec2 topLeft, float size, float hoverT = 0.0f);

// A miniature box with a dot where the aim would land, so the anchor and
// offsets can be read at a glance instead of inferred from three numbers.
// `anchor` matches Config::boxAnchor; offsets are percentages.
void AnchorPreview(int anchor, float offXPct, float offYPct, float height = 62.0f);

// A scaled outline of the monitor with the two regions drawn inside it, so
// their real size is visible without switching the overlay on.
// `followX/followY` are the region centre in screen coordinates. Pass -1 to
// draw it centred, which is what the fixed mode does.
// A box tracking back and forth with a marker where the lead points.
//
// The value is in watching it react: a method that takes time to trust a
// reversal shows the marker lagging at each turn and catching up along the
// straight, which is the behaviour that matters and the hardest thing to
// judge from numbers.
// Draws a time series as a shaded band between the per-column minimum and
// maximum, with the mean over it.
//
// A single line through the mean hides exactly what matters: whether a
// figure is steady at four milliseconds or alternating between one and eight.
// The band shows the spread, the line shows the trend, and columns with no
// data break rather than being bridged by a straight segment across a gap
// that never happened.
void TraceGraph(const char* id, const std::vector<TimeSeries::Column>& cols,
                float height, float spanSec, const char* unit,
                bool warn = false);

void LeadPreview(float leadSeconds, float trust, float smoothMs,
                 bool predictionOn, float height = 74.0f);

void FovPreview(int screenW, int screenH, int fov, bool actionOn, int actionFov,
                int followX = -1, int followY = -1, float height = 96.0f);

// A compact box listing why something is not working. `severity` is 0 for
// nothing wrong, 1 for a caveat, 2 for a hard blocker.
void AlertBox(int severity, const std::vector<std::string>& lines);

// Muted helper text under a control. Suppressed entirely when showHints is
// false, so a user who knows the app can reclaim the space.
void Hint(const char* fmt, ...);

// Coloured inline notice: 0 info, 1 warning, 2 error.
void Notice(int level, const char* fmt, ...);

} // namespace lc::ui
