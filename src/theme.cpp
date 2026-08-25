// theme.cpp
#include "theme.h"

#include "imgui_internal.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace lc::ui {

bool showHints = true;

namespace col {
ImU32 base, panel, raised, hover, border, track;
ImU32 text, textDim, textFaint;
ImU32 accent, accentDim, warn, danger, close;
}

const char* ThemeName(int t) {
    switch (t) {
    case ThemeAmber:    return "Amber";
    case ThemeIndigo:   return "Indigo";
    case ThemeCrimson:  return "Crimson";
    case ThemeViolet:   return "Violet";
    case ThemeForest:   return "Forest";
    case ThemeMatte:    return "Matte";
    case ThemeDaylight: return "Daylight";
    default:            return "Midnight";
    }
}

// Structure stays identical across themes; only hue and contrast move. That
// keeps every widget legible without per-theme special cases.
static void ApplyStyleOnly();
namespace {
float g_uiScale = 1.0f;
int   g_uiStyle = StyleFlat;
int   g_lastTheme = 0;

// Card fold state, held here rather than in ImGui's per-window storage.
//
// It used to live in whichever window happened to be current, keyed by the
// card's title. Two faults followed. A title is not unique -- "Field of view"
// exists on both General and Visual -- so folding one folded the other. And
// the storage a card was read from was not the one it was written to, since a
// card draws into a child window of its own, which is why the state never
// survived to be saved into the config.
std::map<std::string, bool> g_cardOpen;
std::string g_cardKeyPrefix;
}
void ApplyStyleToPalette();

const char* StyleName(int s) {
    switch (s) {
    case StyleRaised: return "Raised";
    case StyleSoft:   return "Soft";
    case StyleGlass:  return "Glass";
    default:          return "Flat";
    }
}
void SetUiStyle(int s) {
    g_uiStyle = ImClamp(s, 0, (int)StyleCount - 1);
    // The palette depends on the style, so it has to be rebuilt from the
    // theme rather than adjusted twice.
    ApplyTheme(g_lastTheme);
}

namespace {

// Lighten or darken a colour, keeping its alpha.
ImU32 Shade(ImU32 c, float amount) {
    const int a = (c >> 24) & 0xFF;
    int r = (c >> 0) & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    if (amount >= 0.0f) {
        r += (int)((255 - r) * amount);
        g += (int)((255 - g) * amount);
        b += (int)((255 - b) * amount);
    } else {
        r += (int)(r * amount);
        g += (int)(g * amount);
        b += (int)(b * amount);
    }
    return IM_COL32(ImClamp(r, 0, 255), ImClamp(g, 0, 255),
                    ImClamp(b, 0, 255), a);
}

ImU32 WithAlpha(ImU32 c, int a) {
    return (c & 0x00FFFFFF) | ((ImU32)ImClamp(a, 0, 255) << 24);
}

} // namespace

// The style moves the palette as well as the shapes.
//
// The references share a common property: the surface and the background
// are nearly the same colour, and the separation comes entirely from light.
// A style that only changed borders on top of a high-contrast palette would
// not read as any of them. So each one adjusts the panel, the raised tone
// and the separators to suit, on top of whichever hue the theme chose.
void ApplyStyleToPalette() {
    switch (g_uiStyle) {
    // Nudged rather than replaced.
    //
    // The first attempt moved the palette far enough that a theme stopped
    // looking like itself once a style was picked, which defeats having the
    // two as separate choices. These are small shifts that support the
    // surface treatment without taking the colour scheme over.
    case StyleRaised:
        col::panel  = Shade(col::panel, 0.05f);
        col::border = Shade(col::border, -0.18f);
        break;
    case StyleSoft:
        // The one case that has to move meaningfully: the look is the
        // absence of contrast between a surface and its background, so the
        // panel has to meet the base most of the way.
        col::panel  = Shade(col::base, 0.03f);
        col::raised = Shade(col::base, 0.07f);
        col::border = Shade(col::base, 0.13f);
        break;
    case StyleGlass:
        col::panel  = Shade(col::panel, 0.07f);
        col::border = WithAlpha(col::accent, 80);
        break;
    default:
        break;
    }
}
int  UiStyleNow() { return g_uiStyle; }

namespace {

ImU32 Shade(ImU32 c, float amount);
ImU32 WithAlpha(ImU32 c, int a);
// Whether a surface is a container or a control, which decides how it is
// lit: cards stand above the panel, controls are cut into the card.
enum SurfaceRole { RoleCard, RoleControl };
// A soft edge band drawn just inside the shape.
//
// This is what sells raised or sunken. A single border line is a hard edge
// and reads as drawn; a few concentric lines fading out give a gradient two
// or three pixels wide, which the eye accepts as a lit curve.
//
// Only the top and bottom runs are drawn, inset past the corner radius so
// nothing pokes out of a rounded shape -- the square corners behind toggles
// came from a fill that could not round, and there is no sense replacing it
// with a band that has the same fault.
static void EdgeBand(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                     ImU32 colour, int alpha, float depth, bool topLit) {
    // Two lines, not four.
    //
    // The band was three or four steps deep on every surface, and there are
    // now a surface behind every toggle, slider track, dropdown and keybind
    // on screen. At two hundred-odd widgets that is most of a thousand extra
    // primitives per frame for a gradient two pixels wide.
    //
    // Two steps still reads as a soft edge and costs half as much. The depth
    // argument is kept because callers pass it, and clamping here rather
    // than at each call site means there is one place to change it.
    const int steps = std::clamp((int)depth, 1, 2);
    for (int i = 0; i < steps; ++i) {
        const float t = (float)i / (float)steps;
        const int fade = (int)(alpha * (1.0f - t) * (1.0f - t));
        if (fade <= 0) continue;
        const float in = (float)i;
        const ImVec2 p0(a.x + in, a.y + in);
        const ImVec2 p1(b.x - in, b.y - in);
        // Nothing narrower than a few pixels can show a gradient, and a
        // slider track is exactly that. Drawing one there costs the same as
        // drawing one on a card and shows nothing.
        if (p1.x - p0.x < 4.0f || p1.y - p0.y < 8.0f) break;

        // Inset by the radius so the run stops where the curve begins.
        const float r = std::min(rounding, (p1.x - p0.x) * 0.5f);
        const ImU32 up   = WithAlpha(IM_COL32(255, 255, 255, 255), fade);
        const ImU32 down = WithAlpha(IM_COL32(0, 0, 0, 255), fade);
        dl->AddLine(ImVec2(p0.x + r, p0.y), ImVec2(p1.x - r, p0.y),
                    topLit ? up : down, 1.0f);
        dl->AddLine(ImVec2(p0.x + r, p1.y), ImVec2(p1.x - r, p1.y),
                    topLit ? down : up, 1.0f);
        (void)colour;
    }
}

void DrawSurface(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                 bool hovered, bool pressed, int role) {
    // The face keeps the panel colour; only the edges are lit.
    //
    // Two things were wrong with the gradient version. AddRectFilledMultiColor
    // takes no rounding, so every surface drawn with it had square corners --
    // that is the box visible behind each toggle and slider track. And
    // tinting the whole face made cards read as a different colour rather
    // than as a raised panel, which is not what a raised surface looks like:
    // a real one is the same material, lit differently at its edges.
    //
    // So the fill is flat and the style lives entirely in the band around
    // it. That fixes the corners and the colour shift together, because both
    // came from the same call.
    const bool sunken = (role == RoleControl) || pressed;

    // The radius cannot exceed half the shorter side.
    //
    // A slider track is six pixels tall and asks for a radius of three,
    // which is fine -- but a toggle pill and some rows ask for more than
    // half their height, and ImGui does not draw a rounded rect whose radius
    // is larger than the shape. The corners come out square, which is the
    // hard edge that appears on the sunken controls under every style except
    // Flat. Clamping here fixes all of them at once rather than at each
    // caller.
    const float halfShort = std::min(b.x - a.x, b.y - a.y) * 0.5f;
    rounding = std::clamp(rounding, 0.0f, std::max(0.0f, halfShort));

    // A highlight or a band on something only a few pixels tall shows
    // nothing and costs the same as one on a card.
    const bool thin = (b.y - a.y) < 12.0f;

    switch (g_uiStyle) {
    case StyleRaised: {
        dl->AddRectFilled(a, b, sunken ? Shade(col::base, -0.06f) : col::panel,
                          rounding);
        if (!thin) EdgeBand(dl, a, b, rounding, 0, sunken ? 120 : 100, 3.0f, !sunken);
        dl->AddRect(a, b, Shade(col::border, sunken ? -0.30f : -0.10f),
                    rounding, 0, 1.0f);
        if (hovered && sunken)
            dl->AddRect(a, b, WithAlpha(col::accent, 80), rounding, 0, 1.0f);
        break;
    }
    case StyleSoft: {
        // Same colour as the background, separated only by the band. The
        // absence of contrast is the look; a border would undo it.
        dl->AddRectFilled(a, b, sunken ? Shade(col::panel, -0.04f) : col::panel,
                          rounding);
        if (!thin) EdgeBand(dl, a, b, rounding, 0, sunken ? 95 : 70, 4.0f, !sunken);
        if (hovered)
            dl->AddRect(a, b, WithAlpha(col::accent, 55), rounding, 0, 1.0f);
        break;
    }
    case StyleGlass: {
        // Translucent rather than repainted. There is no blur available, so
        // the impression comes from letting the background through and
        // putting a bright rim on it -- which is why the fill is alpha and
        // not a lighter grey.
        dl->AddRectFilled(a, b,
            WithAlpha(Shade(col::panel, sunken ? -0.04f : 0.06f),
                      sunken ? 200 : 150), rounding);
        // A single soft highlight along the top, rounded like everything
        // else, standing in for the sheen on glass.
        if (!thin)
            dl->AddRectFilled(a, ImVec2(b.x, a.y + std::min(10.0f, (b.y - a.y) * 0.4f)),
                              IM_COL32(255, 255, 255, sunken ? 6 : 16), rounding);
        if (!thin) EdgeBand(dl, a, b, rounding, 0, sunken ? 60 : 40, 2.0f, !sunken);
        dl->AddRect(a, b, WithAlpha(col::accent,
                                    sunken ? 130 : (hovered ? 120 : 70)),
                    rounding, 0, 1.2f);
        break;
    }
    default:
        dl->AddRectFilled(a, b, sunken ? col::track : col::panel, rounding);
        dl->AddRect(a, b, col::border, rounding, 0, 1.0f);
        break;
    }
}

} // namespace

// Every dark theme is Midnight's neutral ramp with a few points of tint.
//
// The earlier palettes coloured every channel, so the greys themselves were
// brown or red or purple. Nothing then reads as neutral, the accent has
// nothing to sit against, and the whole thing looks washed out -- which is
// why Midnight was the only one that felt clean. Keeping the structure and
// changing only the accent, with barely enough tint to tell them apart,
// gives every theme the contrast Midnight has.
static void SetPalette(int theme) {
    switch (theme) {
    case ThemeAmber:
        col::base   = IM_COL32(0x16, 0x15, 0x15, 0xFF);
        col::panel  = IM_COL32(0x1D, 0x1D, 0x1F, 0xFF);
        col::raised = IM_COL32(0x25, 0x26, 0x2A, 0xFF);
        col::hover  = IM_COL32(0x2F, 0x31, 0x37, 0xFF);
        col::border = IM_COL32(0x2B, 0x2D, 0x33, 0xFF);
        col::track  = IM_COL32(0x2D, 0x30, 0x36, 0xFF);
        col::text   = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
        col::textDim= IM_COL32(0x98, 0xA1, 0xB0, 0xFF);
        col::textFaint= IM_COL32(0x5E, 0x67, 0x76, 0xFF);
        col::accent = IM_COL32(0xF0, 0xA9, 0x3C, 0xFF);
        col::accentDim = IM_COL32(0x9A, 0x6B, 0x24, 0xFF);
        break;
    case ThemeCrimson:
        col::base   = IM_COL32(0x16, 0x11, 0x17, 0xFF);
        col::panel  = IM_COL32(0x1D, 0x19, 0x21, 0xFF);
        col::raised = IM_COL32(0x25, 0x22, 0x2C, 0xFF);
        col::hover  = IM_COL32(0x2F, 0x2D, 0x39, 0xFF);
        col::border = IM_COL32(0x2B, 0x29, 0x35, 0xFF);
        col::track  = IM_COL32(0x2D, 0x2C, 0x38, 0xFF);
        col::text   = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
        col::textDim= IM_COL32(0x98, 0xA1, 0xB0, 0xFF);
        col::textFaint= IM_COL32(0x5E, 0x67, 0x76, 0xFF);
        col::accent = IM_COL32(0xE8, 0x5A, 0x6E, 0xFF);
        col::accentDim = IM_COL32(0x93, 0x37, 0x46, 0xFF);
        break;
    case ThemeViolet:
        col::base   = IM_COL32(0x13, 0x11, 0x1E, 0xFF);
        col::panel  = IM_COL32(0x1A, 0x19, 0x28, 0xFF);
        col::raised = IM_COL32(0x22, 0x22, 0x33, 0xFF);
        col::hover  = IM_COL32(0x2C, 0x2D, 0x40, 0xFF);
        col::border = IM_COL32(0x28, 0x29, 0x3C, 0xFF);
        col::track  = IM_COL32(0x2A, 0x2C, 0x3F, 0xFF);
        col::text   = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
        col::textDim= IM_COL32(0x98, 0xA1, 0xB0, 0xFF);
        col::textFaint= IM_COL32(0x5E, 0x67, 0x76, 0xFF);
        col::accent = IM_COL32(0xA9, 0x8B, 0xFF, 0xFF);
        col::accentDim = IM_COL32(0x66, 0x52, 0xA8, 0xFF);
        break;
    case ThemeForest:
        col::base   = IM_COL32(0x0F, 0x17, 0x17, 0xFF);
        col::panel  = IM_COL32(0x16, 0x1F, 0x21, 0xFF);
        col::raised = IM_COL32(0x1E, 0x28, 0x2C, 0xFF);
        col::hover  = IM_COL32(0x28, 0x33, 0x39, 0xFF);
        col::border = IM_COL32(0x24, 0x2F, 0x35, 0xFF);
        col::track  = IM_COL32(0x26, 0x32, 0x38, 0xFF);
        col::text   = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
        col::textDim= IM_COL32(0x98, 0xA1, 0xB0, 0xFF);
        col::textFaint= IM_COL32(0x5E, 0x67, 0x76, 0xFF);
        col::accent = IM_COL32(0x5A, 0xD6, 0x9A, 0xFF);
        col::accentDim = IM_COL32(0x33, 0x86, 0x5E, 0xFF);
        break;
    case ThemeMatte:
        // Near-black with almost no colour in it. The separations are tiny
        // on purpose: at this depth a few levels of grey is all it takes,
        // and anything more reads as a lighter theme rather than a matte one.
        col::base   = IM_COL32(0x05, 0x05, 0x06, 0xFF);
        col::panel  = IM_COL32(0x0B, 0x0B, 0x0C, 0xFF);
        col::raised = IM_COL32(0x12, 0x12, 0x14, 0xFF);
        col::hover  = IM_COL32(0x1A, 0x1A, 0x1C, 0xFF);
        col::border = IM_COL32(0x1E, 0x1E, 0x21, 0xFF);
        col::track  = IM_COL32(0x1F, 0x1F, 0x22, 0xFF);
        col::text   = IM_COL32(0xE8, 0xE8, 0xEA, 0xFF);
        col::textDim= IM_COL32(0x8A, 0x8A, 0x8E, 0xFF);
        col::textFaint = IM_COL32(0x55, 0x55, 0x59, 0xFF);
        col::accent = IM_COL32(0xD8, 0xD8, 0xDC, 0xFF);
        col::accentDim = IM_COL32(0x6E, 0x6E, 0x73, 0xFF);
        break;
    case ThemeDaylight:
        // The one light theme. Every relationship is inverted rather than
        // brightened: text goes dark, panels go above the base rather than
        // below it, and the accent darkens so it still reads on white.
        col::base   = IM_COL32(0xEF, 0xF1, 0xF4, 0xFF);
        col::panel  = IM_COL32(0xFA, 0xFB, 0xFC, 0xFF);
        col::raised = IM_COL32(0xE7, 0xEA, 0xEE, 0xFF);
        col::hover  = IM_COL32(0xDC, 0xE0, 0xE6, 0xFF);
        col::border = IM_COL32(0xCE, 0xD3, 0xDA, 0xFF);
        col::track  = IM_COL32(0xD5, 0xDA, 0xE1, 0xFF);
        col::text   = IM_COL32(0x1A, 0x1E, 0x24, 0xFF);
        col::textDim= IM_COL32(0x5A, 0x63, 0x6E, 0xFF);
        col::textFaint = IM_COL32(0x8B, 0x94, 0x9F, 0xFF);
        col::accent = IM_COL32(0x0E, 0x8F, 0x74, 0xFF);
        col::accentDim = IM_COL32(0x7B, 0xC4, 0xB2, 0xFF);
        break;
    case ThemeIndigo:
        col::base   = IM_COL32(0x0F, 0x12, 0x1E, 0xFF);
        col::panel  = IM_COL32(0x16, 0x1A, 0x28, 0xFF);
        col::raised = IM_COL32(0x1E, 0x23, 0x33, 0xFF);
        col::hover  = IM_COL32(0x28, 0x2E, 0x40, 0xFF);
        col::border = IM_COL32(0x24, 0x2A, 0x3C, 0xFF);
        col::track  = IM_COL32(0x26, 0x2D, 0x3F, 0xFF);
        col::text   = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
        col::textDim= IM_COL32(0x98, 0xA1, 0xB0, 0xFF);
        col::textFaint = IM_COL32(0x5E, 0x67, 0x76, 0xFF);
        col::accent = IM_COL32(0x7C, 0x9B, 0xFF, 0xFF);
        col::accentDim = IM_COL32(0x4A, 0x5E, 0xA8, 0xFF);
        break;
    default:
        col::base   = IM_COL32(0x0E, 0x10, 0x13, 0xFF);
        col::panel  = IM_COL32(0x15, 0x18, 0x1D, 0xFF);
        col::raised = IM_COL32(0x1C, 0x20, 0x27, 0xFF);
        col::hover  = IM_COL32(0x23, 0x28, 0x31, 0xFF);
        col::border = IM_COL32(0x28, 0x2D, 0x36, 0xFF);
        col::track  = IM_COL32(0x2A, 0x30, 0x3A, 0xFF);
        col::text   = IM_COL32(0xE6, 0xE9, 0xEE, 0xFF);
        col::textDim= IM_COL32(0x8B, 0x93, 0xA1, 0xFF);
        col::textFaint = IM_COL32(0x5C, 0x64, 0x71, 0xFF);
        col::accent = IM_COL32(0x4F, 0xC7, 0xAC, 0xFF);
        col::accentDim = IM_COL32(0x35, 0x8C, 0x79, 0xFF);
        break;
    }
    // Warning and danger have to stay legible against the base, so the light
    // theme gets darker versions rather than the same ones.
    if (theme == ThemeDaylight) {
        col::warn   = IM_COL32(0xB0, 0x6E, 0x00, 0xFF);
        col::danger = IM_COL32(0xC0, 0x2A, 0x2A, 0xFF);
    } else {
        col::warn   = IM_COL32(0xE8, 0xA8, 0x4C, 0xFF);
        col::danger = IM_COL32(0xE0, 0x63, 0x60, 0xFF);
    }
    col::close  = IM_COL32(0xC4, 0x2B, 0x1C, 0xFF);
}

ImFont* fontUI    = nullptr;
ImFont* fontSmall = nullptr;
ImFont* fontTitle = nullptr;

static ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// One column split for every row widget, so labels and controls line up
// down the whole panel instead of drifting per-widget.
// The label gutter shrinks with the panel, so a narrow card still leaves a
// usable control instead of a two-pixel slider.
// Card drag state. Declared here because BeginCard, which is above the
// column helpers, writes to it.
namespace {
float g_lastCardH  = 0.0f;
bool  g_hdrHeld    = false;
bool  g_hdrPressed = false;
float g_hdrTop     = 0.0f;
}

static float LabelWidth() {
    const float avail = ImGui::GetContentRegionAvail().x;
    return ImClamp(avail * 0.42f, 66.0f, 132.0f);
}

static void TipOnHover(const char* tip) {
    if (!tip || !*tip) return;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
    ImGui::TextUnformatted(tip);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

void ThemeSwatch(int t, ImU32& background, ImU32& accent) {
    // Applying the palette to read two colours out would disturb the live
    // one, so the swatch is built from a scratch copy instead.
    const ImU32 b0 = col::base,   p0 = col::panel,   r0 = col::raised;
    const ImU32 h0 = col::hover,  bd0 = col::border, t0 = col::track;
    const ImU32 x0 = col::text,   d0 = col::textDim, f0 = col::textFaint;
    const ImU32 a0 = col::accent, ad0 = col::accentDim;

    SetPalette(t);
    background = col::panel;
    accent     = col::accent;

    col::base = b0; col::panel = p0; col::raised = r0;
    col::hover = h0; col::border = bd0; col::track = t0;
    col::text = x0; col::textDim = d0; col::textFaint = f0;
    col::accent = a0; col::accentDim = ad0;
}

bool ThemePicker(const char* label, int* current) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rowH = 22.0f;

    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, p.y + (rowH - ls.y) * 0.5f), col::textDim, label);

    const float sw = 22.0f, gap = 6.0f;
    const float full = ImGui::GetContentRegionAvail().x;
    float x = p.x + full - (sw + gap) * ThemeCount + gap;

    bool changed = false;
    for (int i = 0; i < ThemeCount; ++i) {
        ImU32 bg = 0, ac = 0;
        ThemeSwatch(i, bg, ac);

        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##sw", ImVec2(sw, rowH));
        const bool hot = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && *current != i) { *current = i; changed = true; }
        if (hot && ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(ThemeName(i));
            ImGui::EndTooltip();
        }
        ImGui::PopID();

        // Panel on one diagonal, accent on the other: enough to tell the
        // themes apart at a glance without a preview window.
        const ImVec2 a(x, p.y + (rowH - sw) * 0.5f);
        const ImVec2 b(x + sw, a.y + sw);
        // Clipped to the rounded shape.
        //
        // The accent half was a bare triangle with square corners drawn over
        // a rounded fill, so its point pushed out past the border on the two
        // corners it touched. Clipping to the swatch means the corner radius
        // is decided once, by the fill, and everything after it obeys.
        const float rr = 5.0f;
        dl->AddRectFilled(a, b, bg, rr);
        // The accent half drawn as a rounded shape, not clipped to a
        // rectangle.
        //
        // A clip rect has square corners, so clipping a triangle to one
        // still let it fill the swatch's rounded corners -- the overflow was
        // never removed, only moved. Building the half as a path that
        // follows the same radius is what actually keeps it inside.
        dl->PathClear();
        dl->PathLineTo(ImVec2(b.x - rr, a.y));
        dl->PathArcTo(ImVec2(b.x - rr, a.y + rr), rr, -IM_PI * 0.5f, 0.0f, 8);
        dl->PathLineTo(ImVec2(b.x, b.y - rr));
        dl->PathArcTo(ImVec2(b.x - rr, b.y - rr), rr, 0.0f, IM_PI * 0.5f, 8);
        dl->PathLineTo(ImVec2(a.x + rr, b.y));
        dl->PathArcTo(ImVec2(a.x + rr, b.y - rr), rr, IM_PI * 0.5f, IM_PI, 8);
        dl->PathFillConvex(ac);
        // Redrawn over the clip so the rounded edge is clean where the
        // triangle met it.
        dl->AddRect(a, b, (*current == i) ? col::text
                                          : (hot ? col::textDim : col::border),
                    rr, 0, (*current == i) ? 2.0f : 1.0f);
        x += sw + gap;
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 4.0f));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

void ApplyTheme(int theme) {
    g_lastTheme = theme;
    SetPalette(theme);
    // The style shifts the palette the theme just set, rather than being a
    // second set of colours: every combination of the two has to work.
    ApplyStyleToPalette();
    ImGuiIO& io = ImGui::GetIO();

    // The stock bitmap font is the single biggest thing making an ImGui panel
    // look unfinished. Segoe UI is present on every supported Windows.
    const char* face = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (fontUI) { ApplyStyleOnly(); return; }

    // Sizes are multiplied by the display scale rather than left fixed.
    // Under per-monitor awareness these are physical pixels, so a fixed 16
    // is two thirds the intended size at 150% scaling.
    const float k = g_uiScale;
    fontUI    = io.Fonts->AddFontFromFileTTF(face, 16.0f * k);
    fontSmall = io.Fonts->AddFontFromFileTTF(face, 13.0f * k);
    fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf",
                                             15.0f * k);
    if (!fontUI) {                       // font missing: fall back quietly
        fontUI = io.Fonts->AddFontDefault();
        fontSmall = fontTitle = fontUI;
    }
    if (!fontSmall) fontSmall = fontUI;
    if (!fontTitle) fontTitle = fontUI;
    io.FontDefault = fontUI;

    ApplyStyleOnly();
}

float UiScale() { return g_uiScale; }

void SetUiScale(float scale) {
    // Clamped: below 1 nothing is gained and text stops being legible, and
    // beyond 3 no Windows scaling setting goes.
    scale = ImClamp(scale, 1.0f, 3.0f);
    if (fabsf(scale - g_uiScale) < 0.01f) return;
    g_uiScale = scale;

    // The atlas has to be rebuilt: a font rasterised at one size and then
    // stretched is exactly the blurriness this is meant to avoid.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    fontUI = fontSmall = fontTitle = nullptr;

    const char* face = "C:\\Windows\\Fonts\\segoeui.ttf";
    fontUI    = io.Fonts->AddFontFromFileTTF(face, 16.0f * g_uiScale);
    fontSmall = io.Fonts->AddFontFromFileTTF(face, 13.0f * g_uiScale);
    fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf",
                                             15.0f * g_uiScale);
    if (!fontUI) {
        fontUI = io.Fonts->AddFontDefault();
        fontSmall = fontTitle = fontUI;
    }
    if (!fontSmall) fontSmall = fontUI;
    if (!fontTitle) fontTitle = fontUI;
    io.FontDefault = fontUI;
    io.Fonts->Build();

    ApplyStyleOnly();
}

static void ApplyStyleOnly() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();          // from defaults, so scaling never compounds
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 8.0f;
    s.TabRounding       = 5.0f;

    s.WindowPadding     = ImVec2(0, 0);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(8, 8);
    s.ItemInnerSpacing  = ImVec2(6, 6);
    s.ScrollbarSize     = 10.0f;
    s.GrabMinSize       = 12.0f;

    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = V(col::base);
    c[ImGuiCol_ChildBg]             = V(col::panel);
    c[ImGuiCol_PopupBg]             = V(col::raised);
    c[ImGuiCol_Border]              = V(col::border);
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text]                = V(col::text);
    c[ImGuiCol_TextDisabled]        = V(col::textDim);

    // Text fields and other plain ImGui frames.
    //
    // These cannot go through DrawSurface -- ImGui draws them itself -- but
    // they can at least sit at the right depth for the chosen style, so a
    // text box does not look raised on a theme where every other control is
    // recessed. Recessed styles darken the frame; the flat one leaves it as
    // it was.
    {
        const bool recessed = (g_uiStyle == StyleRaised ||
                               g_uiStyle == StyleSoft);
        const ImU32 fb = recessed ? Shade(col::base, -0.04f) : col::raised;
        c[ImGuiCol_FrameBg]         = V(fb);
        c[ImGuiCol_FrameBgHovered]  = V(col::hover);
        c[ImGuiCol_FrameBgActive]   = V(col::hover);
    }

    c[ImGuiCol_Button]              = V(col::raised);
    c[ImGuiCol_ButtonHovered]       = V(col::hover);
    c[ImGuiCol_ButtonActive]        = V(col::border);

    c[ImGuiCol_Header]              = V(col::raised);
    c[ImGuiCol_HeaderHovered]       = V(col::hover);
    c[ImGuiCol_HeaderActive]        = V(col::hover);

    c[ImGuiCol_SliderGrab]          = V(col::accent);
    c[ImGuiCol_SliderGrabActive]    = V(col::accent);
    c[ImGuiCol_CheckMark]           = V(col::accent);

    c[ImGuiCol_Separator]           = V(col::border);
    c[ImGuiCol_SeparatorHovered]    = V(col::accentDim);
    c[ImGuiCol_SeparatorActive]     = V(col::accent);

    c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]       = V(col::border);
    c[ImGuiCol_ScrollbarGrabHovered]= V(col::hover);
    c[ImGuiCol_ScrollbarGrabActive] = V(col::accentDim);

    c[ImGuiCol_TableHeaderBg]       = V(col::raised);
    c[ImGuiCol_TableBorderStrong]   = V(col::border);
    c[ImGuiCol_TableBorderLight]    = V(col::border);
    c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1, 1, 1, 0.018f);

    c[ImGuiCol_PlotLines]           = V(col::accent);
    c[ImGuiCol_PlotLinesHovered]    = V(col::accent);

    // Padding, spacing, rounding and every other metric, in one step. Done
    // last so it applies to the values set above rather than being undone
    // by them, and from a fresh ImGuiStyle so repeated calls cannot compound.
    if (g_uiScale != 1.0f) s.ScaleAllSizes(g_uiScale);
}

// ---------------------------------------------------------------- cards

bool BeginCard(const char* title, bool defaultOpen, float width) {
    // Read from the keyed store, which outlives this window and is unique
    // per tab. The window's own storage was neither.
    const std::string key = g_cardKeyPrefix + title;
    bool open = GetCardOpenKeyed(key.c_str(), defaultOpen);

    // The child itself is drawn transparent; the surface underneath is
    // painted afterwards, because the card's height is only known once its
    // contents have been laid out.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 11));

    ImGui::BeginChild(title, ImVec2(width, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    // Reserved before anything else draws into it, so the surface lands
    // behind the contents rather than over them.
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 20.0f;
    const float chevW = 26.0f;

    // The title is a drag handle, not a toggle. Collapsing on a click of the
    // whole header made reordering impossible: every drag started by
    // folding the card away.
    // The title is a drag handle. The layout above decides what a drag
    // means; all this does is report that one is happening, because only the
    // layout knows where the other cards sit.
    ImGui::InvisibleButton("##carddrag", ImVec2(ImMax(1.0f, w - chevW), rowH));
    const bool titleHot = ImGui::IsItemHovered();
    g_hdrHeld    = ImGui::IsItemActive();
    g_hdrPressed = ImGui::IsItemClicked();
    g_hdrTop     = p.y;

    ImGui::SetCursorScreenPos(ImVec2(p.x + w - chevW, p.y));
    ImGui::InvisibleButton("##cardchev", ImVec2(chevW, rowH));
    const bool chevHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        open = !open;
        SetCardOpenKeyed(key.c_str(), open);
    }

    ImGui::PushFont(fontTitle);
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(p.x, p.y + (rowH - ts.y) * 0.5f),
                (titleHot || chevHot) ? col::text : col::textDim, title);
    ImGui::PopFont();

    // Grip dots, so the title reads as draggable rather than decorative.
    if (titleHot) {
        const float gx = p.x + ts.x + 10.0f;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                dl->AddCircleFilled(ImVec2(gx + c * 4.0f,
                                           p.y + rowH * 0.5f - 2.0f + r * 4.0f),
                                    1.0f, col::textFaint, 6);
    }

    const float cx = p.x + w - 8.0f, cy = p.y + rowH * 0.5f;
    const ImU32 cc = chevHot ? col::text : col::textFaint;
    if (open) {
        dl->AddLine(ImVec2(cx - 9, cy - 2), ImVec2(cx - 4.5f, cy + 3), cc, 1.4f);
        dl->AddLine(ImVec2(cx - 4.5f, cy + 3), ImVec2(cx, cy - 2), cc, 1.4f);
    } else {
        dl->AddLine(ImVec2(cx - 7, cy - 5), ImVec2(cx - 2, cy), cc, 1.4f);
        dl->AddLine(ImVec2(cx - 2, cy), ImVec2(cx - 7, cy + 5), cc, 1.4f);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    if (open) ImGui::Dummy(ImVec2(0, 3));
    return open;
}

void EndCard() {
    // The surface goes on the lower channel, which is drawn first.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 b = ImVec2(a.x + ImGui::GetWindowWidth(),
                                a.y + ImGui::GetWindowHeight());
        dl->ChannelsSetCurrent(0);
        DrawSurface(dl, a, b, 8.0f,
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows),
                    false, RoleCard);
        dl->ChannelsMerge();
    }

    ImGui::EndChild();
    // The child is the last item at this point, so its rectangle is the
    // card's real height. Taken before anything else is submitted.
    g_lastCardH = ImGui::GetItemRectSize().y;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, 8));
}

float LastCardHeight() { return g_lastCardH; }

// Fold state, held here rather than in ImGui's per-window storage.
//
// It used to live in whichever window happened to be current, keyed by the
// card's title. Two problems followed. A title is not unique -- "Field of
// view" exists on both General and Visual -- so folding one folded the other.
// And the storage a card is read from is not the one it is written to, since
// a card draws into a child window of its own, which is why the state never
// survived to be saved.
//
// A plain map keyed by tab and title fixes both, and makes the lifetime
// obvious rather than dependent on which window is current.
void SetCardKeyPrefix(const char* prefix) {
    g_cardKeyPrefix = prefix ? prefix : "";
}

void SetCardOpenKeyed(const char* key, bool open) {
    if (key) g_cardOpen[key] = open;
}

bool GetCardOpenKeyed(const char* key, bool defaultOpen) {
    if (!key) return defaultOpen;
    auto it = g_cardOpen.find(key);
    return it == g_cardOpen.end() ? defaultOpen : it->second;
}

void SetCardOpen(const char* title, bool open) {
    SetCardOpenKeyed((g_cardKeyPrefix + title).c_str(), open);
}

bool GetCardOpen(const char* title, bool defaultOpen) {
    return GetCardOpenKeyed((g_cardKeyPrefix + title).c_str(), defaultOpen);
}

// -------------------------------------------------------------- columns

namespace {
float g_colWidth  = 0.0f;
float g_colGap    = 12.0f;
bool  g_singleCol = false;
}

// Below this, two columns are narrower than the label gutter plus a usable
// control, and everything starts overlapping. One column is simply correct
// at that size.
static constexpr float kTwoColumnMin = 600.0f;

void BeginColumns(float gap) {
    g_colGap = gap;
    const float avail = ImGui::GetContentRegionAvail().x;
    g_singleCol = avail < kTwoColumnMin;
    g_colWidth = g_singleCol ? avail : (avail - gap) * 0.5f;
    ImGui::BeginChild("##col_left", ImVec2(g_colWidth, 0),
                      ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
}

void NextColumn() {
    if (g_singleCol) return;    // keep filling the same column
    ImGui::EndChild();
    ImGui::SameLine(0.0f, g_colGap);
    ImGui::BeginChild("##col_right", ImVec2(g_colWidth, 0),
                      ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
}

void EndColumns() {
    ImGui::EndChild();
}

bool  LastHeaderHeld()    { return g_hdrHeld; }
bool  LastHeaderPressed() { return g_hdrPressed; }
float LastHeaderTop()     { return g_hdrTop; }

// ----------------------------------------------------------------- tabs

bool TabRow(const char* const* labels, int count, int* current,
            float height, float* outWidth, float padding)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    bool changed = false;
    float x = origin.x;

    // Recorded during the pass, drawn afterwards, so the pill is not painted
    // over by the tabs that follow it.
    float activeX = origin.x, activeW = 0.0f;
    ImVec2 activeTs(0, 0);

    for (int i = 0; i < count; ++i) {
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        // Padding is supplied by the caller, which shrinks it when the bar
        // is running out of room rather than letting anything overlap.
        const float w = ts.x + padding;

        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::InvisibleButton(labels[i], ImVec2(w, height));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && *current != i) { *current = i; changed = true; }

        const bool active = (*current == i);
        const ImU32 tc = (active || hovered) ? col::text : col::textDim;

        dl->AddText(ImVec2(x + padding * 0.5f,
                           origin.y + (height - ts.y) * 0.5f), tc, labels[i]);

        // The active pill is drawn once, afterwards, at an animated
        // position: drawing it here would put it under the following tabs
        // and make it jump between them rather than travel.
        if (active) {
            activeX = x;
            activeW = w;
            activeTs = ts;
        }
        x += w;
    }

    // The pill glides to the selected tab rather than reappearing on it.
    // Travel is what tells the eye the two are the same object.
    if (activeW > 0.0f) {
        static float slideX = -1.0f, slideW = 0.0f, vX = 0.0f, vW = 0.0f;
        const float dt = ImGui::GetIO().DeltaTime;
        if (slideX < 0.0f) { slideX = activeX; slideW = activeW; }

        auto spring = [&](float& p, float& v, float target) {
            if (dt <= 0.0f) return;
            const float d = ImMin(dt, 0.05f);
            const float k = 22.0f;
            v += (-2.0f * k * v - k * k * (p - target)) * d;
            p += v * d;
            if (ImFabs(p - target) < 0.2f && ImFabs(v) < 2.0f) { p = target; v = 0.0f; }
        };
        spring(slideX, vX, activeX);
        spring(slideW, vW, activeW);

        const float y0 = origin.y + 3.0f;
        const float y1 = origin.y + height - 2.0f;
        const ImVec2 a0(slideX + 1.0f, y0), a1(slideX + slideW - 1.0f, y1);
        dl->AddRectFilled(a0, a1, col::panel, 6.0f);
        dl->AddRect(a0, a1, col::border, 6.0f, 0, 1.0f);
        dl->AddRectFilled(ImVec2(slideX + 7.0f, y1 - 3.0f),
                          ImVec2(slideX + slideW - 7.0f, y1 - 1.0f),
                          col::accent, 1.0f);
        // The label is redrawn over the pill, at the tab's real position, so
        // it stays legible while the pill is still catching up.
        dl->AddText(ImVec2(activeX + padding * 0.5f,
                           origin.y + (height - activeTs.y) * 0.5f),
                    col::text, labels[*current]);
    }

    if (outWidth) *outWidth = x - origin.x;

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

// -------------------------------------------------------------- headings

void Spinner(float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = size * 0.5f;
    const ImVec2 c(p.x + r, p.y + r);
    const double t = ImGui::GetTime();

    // The loopcore mark: three arcs at 120 degrees, rotating together, each
    // fading behind the one in front. Drawn rather than loaded, so it scales
    // and tints with the theme instead of needing an asset per size.
    for (int i = 0; i < 3; ++i) {
        const float base = (float)(t * 2.6) + (float)i * (IM_PI * 2.0f / 3.0f);
        const int alpha = 210 - i * 55;
        dl->PathClear();
        dl->PathArcTo(c, r - 2.0f, base, base + 1.5f, 16);
        dl->PathStroke((col::accent & 0x00FFFFFF) | ((ImU32)alpha << 24),
                       0, 2.4f);
    }
    ImGui::Dummy(ImVec2(size, size));
}

namespace { std::vector<ImVec2> g_groupStack; }

void BeginGroupBox() {
    ImGui::Indent(8.0f);
    g_groupStack.push_back(ImGui::GetCursorScreenPos());
    ImGui::Dummy(ImVec2(0, 2));
}

void EndGroupBox() {
    if (g_groupStack.empty()) { ImGui::Unindent(8.0f); return; }
    const ImVec2 top = g_groupStack.back();
    g_groupStack.pop_back();
    ImGui::Dummy(ImVec2(0, 2));
    const float bottom = ImGui::GetCursorScreenPos().y;
    ImGui::Unindent(8.0f);

    // Drawn behind what it contains, so the controls stay legible over it.
    // A faint bracket rather than a full box: the point is to say "these
    // belong to the switch above", which a hint of an edge does without
    // turning every group into another card.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = top.x - 6.0f;
    const float x1 = x0 + ImGui::GetContentRegionAvail().x + 4.0f;
    dl->AddRectFilled(ImVec2(x0, top.y), ImVec2(x1, bottom),
                      WithAlpha(col::accent, 10), 6.0f);
    dl->AddRect(ImVec2(x0, top.y), ImVec2(x1, bottom),
                WithAlpha(col::accent, 55), 6.0f, 0, 1.0f);
    ImGui::Dummy(ImVec2(0, 2));
}

void SectionHeader(const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fontSmall);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(p, col::textFaint, text);
    dl->AddLine(ImVec2(p.x + ts.x + 10.0f, p.y + ts.y * 0.5f),
                ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + ts.y * 0.5f),
                col::border, 1.0f);
    ImGui::Dummy(ImVec2(0, ts.y));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
}

// --------------------------------------------------------------- sliders

// The smallest change the displayed value can show. "%.2f" steps by 0.01,
// "%.0f ms" by 1. Nudging by anything else moves the number without the
// reading changing, which looks broken.
static float StepFromFormat(const char* fmt) {
    if (!fmt) return 0.01f;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') continue;
        const char* q = p + 1;
        while (*q && !strchr("dif", *q)) {
            if (*q == '.') {
                const int decimals = atoi(q + 1);
                float step = 1.0f;
                for (int i = 0; i < decimals; ++i) step *= 0.1f;
                return step;
            }
            ++q;
        }
        return 1.0f;
    }
    return 0.01f;
}

enum { SA_NONE = 0, SA_DRAG, SA_DEC, SA_INC };

static void Chevron(ImDrawList* dl, float cx, float cy, bool left, bool hot) {
    const ImU32 c = hot ? col::accent : col::textFaint;
    const float w = 3.0f, h = 4.5f;
    const float dir = left ? -1.0f : 1.0f;
    dl->AddLine(ImVec2(cx - dir * w * 0.5f, cy - h),
                ImVec2(cx + dir * w * 0.5f, cy), c, 1.6f);
    dl->AddLine(ImVec2(cx + dir * w * 0.5f, cy),
                ImVec2(cx - dir * w * 0.5f, cy + h), c, 1.6f);
}

// Shared drawing and interaction. `t` is the normalised position; the return
// says what the user did so the caller can apply it in value units.
static int SliderCore(const char* id, const char* label, float* t,
                      const char* valueText)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return SA_NONE;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float rowH   = 22.0f;
    const float labelW = LabelWidth();
    const float valueW = 54.0f;
    const float arrowW = 15.0f;
    const float knobR  = 6.0f;
    const float trackH = 4.0f;
    const float fullW  = ImGui::GetContentRegionAvail().x;
    const float trackW = ImMax(30.0f, fullW - labelW - valueW - arrowW * 2.0f);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + rowH * 0.5f;

    const float decX    = p.x + labelW;
    const float trackX0 = decX + arrowW;
    const float trackX1 = trackX0 + trackW;

    int action = SA_NONE;
    char sub[160];

    snprintf(sub, sizeof(sub), "%s_dec", id);
    ImGui::SetCursorScreenPos(ImVec2(decX, p.y));
    ImGui::InvisibleButton(sub, ImVec2(arrowW, rowH));
    const bool decHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) action = SA_DEC;

    snprintf(sub, sizeof(sub), "%s_track", id);
    ImGui::SetCursorScreenPos(ImVec2(trackX0, p.y));
    ImGui::InvisibleButton(sub, ImVec2(trackW, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    if (held) {
        float nt = ImClamp((ImGui::GetIO().MousePos.x - trackX0) / ImMax(1.0f, trackW),
                           0.0f, 1.0f);
        if (nt != *t) { *t = nt; action = SA_DRAG; }
    }

    snprintf(sub, sizeof(sub), "%s_inc", id);
    ImGui::SetCursorScreenPos(ImVec2(trackX1, p.y));
    ImGui::InvisibleButton(sub, ImVec2(arrowW, rowH));
    const bool incHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) action = SA_INC;

    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, cy - ls.y * 0.5f),
                (hovered || held) ? col::text : col::textDim, label);

    Chevron(dl, decX + arrowW * 0.5f, cy, true, decHot);
    Chevron(dl, trackX1 + arrowW * 0.5f, cy, false, incHot);

    // The empty part of the track is a recessed surface, so sliders change
    // with the style like everything else. The filled part stays flat accent:
    // it shows a value, and a value has to read at a glance rather than
    // participate in the lighting.
    DrawSurface(dl, ImVec2(trackX0, cy - trackH * 0.5f),
                ImVec2(trackX1, cy + trackH * 0.5f),
                trackH * 0.5f, false, true, RoleControl);
    const float knobX = trackX0 + trackW * (*t);

    // The filled part is only drawn once it is wide enough to be round.
    //
    // At the low end of a slider the fill is a couple of pixels wide while
    // the radius is half the track height, and ImGui cannot round a shape
    // narrower than its own corners -- so it comes out as a hard square nub
    // against an otherwise rounded track. That is the square that shows up
    // on every slider sitting near its minimum.
    //
    // Below that width there is nothing meaningful to show anyway: the knob
    // is drawn on top and already marks the position.
    const float fillW = knobX - trackX0;
    if (fillW > trackH) {
        dl->AddRectFilled(ImVec2(trackX0, cy - trackH * 0.5f),
                          ImVec2(knobX, cy + trackH * 0.5f),
                          col::accent, trackH * 0.5f);
    } else if (fillW > 0.5f) {
        // A stub too short to round is drawn as a circle instead, which is
        // what the rounded end would have looked like.
        dl->AddCircleFilled(ImVec2(trackX0 + trackH * 0.5f, cy),
                            trackH * 0.5f, col::accent, 10);
    }

    if (hovered || held)
        dl->AddCircleFilled(ImVec2(knobX, cy), knobR + 4.0f,
                            (col::accent & 0x00FFFFFF) | 0x33000000);
    dl->AddCircleFilled(ImVec2(knobX, cy), knobR, col::accent);
    dl->AddCircleFilled(ImVec2(knobX, cy), knobR - 2.4f, col::base);

    const ImVec2 vs = ImGui::CalcTextSize(valueText);
    dl->AddText(ImVec2(p.x + fullW - vs.x, cy - vs.y * 0.5f), col::text, valueText);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
    return action;
}

bool SliderInt(const char* label, int* v, int lo, int hi, int step,
               const char* fmt, const char* tip) {
    float t = (hi > lo) ? float(*v - lo) / float(hi - lo) : 0.0f;
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);

    char id[128];
    snprintf(id, sizeof(id), "##si_%s", label);

    const int nudge = step > 1 ? step : 1;
    const int act = SliderCore(id, label, &t, buf);
    TipOnHover(tip);

    int nv = *v;
    if (act == SA_DRAG) {
        nv = lo + int(t * float(hi - lo) + 0.5f);
        if (step > 1) nv = lo + ((nv - lo + step / 2) / step) * step;
    } else if (act == SA_DEC) {
        nv = *v - nudge;
    } else if (act == SA_INC) {
        nv = *v + nudge;
    } else {
        return false;
    }
    nv = ImClamp(nv, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

bool SliderFloat(const char* label, float* v, float lo, float hi,
                 const char* fmt, const char* tip) {
    float t = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);

    char id[128];
    snprintf(id, sizeof(id), "##sf_%s", label);

    const float nudge = StepFromFormat(fmt);
    const int act = SliderCore(id, label, &t, buf);
    TipOnHover(tip);

    float nv = *v;
    if (act == SA_DRAG)     nv = lo + t * (hi - lo);
    else if (act == SA_DEC) nv = *v - nudge;
    else if (act == SA_INC) nv = *v + nudge;
    else                    return false;

    // Snap to the step grid, so repeated nudges do not drift off it.
    if (act != SA_DRAG && nudge > 0.0f)
        nv = roundf(nv / nudge) * nudge;

    nv = ImClamp(nv, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

bool SliderConfidence(const char* label, float* v, const char* tip) {
    // Same geometry as SliderCore, but the track says that both ends are bad
    // rather than that more is better.
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float rowH   = 22.0f;
    const float labelW = LabelWidth();
    const float valueW = 54.0f;
    const float arrowW = 15.0f;
    const float knobR  = 6.0f;
    const float trackH = 5.0f;
    const float fullW  = ImGui::GetContentRegionAvail().x;
    const float trackW = ImMax(30.0f, fullW - labelW - valueW - arrowW * 2.0f);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + rowH * 0.5f;
    const float decX = p.x + labelW;
    const float x0 = decX + arrowW, x1 = x0 + trackW;
    const float y0 = cy - trackH * 0.5f, y1 = cy + trackH * 0.5f;

    bool changed = false;
    const float nudge = 0.01f;

    // Scoped by the label, like every other slider.
    //
    // The three ids below are fixed literals, so two of these in one window
    // would share one set of widget identities: a click on either would
    // register as the first, and a drag would move whichever ImGui decided
    // owned the id. There are two call sites today, which is exactly the
    // number needed for it to go wrong.
    ImGui::PushID(label);

    ImGui::SetCursorScreenPos(ImVec2(decX, p.y));
    ImGui::InvisibleButton("##conf_dec", ImVec2(arrowW, rowH));
    const bool decHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { *v -= nudge; changed = true; }

    ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
    ImGui::InvisibleButton("##conf_track", ImVec2(trackW, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    if (held) {
        const float t = ImClamp((ImGui::GetIO().MousePos.x - x0) / ImMax(1.0f, trackW),
                                0.0f, 1.0f);
        const float nv = 0.01f + t * 0.94f;
        if (nv != *v) { *v = nv; changed = true; }
    }

    ImGui::SetCursorScreenPos(ImVec2(x1, p.y));
    ImGui::InvisibleButton("##conf_inc", ImVec2(arrowW, rowH));
    const bool incHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { *v += nudge; changed = true; }

    if (changed) *v = ImClamp(roundf(*v / nudge) * nudge, 0.01f, 0.95f);

    const ImU32 bad = col::danger, ok = col::accent;
    auto seg = [&](float a, float b, ImU32 ca2, ImU32 cb2) {
        dl->AddRectFilledMultiColor(ImVec2(x0 + trackW * a, y0),
                                    ImVec2(x0 + trackW * b, y1), ca2, cb2, cb2, ca2);
    };
    seg(0.00f, 0.20f, bad, bad);
    seg(0.20f, 0.32f, bad, ok);
    seg(0.32f, 0.82f, ok,  ok);
    seg(0.82f, 1.00f, ok,  bad);

    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, cy - ls.y * 0.5f),
                (hovered || held) ? col::text : col::textDim, label);

    Chevron(dl, decX + arrowW * 0.5f, cy, true,  decHot);
    Chevron(dl, x1 + arrowW * 0.5f,   cy, false, incHot);

    const float t = ImClamp((*v - 0.01f) / 0.94f, 0.0f, 1.0f);
    const float kx = x0 + trackW * t;
    const bool risky = (*v < 0.20f) || (*v > 0.85f);
    const ImU32 kc = risky ? col::danger : col::accent;
    if (hovered || held)
        dl->AddCircleFilled(ImVec2(kx, cy), knobR + 4.0f, (kc & 0x00FFFFFF) | 0x33000000);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR, kc);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR - 2.4f, col::base);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", *v);
    const ImVec2 vs = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + fullW - vs.x, cy - vs.y * 0.5f),
                risky ? col::danger : col::text, buf);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
    TipOnHover(tip);
    ImGui::PopID();
    return changed;
}

// ----------------------------------------------------------------- splash

bool DrawSplash() {
    static double t0 = -1.0;
    const double now = ImGui::GetTime();
    if (t0 < 0.0) t0 = now;

    const float t = (float)(now - t0);
    const float kTotal = 1.35f;
    if (t >= kTotal) return false;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Fades out over the last third, so the panel is revealed rather than
    // swapped in.
    const float fade = t < kTotal * 0.66f
                     ? 1.0f
                     : 1.0f - (t - kTotal * 0.66f) / (kTotal * 0.34f);
    const float a = ImClamp(fade, 0.0f, 1.0f);

    dl->AddRectFilled(vp->WorkPos,
                      ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                             vp->WorkPos.y + vp->WorkSize.y),
                      (col::base & 0x00FFFFFF) | ((ImU32)(255 * a) << 24));

    // Ease-out on the entrance, so it arrives rather than snaps.
    const float e = ImClamp(t / 0.55f, 0.0f, 1.0f);
    const float ease = 1.0f - powf(1.0f - e, 3.0f);
    const float size = 46.0f + 34.0f * ease;

    const ImVec2 c(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                   vp->WorkPos.y + vp->WorkSize.y * 0.5f - 14.0f);

    // The mark is drawn piecewise so it assembles: ring first, then spokes,
    // then the core, which is the order it reads in.
    const float w = ImMax(1.0f, size * 0.055f);
    const float R = size * 0.34f;
    ImVec2 p[3];
    for (int i = 0; i < 3; ++i) {
        const float ang = (i * 120.0f - 90.0f) * 3.14159265f / 180.0f;
        p[i] = ImVec2(c.x + R * cosf(ang), c.y + R * sinf(ang));
    }

    auto fadeCol = [&](ImU32 col, float k) {
        return (col & 0x00FFFFFF) | ((ImU32)(255 * ImClamp(k, 0.0f, 1.0f) * a) << 24);
    };

    const float edges  = ImClamp((t - 0.05f) / 0.30f, 0.0f, 1.0f);
    const float spokes = ImClamp((t - 0.22f) / 0.30f, 0.0f, 1.0f);
    const float core   = ImClamp((t - 0.40f) / 0.25f, 0.0f, 1.0f);

    for (int i = 0; i < 3; ++i) {
        const ImVec2& q = p[(i + 1) % 3];
        dl->AddLine(p[i], ImVec2(p[i].x + (q.x - p[i].x) * edges,
                                 p[i].y + (q.y - p[i].y) * edges),
                    fadeCol(col::accentDim, edges), w);
    }
    for (int i = 0; i < 3; ++i)
        dl->AddLine(c, ImVec2(c.x + (p[i].x - c.x) * spokes,
                              c.y + (p[i].y - c.y) * spokes),
                    fadeCol(col::accent, spokes), w);
    for (int i = 0; i < 3; ++i)
        dl->AddCircleFilled(p[i], size * 0.105f * spokes,
                            fadeCol(col::accent, spokes), 20);
    dl->AddCircleFilled(c, size * 0.13f * core,
                        fadeCol(col::accent, core), 22);

    ImGui::PushFont(fontTitle);
    const float nameA = ImClamp((t - 0.55f) / 0.30f, 0.0f, 1.0f);
    const ImVec2 ts = ImGui::CalcTextSize("loopcore");
    dl->AddText(fontTitle, ImGui::GetFontSize(),
                ImVec2(c.x - ts.x * 0.5f, c.y + size * 0.62f),
                fadeCol(col::text, nameA), "loopcore");
    ImGui::PopFont();

    return true;
}

bool SliderPiecewise(const char* label, float* v, float lo, float mid,
                     float hi, float split, const char* fmt, const char* tip)
{
    split = ImClamp(split, 0.05f, 0.95f);

    // Value to track position, and back again.
    auto toT = [&](float val) {
        if (val <= mid) return (mid > lo) ? (val - lo) / (mid - lo) * split : 0.0f;
        return split + (hi > mid ? (val - mid) / (hi - mid) * (1.0f - split) : 0.0f);
    };
    auto toV = [&](float t) {
        if (t <= split) return lo + (t / split) * (mid - lo);
        return mid + ((t - split) / (1.0f - split)) * (hi - mid);
    };

    float t = ImClamp(toT(*v), 0.0f, 1.0f);
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);

    char id[128];
    snprintf(id, sizeof(id), "##sp_%s", label);

    // The nudge is a step of the track, so the arrows move by a sensible
    // amount at both ends rather than one that suits only the dense half.
    const float nudgeT = 0.01f;
    const int act = SliderCore(id, label, &t, buf);
    TipOnHover(tip);

    float nv = *v;
    if (act == SA_DRAG)      nv = toV(t);
    else if (act == SA_DEC)  nv = toV(ImClamp(toT(*v) - nudgeT, 0.0f, 1.0f));
    else if (act == SA_INC)  nv = toV(ImClamp(toT(*v) + nudgeT, 0.0f, 1.0f));
    else                     return false;

    // Rounded to something readable: a lead time of 137.4 ms is noise.
    nv = (nv < mid) ? roundf(nv) : roundf(nv / 5.0f) * 5.0f;
    nv = ImClamp(nv, lo, hi);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

bool SliderSweetSpot(const char* label, int* v, int lo, int hi, int step,
                     int sweet, float sweetWidth, const char* fmt,
                     const char* tip)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float rowH   = 22.0f;
    const float labelW = LabelWidth();
    const float valueW = 54.0f;
    const float arrowW = 15.0f;
    const float knobR  = 6.0f;
    const float trackH = 5.0f;
    const float fullW  = ImGui::GetContentRegionAvail().x;
    const float trackW = ImMax(30.0f, fullW - labelW - valueW - arrowW * 2.0f);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + rowH * 0.5f;
    const float decX = p.x + labelW;
    const float x0 = decX + arrowW, x1 = x0 + trackW;
    const float y0 = cy - trackH * 0.5f, y1 = cy + trackH * 0.5f;

    bool changed = false;

    // Scoped by the label, like the other sliders.
    //
    // The three ids below are fixed literals, so two of these sliders in one
    // window would share a single set of widget identities: ImGui would
    // treat a click on either as a click on the first, and a drag would move
    // whichever it decided owned the id. There is one caller today, which is
    // why nothing has misbehaved -- it would break the moment a second
    // appeared, and the failure would look like a UI glitch rather than a
    // missing PushID.
    ImGui::PushID(label);

    ImGui::SetCursorScreenPos(ImVec2(decX, p.y));
    ImGui::InvisibleButton("##ss_dec", ImVec2(arrowW, rowH));
    const bool decHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { *v -= step; changed = true; }

    ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
    ImGui::InvisibleButton("##ss_track", ImVec2(trackW, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    if (held) {
        const float t = ImClamp((ImGui::GetIO().MousePos.x - x0) / ImMax(1.0f, trackW),
                                0.0f, 1.0f);
        const int nv = lo + (int)(t * (hi - lo) + 0.5f);
        if (nv != *v) { *v = nv; changed = true; }
    }

    ImGui::SetCursorScreenPos(ImVec2(x1, p.y));
    ImGui::InvisibleButton("##ss_inc", ImVec2(arrowW, rowH));
    const bool incHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { *v += step; changed = true; }

    if (changed) {
        if (step > 1) *v = ((*v + step / 2) / step) * step;
        *v = ImClamp(*v, lo, hi);
    }

    // Track: red at both ends, easing into the accent across a narrow band
    // centred on the recommendation.
    const float sc = ImClamp((float)(sweet - lo) / ImMax(1.0f, (float)(hi - lo)),
                             0.0f, 1.0f);
    const float half = ImClamp(sweetWidth * 0.5f, 0.01f, 0.45f);
    const ImU32 bad = col::danger, ok = col::accent;
    auto seg = [&](float a, float b, ImU32 ca, ImU32 cb) {
        if (b <= a) return;
        dl->AddRectFilledMultiColor(ImVec2(x0 + trackW * a, y0),
                                    ImVec2(x0 + trackW * b, y1), ca, cb, cb, ca);
    };
    const float lA = ImMax(0.0f, sc - half * 2.0f), lB = ImMax(0.0f, sc - half);
    const float rA = ImMin(1.0f, sc + half), rB = ImMin(1.0f, sc + half * 2.0f);
    seg(0.0f, lA, bad, bad);
    seg(lA, lB, bad, ok);
    seg(lB, rA, ok, ok);
    seg(rA, rB, ok, bad);
    seg(rB, 1.0f, bad, bad);

    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, cy - ls.y * 0.5f),
                (hovered || held) ? col::text : col::textDim, label);

    Chevron(dl, decX + arrowW * 0.5f, cy, true,  decHot);
    Chevron(dl, x1 + arrowW * 0.5f,   cy, false, incHot);

    // A notch at the recommendation itself, so it can be hit exactly.
    dl->AddLine(ImVec2(x0 + trackW * sc, y0 - 4.0f),
                ImVec2(x0 + trackW * sc, y1 + 4.0f),
                (col::text & 0x00FFFFFF) | 0x66000000, 1.0f);

    const float t = ImClamp((float)(*v - lo) / ImMax(1.0f, (float)(hi - lo)),
                            0.0f, 1.0f);
    const float kx = x0 + trackW * t;
    const bool risky = std::fabs(t - sc) > half * 2.0f;
    const ImU32 kc = risky ? col::danger : col::accent;
    if (hovered || held)
        dl->AddCircleFilled(ImVec2(kx, cy), knobR + 4.0f, (kc & 0x00FFFFFF) | 0x33000000);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR, kc);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR - 2.4f, col::base);

    char buf[48];
    snprintf(buf, sizeof(buf), fmt, *v);
    const ImVec2 vs = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + fullW - vs.x, cy - vs.y * 0.5f),
                risky ? col::danger : col::text, buf);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
    TipOnHover(tip);
    ImGui::PopID();
    return changed;
}

void TraceGraph(const char* id, const std::vector<TimeSeries::Column>& cols,
                float height, float spanSec, const char* unit, bool warn)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    // The track colour, not a fixed dark one: on a light theme a hardcoded
    // near-black panel is the only thing on screen that did not change.
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), col::track, 4.0f);

    if (cols.empty()) { ImGui::Dummy(ImVec2(w, height)); return; }

    // Clipped to the frame, so a column at the extreme of the range cannot
    // paint a pixel outside it and leave a stray line down the panel.
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + height), true);

    // Scaled from the data present, with a floor so a flat trace does not
    // fill the panel with amplified rounding noise.
    float lo = 1e30f, hi = -1e30f;
    for (const auto& c : cols)
        if (c.has) { lo = ImMin(lo, c.mn); hi = ImMax(hi, c.mx); }
    if (lo > hi) {
        // Popped on this path too: an unbalanced clip rect corrupts every
        // widget drawn afterwards, not just this one.
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(w, height));
        return;
    }
    if (hi - lo < 0.25f) { const float m = (lo + hi) * 0.5f; lo = m - 0.125f; hi = m + 0.125f; }
    lo = ImMax(0.0f, lo - (hi - lo) * 0.08f);
    hi += (hi - lo) * 0.08f;

    const float pad = 3.0f;
    const float gh = height - pad * 2;
    auto yOf = [&](float v) {
        return p.y + pad + gh - (v - lo) / ImMax(0.0001f, hi - lo) * gh;
    };
    const float step = w / (float)cols.size();

    const ImU32 line = warn ? col::warn : col::accent;
    const ImU32 band = (line & 0x00FFFFFF) | 0x38000000;

    // The band as a continuous envelope, quad by quad between neighbours.
    //
    // Drawing one upright bar per column was correct but unreadable: at two
    // hundred and fifty columns that is two hundred and fifty separate bars,
    // and the eye sees a picket fence rather than a range. Joining each
    // column to the next turns the same numbers into a shape with a top edge
    // and a bottom edge, which is what makes a spread legible at all.
    //
    // Each quad spans two adjacent columns and is convex by construction,
    // which is the only constraint the fill routine actually has.
    for (size_t k = 0; k + 1 < cols.size(); ++k) {
        if (!cols[k].has || !cols[k + 1].has) continue;
        const float xa = p.x + (k + 0.5f) * step;
        const float xb = p.x + (k + 1.5f) * step;
        dl->AddQuadFilled(ImVec2(xa, yOf(cols[k].mx)),
                          ImVec2(xb, yOf(cols[k + 1].mx)),
                          ImVec2(xb, yOf(cols[k + 1].mn)),
                          ImVec2(xa, yOf(cols[k].mn)),
                          band);
    }

    // The mean, broken wherever the data is.
    size_t i = 0;
    while (i < cols.size()) {
        if (!cols[i].has) { ++i; continue; }
        size_t j = i;
        while (j + 1 < cols.size() && cols[j + 1].has) ++j;
        if (j > i) {
            dl->PathClear();
            for (size_t k = i; k <= j; ++k)
                dl->PathLineTo(ImVec2(p.x + (k + 0.5f) * step,
                                      yOf(cols[k].mean)));
            dl->PathStroke(line, 0, 1.4f);
        } else {
            // A lone column has no neighbour to draw a segment to, and
            // leaving it out entirely was how a sparse trace turned into a
            // row of disconnected ticks.
            const float x = p.x + (i + 0.5f) * step;
            const float y = yOf(cols[i].mean);
            dl->AddRectFilled(ImVec2(x - step * 0.5f, y - 0.7f),
                              ImVec2(x + step * 0.5f, y + 0.7f), line);
        }
        i = j + 1;
    }

    ImGui::PushFont(fontSmall);
    char t[48];

    // Labels sit on a dark plate rather than directly on the trace. Drawn
    // over a filled band they were unreadable, which is what a scale that
    // cannot be read is worth.
    auto plate = [&](ImVec2 at, const char* text) {
        const ImVec2 sz = ImGui::CalcTextSize(text);
        // The label plate takes the panel colour so it reads on any theme.
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1),
                          ImVec2(at.x + sz.x + 3, at.y + sz.y + 1),
                          (col::panel & 0x00FFFFFF) | 0xE0000000, 3.0f);
        dl->AddText(at, col::textDim, text);
    };

    snprintf(t, sizeof(t), "%.1f %s", hi, unit ? unit : "");
    plate(ImVec2(p.x + 6, p.y + 3), t);

    snprintf(t, sizeof(t), "%.1f", lo);
    plate(ImVec2(p.x + 6, p.y + height - ImGui::GetTextLineHeight() - 4), t);

    snprintf(t, sizeof(t), "%.0fs", spanSec);
    const ImVec2 ts = ImGui::CalcTextSize(t);
    plate(ImVec2(p.x + w - ts.x - 6, p.y + height - ts.y - 4), t);
    ImGui::PopFont();
    dl->PopClipRect();

    ImGui::Dummy(ImVec2(w, height));
}

void LeadPreview(float leadSeconds, float trust, float smoothMs,
                 bool predictionOn, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;

    const float pad = 26.0f;
    const float trackW = ImMax(60.0f, full - pad * 2);
    const float cy = p.y + height * 0.45f;

    // A triangle wave rather than a sine: constant speed with an abrupt
    // reversal is what actually tests a prediction. A sine spends most of
    // its time turning, which flatters every method equally.
    // The period follows from the width and that speed, so the box always
    // crosses at the same rate however wide the widget happens to be.
    // Slow enough to follow. The point is watching the marker lag and
    // catch up at each reversal, and at the previous speed the whole cycle
    // was over before that could be read.
    const float period = ImMax(2.4f, 2.0f * trackW / ImMax(1.0f, 210.0f * g_uiScale));
    const float t = fmodf((float)ImGui::GetTime(), period) / period;
    const float tri = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
    const float dir = (t < 0.5f) ? 1.0f : -1.0f;

    const float bw = 34.0f, bh = 26.0f;
    const float cx = p.x + pad + tri * trackW;

    // Speed in real pixels a second, not in fractions of the widget.
    //
    // Sweeping a fixed proportion of the track meant a wider card, or a
    // scaled-up interface, made the target move faster -- so the preview
    // showed a different behaviour on every layout, which is precisely what
    // it exists to let someone judge.
    const float speed = 210.0f * g_uiScale;

    dl->AddLine(ImVec2(p.x + pad, cy + bh * 0.5f + 8.0f),
                ImVec2(p.x + pad + trackW, cy + bh * 0.5f + 8.0f),
                col::border, 1.0f);

    dl->AddRectFilled(ImVec2(cx - bw * 0.5f, cy - bh * 0.5f),
                      ImVec2(cx + bw * 0.5f, cy + bh * 0.5f),
                      (col::accent & 0x00FFFFFF) | 0x1F000000, 3.0f);
    dl->AddRect(ImVec2(cx - bw * 0.5f, cy - bh * 0.5f),
                ImVec2(cx + bw * 0.5f, cy + bh * 0.5f),
                col::accent, 3.0f, 0, 1.5f);

    if (predictionOn) {
        // The estimate lags the truth by the smoothing window, which is the
        // whole point of showing it: the marker overshoots at each reversal
        // for exactly as long as the filter takes to notice.
        static float shownVel = 0.0f;
        const float dt = ImGui::GetIO().DeltaTime;
        const float tau = ImMax(0.02f, smoothMs * 0.001f);
        shownVel += (dir * speed - shownVel) * (1.0f - expf(-dt / tau));

        const float lx = cx + shownVel * leadSeconds * ImClamp(trust, 0.0f, 1.0f);
        const float clx = ImClamp(lx, p.x + 6.0f, p.x + full - 6.0f);

        dl->AddLine(ImVec2(clx - 8, cy), ImVec2(clx - 3, cy),
                    col::accent, 1.6f);
        dl->AddLine(ImVec2(clx + 3, cy), ImVec2(clx + 8, cy),
                    col::accent, 1.6f);
        dl->AddLine(ImVec2(clx, cy - 8), ImVec2(clx, cy - 3),
                    col::accent, 1.6f);
        dl->AddLine(ImVec2(clx, cy + 3), ImVec2(clx, cy + 8),
                    col::accent, 1.6f);
        dl->AddCircle(ImVec2(clx, cy), 3.5f,
                      col::accent, 12, 1.2f);
    }

    ImGui::PushFont(fontSmall);
    const char* note = predictionOn
        ? "box: where the target is    cross: where the lead points"
        : "prediction is off, so there is nothing to lead with";
    const ImVec2 ts = ImGui::CalcTextSize(note);
    dl->AddText(ImVec2(p.x + full * 0.5f - ts.x * 0.5f, p.y + height - 15.0f),
                col::textFaint, note);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(full, height));
}

void FovPreview(int screenW, int screenH, int fov, bool actionOn, int actionFov,
                int followX, int followY, float height)
{
    if (screenW <= 0 || screenH <= 0) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;

    // The monitor, drawn to its real aspect and centred.
    const float mh = height - 22.0f;
    const float mw = mh * (float)screenW / (float)screenH;
    const float mx = p.x + full * 0.5f - mw * 0.5f;
    const float my = p.y + 4.0f;

    dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                      (col::base & 0x00FFFFFF) | 0xFF000000, 3.0f);
    dl->AddRect(ImVec2(mx, my), ImVec2(mx + mw, my + mh), col::border, 3.0f,
                0, 1.0f);

    const float scale = mh / (float)screenH;   // same on both axes

    // Where the region actually is. In follow mode that is wherever the
    // cursor is, so the preview moves with it rather than implying the
    // region sits in the middle.
    float cx = mx + mw * 0.5f, cy = my + mh * 0.5f;
    if (followX >= 0 && followY >= 0) {
        cx = mx + (float)followX * scale;
        cy = my + (float)followY * scale;
        // Clamped so the box stays drawn inside the monitor outline even
        // when the cursor is right on an edge.
        const float halfF = fov * scale * 0.5f;
        cx = ImClamp(cx, mx + halfF, mx + mw - halfF);
        cy = ImClamp(cy, my + halfF, my + mh - halfF);
    }

    const float f = fov * scale * 0.5f;
    dl->AddRect(ImVec2(cx - f, cy - f), ImVec2(cx + f, cy + f),
                col::accent, 0.0f, 0, 1.5f);

    if (actionOn && actionFov > 0) {
        const float a = actionFov * scale * 0.5f;
        dl->AddRect(ImVec2(cx - a, cy - a), ImVec2(cx + a, cy + a),
                    col::accent, 0.0f, 0, 1.5f);
    }

    ImGui::PushFont(fontSmall);
    char buf[96];
    const char* mode = (followX >= 0) ? ", following" : "";
    if (actionOn)
        snprintf(buf, sizeof(buf), "%d px detect, %d px act, %d x %d%s",
                 fov, actionFov, screenW, screenH, mode);
    else
        snprintf(buf, sizeof(buf), "%d px on %d x %d%s",
                 fov, screenW, screenH, mode);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + full * 0.5f - ts.x * 0.5f, my + mh + 4.0f),
                col::textFaint, buf);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(full, height));
}

// ------------------------------------------------------------------ toast

namespace {
std::string g_toastText;
double      g_toastUntil = 0.0;
}

void Toast(const char* text, double seconds) {
    g_toastText  = text ? text : "";
    g_toastUntil = ImGui::GetTime() + seconds;
}

void DrawToast() {
    if (g_toastText.empty()) return;
    const double now = ImGui::GetTime();
    if (now > g_toastUntil) { g_toastText.clear(); return; }

    // Fade the last half second, so it leaves rather than vanishes.
    const double left = g_toastUntil - now;
    const float a = (float)ImClamp(left / 0.5, 0.0, 1.0);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImGui::PushFont(fontSmall);
    const float fsz = ImGui::GetFontSize();   // ImFont::FontSize is gone in 1.92
    const ImVec2 ts = ImGui::CalcTextSize(g_toastText.c_str());
    ImGui::PopFont();

    const ImVec2 pad(14, 9);
    const ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    const ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
                     vp->WorkPos.y + vp->WorkSize.y - size.y - 22.0f);

    const ImU32 bg = (col::raised & 0x00FFFFFF) | ((ImU32)(232 * a) << 24);
    const ImU32 bd = (col::warn   & 0x00FFFFFF) | ((ImU32)(255 * a) << 24);
    const ImU32 tc = (col::text   & 0x00FFFFFF) | ((ImU32)(255 * a) << 24);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 7.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), bd, 7.0f, 0, 1.5f);
    dl->AddText(fontSmall, fsz,
                ImVec2(pos.x + pad.x, pos.y + pad.y), tc, g_toastText.c_str());
}

// ---------------------------------------------------------------- toggle

bool Toggle(const char* label, bool* v, const char* tip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float rowH  = 22.0f;
    const float pillW = 34.0f;
    const float pillH = 18.0f;
    const float fullW = ImGui::GetContentRegionAvail().x;

    const ImVec2 p = ImGui::GetCursorScreenPos();

    char id[128];
    snprintf(id, sizeof(id), "##tg_%s", label);
    ImGui::InvisibleButton(id, ImVec2(fullW, rowH));
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    TipOnHover(tip);

    const float cy = p.y + rowH * 0.5f;
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, cy - ls.y * 0.5f),
                hovered ? col::text : col::textDim, label);

    const float px0 = p.x + fullW - pillW;
    // The track follows the chosen surface when it is off.
    //
    // Toggles, sliders and cards never called DrawSurface at all -- only the
    // two button types did -- which is why switching surface style changed
    // so little. A style that reaches two widgets out of five is not a style.
    //
    // When on, the accent fill stays: that is state, and it has to read
    // instantly whatever the surface is doing.
    const ImVec2 tr0(px0, cy - pillH * 0.5f);
    const ImVec2 tr1(px0 + pillW, cy + pillH * 0.5f);
    if (*v) {
        dl->AddRectFilled(tr0, tr1, col::accent, pillH * 0.5f);
    } else {
        DrawSurface(dl, tr0, tr1, pillH * 0.5f, false, true, RoleControl);
    }

    const float knobR = pillH * 0.5f - 3.0f;
    const float knobX = *v ? (px0 + pillW - pillH * 0.5f) : (px0 + pillH * 0.5f);
    dl->AddCircleFilled(ImVec2(knobX, cy), knobR,
                        *v ? col::base : col::textDim);
    return changed;
}

// ------------------------------------------------------------ combo row

bool ComboRow(const char* label, int* v, const char* const items[], int count,
              const char* tip)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;
    const float rowH = 24.0f;

    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, p.y + (rowH - ls.y) * 0.5f), col::textDim, label);

    const float lw = LabelWidth();
    const float boxW = ImMax(60.0f, full - lw);
    const ImVec2 b0(p.x + lw, p.y);
    const ImVec2 b1(p.x + lw + boxW, p.y + rowH);

    // Laid out here rather than by ImGui.
    //
    // ImGui sizes a combo from the font plus FramePadding, which came to 25
    // px against the 24 px box drawn behind it -- so the text sat a pixel
    // low. Worse, its arrow button is a square of that frame height, giving
    // a 25 px arrow next to 13 px text, which is what looked oversized.
    //
    // The popup is worth keeping, so BeginCombo still provides it; only the
    // closed state is drawn here, where the box height is already known.
    const bool hot = ImGui::IsMouseHoveringRect(b0, b1);
    DrawSurface(dl, b0, b1, 4.0f, hot, true, RoleControl);

    // The chevron, at text scale and vertically centred on the box.
    const float cx = b1.x - 12.0f;
    const float cy = (b0.y + b1.y) * 0.5f;
    const float cw = 3.5f;
    dl->AddLine(ImVec2(cx - cw, cy - cw * 0.5f), ImVec2(cx, cy + cw * 0.6f),
                col::textDim, 1.6f);
    dl->AddLine(ImVec2(cx + cw, cy - cw * 0.5f), ImVec2(cx, cy + cw * 0.6f),
                col::textDim, 1.6f);

    // The current value, clipped short of the chevron.
    const char* cur = (*v >= 0 && *v < count) ? items[*v] : "";
    const ImVec2 vs = ImGui::CalcTextSize(cur);
    dl->PushClipRect(ImVec2(b0.x + 8.0f, b0.y),
                     ImVec2(cx - cw - 4.0f, b1.y), true);
    dl->AddText(ImVec2(b0.x + 8.0f, cy - vs.y * 0.5f), col::text, cur);
    dl->PopClipRect();

    char id[128];
    snprintf(id, sizeof(id), "##cb_%s", label);

    // An invisible button over the box carries the click and the popup
    // anchor, so the drawn state and the interactive area are the same
    // rectangle by construction.
    ImGui::SetCursorScreenPos(b0);
    ImGui::InvisibleButton(id, ImVec2(boxW, rowH));
    const bool clicked = ImGui::IsItemClicked();
    TipOnHover(tip);

    char pid[136];
    snprintf(pid, sizeof(pid), "##cbp_%s", label);
    if (clicked) ImGui::OpenPopup(pid);

    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(b0.x, b1.y + 2.0f));
    ImGui::SetNextWindowSize(ImVec2(boxW, 0.0f));
    if (ImGui::BeginPopup(pid)) {
        for (int i = 0; i < count; ++i) {
            const bool sel = (i == *v);
            if (ImGui::Selectable(items[i], sel)) {
                *v = i;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 4.0f));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

// ---------------------------------------------------------- keybind row

const char* KeyName(int vk) {
    static char buf[64];
    switch (vk) {
    case 0:              return "None";
    case VK_LBUTTON:     return "Mouse L";
    case VK_RBUTTON:     return "Mouse R";
    case VK_MBUTTON:     return "Mouse M";
    case VK_XBUTTON1:    return "Mouse 4";
    case VK_XBUTTON2:    return "Mouse 5";
    case VK_SPACE:       return "Space";
    case VK_SHIFT:       return "Shift";
    case VK_LSHIFT:      return "L Shift";
    case VK_RSHIFT:      return "R Shift";
    case VK_CONTROL:     return "Ctrl";
    case VK_LCONTROL:    return "L Ctrl";
    case VK_RCONTROL:    return "R Ctrl";
    case VK_MENU:        return "Alt";
    case VK_LMENU:       return "L Alt";
    case VK_RMENU:       return "R Alt";
    case VK_TAB:         return "Tab";
    case VK_CAPITAL:     return "Caps";
    case VK_RETURN:      return "Enter";
    case VK_BACK:        return "Backspace";
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z') { snprintf(buf, sizeof(buf), "%c", vk); return buf; }
    if (vk >= '0' && vk <= '9') { snprintf(buf, sizeof(buf), "%c", vk); return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    // Fall back to whatever the keyboard layout calls it.
    const UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0) return buf;
    snprintf(buf, sizeof(buf), "VK %d", vk);
    return buf;
}

bool KeybindRow(const char* label, int* vk, const char* tip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;
    const float rowH = 24.0f;

    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    // Deliberately shared between every keybind row in a window.
    //
    // These are storage keys, not widget ids: they hold which row is
    // currently capturing, and there must be exactly one answer to that per
    // window. Scoping them per label would let two rows capture at once and
    // both swallow the same keypress.
    const ImGuiID capturingId = ImGui::GetID("##kb_capturing");
    const ImGuiID armedId     = ImGui::GetID("##kb_armed");

    bool capturing = store->GetInt(capturingId, 0) == (int)id;

    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, p.y + (rowH - ls.y) * 0.5f), col::textDim, label);

    const float btnW = ImMin(96.0f, ImMax(64.0f, full * 0.42f));
    ImGui::SetCursorScreenPos(ImVec2(p.x + full - btnW, p.y));

    char bid[128];
    snprintf(bid, sizeof(bid), "%s##kbbtn_%s",
             capturing ? "press a key" : KeyName(*vk), label);

    bool changed = false;

    // Painted as a surface, with ImGui's own fill removed.
    //
    // This drew with flat colours like the combo did, so it was the other
    // control that stayed the same whatever surface was chosen. While
    // capturing it keeps the accent fill instead: that is a state the user
    // needs to see immediately, and it should not be competing with a
    // lighting effect.
    const ImVec2 kb0(p.x + full - btnW, p.y);
    const ImVec2 kb1(p.x + full, p.y + rowH);
    if (capturing)
        dl->AddRectFilled(kb0, kb1, col::accentDim, 4.0f);
    else
        DrawSurface(dl, kb0, kb1, 4.0f,
                    ImGui::IsMouseHoveringRect(kb0, kb1), false, RoleControl);

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(bid, ImVec2(btnW, rowH))) {
        if (!capturing) {
            store->SetInt(capturingId, (int)id);
            // The click that started capture must be released before any
            // button counts, or the bind instantly becomes Mouse L.
            store->SetInt(armedId, 0);
            capturing = true;
        }
    }
    ImGui::PopStyleColor(2);
    TipOnHover(tip);

    if (capturing) {
        const bool armed = store->GetInt(armedId, 0) != 0;
        if (!armed) {
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
                store->SetInt(armedId, 1);
        } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            store->SetInt(capturingId, 0);
        } else {
            for (int k = 1; k < 255; ++k) {
                if (k == VK_ESCAPE) continue;
                if (GetAsyncKeyState(k) & 0x8000) {
                    *vk = k;
                    changed = true;
                    store->SetInt(capturingId, 0);
                    break;
                }
            }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 4.0f));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

// --------------------------------------------------------------- buttons

bool PrimaryButton(const char* label, const ImVec2& size) {
    if (g_uiStyle != StyleFlat) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImVec2 sz = size;
        if (sz.x <= 0.0f) sz.x = ImGui::CalcTextSize(label).x + 26.0f;
        if (sz.y <= 0.0f) sz.y = ImGui::GetFrameHeight();

        ImGui::InvisibleButton(label, sz);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const bool clicked = ImGui::IsItemClicked();

        const ImVec2 b(at.x + sz.x, at.y + sz.y);
        DrawSurface(dl, at, b, 6.0f, hovered, held, RoleControl);
        // Tinted with the accent over whatever the surface did, so the
        // primary action still reads as the primary one in every style.
        dl->AddRectFilled(at, b, WithAlpha(col::accent, held ? 200 : 165), 6.0f);

        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(at.x + (sz.x - ts.x) * 0.5f + (held ? 0.5f : 0.0f),
                           at.y + (sz.y - ts.y) * 0.5f + (held ? 0.5f : 0.0f)),
                    col::base, label);
        return clicked;
    }

    ImGui::PushStyleColor(ImGuiCol_Button,        V(col::accentDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(col::accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  V(col::accentDim));
    ImGui::PushStyleColor(ImGuiCol_Text,          V(col::base));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

bool GhostButton(const char* label, const ImVec2& size) {
    // Flat, a button is an outline that fills on hover. The other styles
    // want a surface of their own, because a shape with no body cannot be
    // lit, embossed or made of glass -- which is the whole of what
    // distinguishes them.
    if (g_uiStyle == StyleFlat) {
        // A faint fill and a border, not bare text.
        //
        // A fully transparent button is indistinguishable from a label until
        // it is hovered, which means the only way to find out something is
        // clickable is to move the mouse over it. Cards on the Model tab are
        // mostly buttons, and they read as a paragraph.
        ImGui::PushStyleColor(ImGuiCol_Button,        V(col::raised));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(col::hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  V(col::border));
        ImGui::PushStyleColor(ImGuiCol_Border,        V(col::border));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        const bool r = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return r;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    if (sz.x <= 0.0f) sz.x = ImGui::CalcTextSize(label).x + 22.0f;
    if (sz.y <= 0.0f) sz.y = ImGui::GetFrameHeight();

    ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    // Held, the lighting is inverted: the shadow moves inside and the
    // highlight underneath, which is what a surface being pressed looks like
    // and the reason these styles read as physical at all.
    DrawSurface(dl, at, ImVec2(at.x + sz.x, at.y + sz.y), 6.0f, hovered, held,
                RoleControl);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(at.x + (sz.x - ts.x) * 0.5f + (held ? 0.5f : 0.0f),
                       at.y + (sz.y - ts.y) * 0.5f + (held ? 0.5f : 0.0f)),
                hovered ? col::text : col::textDim, label);
    return clicked;
}

// -------------------------------------------------------- caption buttons

int CaptionButtons(bool isMaximized) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = 38.0f, h = 38.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    int pressed = 0;
    const char* ids[4] = {"##cap_help", "##cap_min", "##cap_max", "##cap_close"};

    for (int i = 0; i < 4; ++i) {
        const float x = origin.x + w * i;
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::InvisibleButton(ids[i], ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) pressed = i + 1;

        if (hov)
            dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + w, origin.y + h),
                              i == 3 ? col::close : col::hover);

        const ImU32 fg = (hov && i == 3) ? IM_COL32_WHITE
                       : hov             ? col::text
                                         : col::textDim;
        const float cx = x + w * 0.5f, cy = origin.y + h * 0.5f;
        const float s = 4.5f;

        switch (i) {
        case 0: {                       // help
            ImGui::PushFont(fontTitle);
            const ImVec2 ts = ImGui::CalcTextSize("?");
            dl->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), fg, "?");
            ImGui::PopFont();
            break;
        }
        case 1:                         // minimise
            dl->AddLine(ImVec2(cx - s, cy), ImVec2(cx + s, cy), fg, 1.2f);
            break;
        case 2:                         // maximise / restore
            if (isMaximized) {
                dl->AddRect(ImVec2(cx - s + 2, cy - s), ImVec2(cx + s, cy + s - 2),
                            fg, 0, 0, 1.2f);
                dl->AddRect(ImVec2(cx - s, cy - s + 2), ImVec2(cx + s - 2, cy + s),
                            fg, 0, 0, 1.2f);
            } else {
                dl->AddRect(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s),
                            fg, 0, 0, 1.2f);
            }
            break;
        default:                        // close
            dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), fg, 1.3f);
            dl->AddLine(ImVec2(cx + s, cy - s), ImVec2(cx - s, cy + s), fg, 1.3f);
            break;
        }
    }
    return pressed;
}

// ----------------------------------------------------------------- misc

void CameraGlyph(ImDrawList* dl, ImVec2 c, float size, ImU32 colour) {
    // A body, a lens and the little bump on top. At this size the bump is
    // what makes it read as a camera rather than a rounded rectangle, so it
    // is drawn first and slightly proud of the body.
    const float w = size, h = size * 0.72f;
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f);
    const ImVec2 b(c.x + w * 0.5f, c.y + h * 0.5f);

    dl->AddRectFilled(ImVec2(c.x - w * 0.16f, a.y - h * 0.18f),
                      ImVec2(c.x + w * 0.10f, a.y + 1.0f),
                      colour, 1.0f);
    dl->AddRectFilled(a, b, colour, size * 0.16f);

    // The lens is punched out rather than drawn over, so the glyph stays
    // legible against whatever the pill behind it is doing.
    dl->AddCircleFilled(ImVec2(c.x, c.y + h * 0.04f), h * 0.26f,
                        col::base, 12);
}

ImU32 OkGreen() {
    const ImU32 b = col::base;
    const int r = (b >> 0) & 0xFF, g = (b >> 8) & 0xFF, bl = (b >> 16) & 0xFF;
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * bl;
    return (lum > 128.0f) ? IM_COL32(0x0E, 0x7A, 0x38, 0xFF)   // on light
                          : IM_COL32(0x22, 0xC5, 0x5E, 0xFF);  // on dark
}

void StatusPill(const char* text, ImU32 colour, bool spinning, ImU32 ring) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fontSmall);

    const ImVec2 ts   = ImGui::CalcTextSize(text);
    const float dotR  = 3.0f;
    const float padX  = 8.0f;
    const float gap   = 7.0f;
    const float padY  = 4.0f;

    // Width has to account for both the dot and the gap after it, or the
    // text runs past the right edge of the pill.
    const ImVec2 sz(padX + dotR * 2 + gap + ts.x + padX, ts.y + padY * 2);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + sz.y * 0.5f;

    const ImVec2 p1(p.x + sz.x, p.y + sz.y);
    const float rr = sz.y * 0.5f;

    dl->AddRectFilled(p, p1, (colour & 0x00FFFFFF) | 0x26000000, rr);

    if (ring) {
        // One faint halo and a thin line, rather than four rings.
        //
        // The heavier version read as a smudge around the pill. A single
        // narrow ring is enough to pick out at a glance, which is all this
        // has to do.
        dl->AddRect(ImVec2(p.x - 1.5f, p.y - 1.5f),
                    ImVec2(p1.x + 1.5f, p1.y + 1.5f),
                    (ring & 0x00FFFFFF) | 0x22000000, rr + 1.5f, 0, 1.0f);
        dl->AddRect(p, p1, ring, rr, 0, 1.2f);
    }
    const ImVec2 dotC(p.x + padX + dotR, cy);
    if (spinning) {
        // An arc that turns, rather than a dot that sits there. A steady
        // light says "this is the state"; a moving one says "this is
        // happening", which is the distinction between building and idle.
        const float a0 = (float)(ImGui::GetTime() * 3.2);
        dl->PathClear();
        dl->PathArcTo(dotC, dotR + 1.0f, a0, a0 + 4.2f, 12);
        dl->PathStroke(colour, 0, 1.8f);
    } else {
        dl->AddCircleFilled(dotC, dotR, colour, 12);
    }
    dl->AddText(ImVec2(p.x + padX + dotR * 2 + gap, cy - ts.y * 0.5f), colour, text);

    ImGui::Dummy(sz);
    ImGui::PopFont();
}

void DrawLogo(ImDrawList* dl, ImVec2 tl, float size, float hoverT) {
    // Same proportions as tools/make_icon.py, so the in-app mark and the
    // taskbar icon are one drawing rather than two similar ones.
    const float t = ImClamp(hoverT, 0.0f, 1.0f);
    const float now = (float)ImGui::GetTime();

    // On hover the three nodes chase each other around the ring and the core
    // pulses, as though the little network had woken up.
    const float spin  = t * now * 2.2f;
    const float bulge = 1.0f + 0.10f * t * sinf(now * 6.0f);

    const float w = ImMax(1.0f, size * 0.055f) * (1.0f + 0.25f * t);
    const ImVec2 c(tl.x + size * 0.5f, tl.y + size * 0.5f);
    const float R = size * 0.34f * bulge;

    ImVec2 p[3];
    for (int i = 0; i < 3; ++i) {
        const float a = (i * 120.0f - 90.0f) * 3.14159265f / 180.0f + spin;
        p[i] = ImVec2(c.x + R * cosf(a), c.y + R * sinf(a));
    }

    // Outer triangle dimmer than the spokes, so the nodes read first.
    dl->AddLine(p[0], p[1], col::accentDim, w);
    dl->AddLine(p[1], p[2], col::accentDim, w);
    dl->AddLine(p[2], p[0], col::accentDim, w);
    for (int i = 0; i < 3; ++i) dl->AddLine(c, p[i], col::accent, w);

    for (int i = 0; i < 3; ++i) {
        // Each node swells slightly out of phase, so the ring looks like it
        // is passing something around rather than throbbing as one piece.
        const float bump = 1.0f + 0.35f * t *
                           sinf(now * 7.0f - (float)i * 2.09f);
        dl->AddCircleFilled(p[i], size * 0.105f * bump, col::accent, 20);
    }

    const float corePulse = 1.0f + 0.22f * t * sinf(now * 9.0f);
    dl->AddCircleFilled(c, size * 0.13f * corePulse,
                        col::accent, 22);

    // A faint halo that only appears on hover.
    if (t > 0.01f)
        dl->AddCircle(c, size * 0.46f * bulge,
                      (col::accent & 0x00FFFFFF) | ((ImU32)(60 * t) << 24),
                      28, 1.5f);
}

void AnchorPreview(int anchor, float offXPct, float offYPct, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Its own width, not whatever is left. Taking the remaining space made
    // it spill past the card when placed beside something.
    const float full = ImMin(86.0f, ImGui::GetContentRegionAvail().x);

    // A tall-ish box, since the targets this is usually pointed at are
    // taller than they are wide and the vertical offset is the one people
    // actually reach for.
    const float bh = height - 12.0f;
    const float bw = bh * 0.55f;
    const float bx = p.x + full * 0.5f - bw * 0.5f;
    const float by = p.y + 6.0f;

    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
                      (col::accent & 0x00FFFFFF) | 0x1A000000, 3.0f);
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), col::accent, 3.0f,
                0, 1.5f);

    // Mark the edge being measured from, so "steadiest" is not a mystery.
    const ImU32 edgeCol = (col::text & 0x00FFFFFF) | 0xCC000000;
    if (anchor == 1)
        dl->AddLine(ImVec2(bx, by), ImVec2(bx + bw, by), edgeCol, 2.5f);
    else if (anchor == 2)
        dl->AddLine(ImVec2(bx, by + bh), ImVec2(bx + bw, by + bh), edgeCol, 2.5f);
    else if (anchor == 0)
        dl->AddLine(ImVec2(bx, by + bh * 0.5f),
                    ImVec2(bx + bw, by + bh * 0.5f),
                    (col::textFaint & 0x00FFFFFF) | 0x99000000, 1.0f);
    else {
        // Steadiest: which one wins depends on the target, so show both
        // faintly rather than claiming one.
        const ImU32 f = (col::textFaint & 0x00FFFFFF) | 0x88000000;
        dl->AddLine(ImVec2(bx, by), ImVec2(bx + bw, by), f, 2.0f);
        dl->AddLine(ImVec2(bx, by + bh), ImVec2(bx + bw, by + bh), f, 2.0f);
    }

    // The aim point itself. Positive Y is upward, matching the slider.
    const float ax = bx + bw * (0.5f + offXPct * 0.01f);
    const float ay = by + bh * (0.5f - offYPct * 0.01f);
    dl->AddCircleFilled(ImVec2(ax, ay), 5.0f,
                        (col::base & 0x00FFFFFF) | 0xFF000000, 16);
    dl->AddCircleFilled(ImVec2(ax, ay), 3.5f,
                        col::accent, 16);
    dl->AddLine(ImVec2(ax - 8, ay), ImVec2(ax + 8, ay), col::accent, 1.0f);
    dl->AddLine(ImVec2(ax, ay - 8), ImVec2(ax, ay + 8), col::accent, 1.0f);

    ImGui::Dummy(ImVec2(full, height));
}

void AlertBox(int severity, const std::vector<std::string>& lines) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;

    const ImU32 accentCol = severity >= 2 ? col::danger
                          : severity == 1 ? col::warn
                                          : col::accent;

    ImGui::PushFont(fontSmall);
    const float pad = 8.0f;
    const float lineH = ImGui::GetTextLineHeight() + 2.0f;

    // Measured before drawing, because the box has to be sized to hold every
    // line rather than the first one.
    float textH = 0.0f;
    for (const auto& l : lines)
        textH += ImMax(lineH, ImGui::CalcTextSize(l.c_str(), nullptr, false,
                                                  w - pad * 2 - 14.0f).y + 2.0f);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = textH + pad * 2;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      (accentCol & 0x00FFFFFF) | 0x14000000, 5.0f);
    // A bar down the left rather than a full border: it reads as a note
    // attached to the control above it instead of a separate panel.
    dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), accentCol, 2.0f);

    float y = p.y + pad;
    for (const auto& l : lines) {
        const float lw = w - pad * 2 - 14.0f;
        // The wrapping overload needs a font and size; passing null takes
        // the draw list's default, which is not the small font pushed above.
        dl->AddText(fontSmall, ImGui::GetFontSize(),
                    ImVec2(p.x + pad + 8.0f, y),
                    severity >= 1 ? col::text : col::textDim,
                    l.c_str(), nullptr, lw);
        y += ImMax(lineH, ImGui::CalcTextSize(l.c_str(), nullptr, false, lw).y + 2.0f);
    }
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(w, h));
}

void Hint(const char* fmt, ...) {
    if (!showHints) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    ImGui::PushFont(fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::textFaint));
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Notice(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const ImU32 c = level == 2 ? col::danger : level == 1 ? col::warn : col::accent;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushFont(fontSmall);
    const float textW = w - 22.0f;
    const ImVec2 ts = ImGui::CalcTextSize(buf, nullptr, false, textW);
    const float boxH = ts.y + 16.0f;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + boxH),
                      (c & 0x00FFFFFF) | 0x1A000000, 5.0f);
    dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + boxH), c, 2.0f);

    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y + 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, V(c));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + boxH + 4.0f));
    ImGui::Dummy(ImVec2(0, 0));
}

} // namespace lc::ui
