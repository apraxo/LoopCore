// chrome.h -- borderless window that keeps every native behaviour.
//
// The frame is removed by zeroing the non-client area in WM_NCCALCSIZE, not
// by using WS_POPUP. That distinction matters: the window keeps WS_CAPTION
// and WS_THICKFRAME, so Aero Snap, snap layouts, the minimise/restore
// animations, edge resizing, and double-click-to-maximise all keep working.
// Only the pixels are ours.
#pragma once

#include <windows.h>

namespace lc::chrome {

// Height of the draggable strip at the top, in pixels.
// Logical, at 96 dpi. Callers multiply by the display scale; the hit-testing
// below reads the live value so the two cannot drift apart.
inline constexpr int kTitleBarHeight = 38;
// Updated by the panel each frame, in real pixels, for hit testing.
void SetTitleBarHeight(int px);
int  TitleBarHeight();

// Width of the button cluster on the right: help, minimise, maximise, close.
// Excluded from the drag area so ImGui receives the clicks.
inline constexpr int kCaptionButtonW     = 38;
inline constexpr int kCaptionButtonsWidth = kCaptionButtonW * 4;

// Thickness of the invisible resize border.
inline constexpr int kResizeBorder = 6;

// Call from the window procedure before anything else. Returns true and sets
// `result` when the message was fully handled.
bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// Call once, right after CreateWindow, to drop the frame and enable the DWM
// shadow.
void Initialise(HWND hwnd);

// Not named IsMaximized: winuser.h defines that as a macro expanding to
// IsZoomed, which would rename this function out from under us and collide
// with the Win32 original.
bool Maximized(HWND hwnd);

// Carves a horizontal span out of the draggable title strip, in client
// pixels. The tabs live up there now, and a region that drags the window
// cannot also be clicked. Call once per frame with the tab strip bounds;
// pass an empty span (x0 >= x1) to clear it.
void SetNoDragSpan(int x0, int x1);

} // namespace lc::chrome
