// ui.h
//
// Tiny hand-rolled, immediate-mode 2D widgets for the menu screens - the 2D
// analogue of shapes.h (which owns the 3D look). raylib core has no widgets, so
// these draw the game's "shaded wire" aesthetic in screen space: a low-alpha
// translucent fill under a bright opaque border, cyan to match the elements.
//
// Immediate mode: call a widget every frame; it draws and returns this frame's
// interaction. Any persistent state (text-field focus) is owned by the caller
// and passed in by reference, so there's no hidden global UI context. Pure 2D -
// depends only on raylib, no shapes.h / 3D coupling.

#pragma once

#include "raylib.h"
#include <string>
#include <cmath> // fminf/fmaxf/roundf (UiSlider)

//MARK: Theme
// Defaults match the cyan element palette (color_outline {0,255,200}). Callers
// can override per-widget where a different accent is wanted (e.g. a modal).
namespace ui {
inline const Color OUTLINE = {0, 255, 200, 255}; // opaque bright border / caret
inline const Color FILL    = {0, 255, 200, 40};  // translucent fill (idle)
inline const Color FILL_HI = {0, 255, 200, 90};  // translucent fill (hover)
inline const Color TEXT     = RAYWHITE;
inline const float BORDER   = 2.0f;
inline const int   PAD      = 10; // left text padding inside fields/buttons
}

//MARK: Panel
// The building block: translucent fill under an opaque border. In 2D there's no
// depth to fight (unlike shapes.h's BeginTranslucentFill), so a plain rect pair
// is enough.
inline void UiPanel(Rectangle r, Color outline = ui::OUTLINE, Color fill = ui::FILL) {
    DrawRectangleRec(r, fill);
    DrawRectangleLinesEx(r, ui::BORDER, outline);
}

//MARK: Modal panel
// Opaque variant for modal popups: a solid backdrop so nothing behind bleeds
// through, then the normal shaded-wire panel (translucent cyan fill + opaque
// border) on top. Same look as UiPanel, but the alpha-40 fill now sits over
// black instead of the dimmed screen, so the popup content stays readable.
inline void UiModalPanel(Rectangle r) {
    DrawRectangleRec(r, BLACK); // opaque backdrop
    UiPanel(r);                 // cyan tint + border, unchanged look
}

//MARK: Button
// Panel + centered label; brightens on hover. Returns true on the frame the
// mouse is pressed while hovering.
inline bool UiButton(Rectangle r, const char* label, int fontSize = 20) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    UiPanel(r, ui::OUTLINE, hovered ? ui::FILL_HI : ui::FILL);
    int tw = MeasureText(label, fontSize);
    DrawText(label,
             (int)(r.x + (r.width  - tw) / 2.0f),
             (int)(r.y + (r.height - fontSize) / 2.0f),
             fontSize, ui::TEXT);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

//MARK: Toggle
// Compact ON/OFF toggle: same shaded-wire panel as UiButton, but its label and
// tint reflect a bool the caller owns (passed by reference). Flips `value` on
// click and returns true on the frame it changed. ON reads in the bright accent,
// OFF dims to the idle fill so the state is legible at a glance.
inline bool UiToggle(Rectangle r, bool& value, int fontSize = 20) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    Color fill = value ? ui::FILL_HI : ui::FILL;
    if (hovered) fill.a = (unsigned char)(fill.a + 40); // lift on hover, either state
    UiPanel(r, ui::OUTLINE, fill);
    const char* label = value ? "ON" : "OFF";
    int tw = MeasureText(label, fontSize);
    DrawText(label,
             (int)(r.x + (r.width  - tw) / 2.0f),
             (int)(r.y + (r.height - fontSize) / 2.0f),
             fontSize, value ? ui::OUTLINE : ui::TEXT);
    bool clicked = hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (clicked) value = !value;
    return clicked;
}

//MARK: Text field
// Click in to focus, click out (or click another field) to defocus. While
// focused, typed printable chars append (up to maxLen) and Backspace deletes;
// a caret blinks at the end. `focused` is caller-owned so multiple fields don't
// share focus. Returns true if the text changed this frame.
// `pristine` (optional) enables "clear the default on first edit": while
// *pristine is true the field still holds untouched default text, so the first
// keystroke wipes it before the input is applied. Pass nullptr to disable.
// Typed chars are also rejected once the rendered text would no longer fit the
// field (or `maxTextWidth` pixels, if a tighter caller budget is given) — so
// the text can fill the visible space but never overflow it. Narrow glyphs get
// more characters than wide ones; maxLen stays as the hard backstop.
inline bool UiTextField(Rectangle r, std::string& text, bool& focused,
                        size_t maxLen = 16, int fontSize = 20,
                        bool* pristine = nullptr, int maxTextWidth = 0) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focused = hovered; // click toggles focus

    // Pixel budget for the rendered text: the field's inner width (padding both
    // sides, room for the caret), tightened further if the caller passed one.
    int fitWidth = (int)r.width - 2 * (int)ui::PAD - 4;
    if (maxTextWidth > 0 && maxTextWidth < fitWidth) fitWidth = maxTextWidth;

    bool changed = false;
    if (focused) {
        int c = GetCharPressed();
        while (c > 0) {
            if (c >= 32 && c <= 125) {
                if (pristine && *pristine) { text.clear(); *pristine = false; }
                if (text.size() < maxLen &&
                    MeasureText((text + (char)c).c_str(), fontSize) <= fitWidth) {
                    text.push_back((char)c);
                    changed = true;
                }
            }
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            if (pristine && *pristine) { text.clear(); *pristine = false; changed = true; }
            else if (!text.empty()) { text.pop_back(); changed = true; }
        }
    }

    UiPanel(r, focused ? ui::OUTLINE : Fade(ui::OUTLINE, 0.6f),
               (hovered || focused) ? ui::FILL_HI : ui::FILL);
    int tx = (int)(r.x + ui::PAD);
    int ty = (int)(r.y + (r.height - fontSize) / 2.0f);
    DrawText(text.c_str(), tx, ty, fontSize, ui::TEXT);
    // Blinking caret while focused (0.5s on / 0.5s off).
    if (focused && fmodf((float)GetTime(), 1.0f) < 0.5f) {
        int caretX = tx + MeasureText(text.c_str(), fontSize) + 2;
        DrawRectangle(caretX, ty, 2, fontSize, ui::OUTLINE);
    }
    return changed;
}

//MARK: Slider
// Horizontal slider. Drag the handle or press anywhere on the track to set value
// in [minV, maxV]. `value` and `active` (the drag latch) are caller-owned by
// reference so multiple sliders don't fight over the mouse - the same pattern
// UiTextField uses for `focused`. `step > 0` snaps the value (e.g. 1.0 for an
// integer slider). Returns true if the value changed this frame.
inline bool UiSlider(Rectangle r, float& value, float minV, float maxV,
                     bool& active, float step = 0.0f) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hovered) active = true;
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) active = false; // released anywhere

    float old = value;
    if (active && r.width > 0.0f) {
        float t = (GetMousePosition().x - r.x) / r.width;
        t = fminf(fmaxf(t, 0.0f), 1.0f);
        value = minV + t * (maxV - minV);
        if (step > 0.0f) value = minV + step * roundf((value - minV) / step);
        value = fminf(fmaxf(value, minV), maxV);
    }

    // Track (dimmed when idle, bright when hovered/dragging - like UiTextField).
    UiPanel(r, (hovered || active) ? ui::OUTLINE : Fade(ui::OUTLINE, 0.6f),
               (hovered || active) ? ui::FILL_HI : ui::FILL);
    // Filled portion up to the current value, then an opaque handle on top.
    float frac = (maxV > minV) ? (value - minV) / (maxV - minV) : 0.0f;
    frac = fminf(fmaxf(frac, 0.0f), 1.0f);
    if (frac > 0.0f)
        DrawRectangleRec({r.x, r.y, frac * r.width, r.height}, ui::FILL_HI);
    float hx = r.x + frac * r.width;
    DrawRectangleRec({hx - 3.0f, r.y - 3.0f, 6.0f, r.height + 6.0f}, ui::OUTLINE);
    return value != old;
}

//MARK: Centered text
// Reusable centered label (centers within [0, areaWidth]).
inline void UiTextCentered(const char* t, int areaWidth, int y, int fontSize, Color c) {
    DrawText(t, (areaWidth - MeasureText(t, fontSize)) / 2, y, fontSize, c);
}

//MARK: Modal chrome
// The shared frame every title-screen popup (CONTROLS, OPTIONS) draws: a dim
// full-screen backdrop + opaque modal panel + centered title. Call first, then
// draw the modal body, then UiModalClose() for the bottom CLOSE button. Sizes
// off the live window (GetScreenWidth/Height) so callers pass only the panel.
inline void UiModalChrome(Rectangle r, const char* title) {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.7f));
    UiModalPanel(r);
    UiTextCentered(title, GetScreenWidth(), (int)r.y + 20, 30, ui::OUTLINE);
}

// CLOSE button pinned bottom-center of modal panel `r`. `enabled` gates the click
// (callers pass the previous frame's open flag so the click that opened the modal
// can't immediately close it). Returns true on the frame it's clicked.
inline bool UiModalClose(Rectangle r, bool enabled) {
    Rectangle b = {GetScreenWidth() / 2.0f - 70.0f, r.y + r.height - 60.0f, 140.0f, 40.0f};
    return enabled && UiButton(b, "CLOSE");
}
