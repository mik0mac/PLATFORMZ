// display.h
//
// Fullscreen (#24), and the fixed-size canvas that makes it possible.
//
// Every screen in the game is laid out in absolute pixels on a 1000x700 canvas -
// buttons, modals, the HUD, the 3D scene target. Rather than teach all of that
// to reflow, the game keeps drawing at 1000x700 and the canvas is SCALED to fit
// whatever it is shown on, with black bars on the sides that don't match
// (letterboxing). The two builds get there differently:
//
//   Native  - fullscreen makes the window the size of the monitor, so each frame
//             is drawn into an off-screen 1000x700 texture and then stretched
//             onto the real window. raylib's mouse offset/scale is set to the
//             inverse, so GetMousePosition() keeps answering in canvas pixels
//             and no button hit-test has to know any of this. A window that is
//             exactly the canvas size (the normal windowed case) skips the
//             texture and draws straight to the screen, as it always has.
//   Web     - the browser does the scaling. shell.html fullscreens a wrapper
//             around the canvas and CSS stretches the canvas inside it; its
//             backing store stays 1000x700, and emscripten already maps mouse
//             positions from CSS size to backing size. So BeginFrame/EndFrame
//             are plain BeginDrawing/EndDrawing there.
//
// The web F key is handled by shell.html itself, not here: a browser only lets a
// page go fullscreen from inside the key's own event handler, and the game reads
// its keys a frame later. The game's part is saying whether F is free to take
// (it isn't while a text field has focus) - see SetFullscreenKeyEnabled.

#pragma once

#include "raylib.h"
#include "rlgl.h"
#include <cmath> // fminf
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include <objc/runtime.h> // SetFullscreen: the NSWindow's own fullscreen
#include <objc/message.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
// (EM_JS defines a function: fine here because only main.cpp's translation unit
// includes display.h - the same arrangement as ui.h's paste hook.)
EM_JS(int, PlatformzIsFullscreen, (), {
    return (document.fullscreenElement || document.webkitFullscreenElement) ? 1 : 0;
});
EM_JS(void, PlatformzExitFullscreen, (), {
    if (window.Module && Module.exitFullscreen) Module.exitFullscreen();
});
EM_JS(void, PlatformzSetFullscreenKeyOk, (int ok), {
    if (window.Module) Module.fullscreenKeyOk = !!ok;
});
#endif

namespace display {

// The canvas every screen is laid out on. main.cpp's screenWidth/screenHeight
// are these; ui.h's modal chrome reads them in place of GetScreenWidth(), which
// reports the WINDOW and so would centre a modal on a monitor-sized window
// rather than on the canvas being drawn.
inline constexpr int CANVAS_W = 1000;
inline constexpr int CANVAS_H = 700;

#if defined(__EMSCRIPTEN__)

inline void ConfigFlags() {}
inline void Init() {}
inline void Shutdown() {}
inline bool IsFullscreen() { return PlatformzIsFullscreen() != 0; }
// Leaving fullscreen needs no gesture, so Esc can do it from the frame loop.
// (Usually the browser has already done it: Esc is its own exit key.)
inline void ExitFullscreen() { PlatformzExitFullscreen(); }
inline void SetFullscreenKeyEnabled(bool ok) { PlatformzSetFullscreenKeyOk(ok ? 1 : 0); }
inline void HandleFullscreenKey() {} // shell.html owns F on the web
inline void BeginFrame() { BeginDrawing(); }
inline void EndFrame()   { EndDrawing(); }

#else

namespace detail {
struct State {
    RenderTexture2D target{};       // the 1000x700 canvas, when letterboxing
    bool            letterbox = false; // this frame went through `target`
    Rectangle       dst{};          // where the canvas lands on the window
};
inline State& S() { static State s; return s; }
}

// Before InitWindow. The window is resizable because the letterbox makes any
// size work - and because macOS will not take a window fullscreen otherwise. As a
// bonus the green button (macOS) and maximize (Windows) now do what F does.
// Never on the web: there raylib reads RESIZABLE as "track the browser window"
// and would resize the canvas's backing store, which display.h relies on staying
// 1000x700.
inline void ConfigFlags() { SetConfigFlags(FLAG_WINDOW_RESIZABLE); }

// After InitWindow (a render texture needs a GL context).
inline void Init() {
    SetWindowMinSize(CANVAS_W / 2, CANVAS_H / 2);
    auto& s = detail::S();
    s.target = LoadRenderTexture(CANVAS_W, CANVAS_H);
    SetTextureFilter(s.target.texture, TEXTURE_FILTER_BILINEAR);
}
inline void Shutdown() { UnloadRenderTexture(detail::S().target); }

#if defined(__APPLE__)
// macOS: the system's own fullscreen - the green-button kind, with its own Space.
// Not raylib's ToggleFullscreen, which hands the window to the display as an
// exclusive video mode: on a Retina Mac GLFW finds no mode matching the scaled
// desktop and SWITCHES THE DISPLAY RESOLUTION (1470x956 -> 1920x1200 on a 14"
// MacBook), and raylib is then left drawing for one size into another. GLFW
// exposes no way to ask for the native kind, so it is asked of the NSWindow
// directly, through the Objective-C runtime (plain C calls - no .mm file).
// The switch animates; the window reports its new size over the next frames,
// and BeginFrame letterboxes whatever size it is at.
namespace detail {
inline void* Send(void* obj, const char* sel, unsigned long arg) {
    using Fn = void* (*)(void*, SEL, unsigned long);
    return ((Fn)objc_msgSend)(obj, sel_registerName(sel), arg);
}
inline unsigned long SendUL(void* obj, const char* sel) {
    using Fn = unsigned long (*)(void*, SEL);
    return ((Fn)objc_msgSend)(obj, sel_registerName(sel));
}
constexpr unsigned long NSWindowStyleMaskFullScreen = 1ul << 14;
}
inline bool IsFullscreen() {
    void* win = GetWindowHandle(); // the NSWindow on macOS
    return win && (detail::SendUL(win, "styleMask") & detail::NSWindowStyleMaskFullScreen);
}
inline void SetFullscreen(bool on) {
    void* win = GetWindowHandle();
    if (!win || on == IsFullscreen()) return;
    // AppKit ignores toggleFullScreen: on a window that cannot be resized -
    // which is why ConfigFlags() asks for a resizable one.
    detail::Send(win, "toggleFullScreen:", 0);
}
#else
// Windows/Linux: borderless windowed - the window is undecorated and laid over
// the whole monitor at the desktop's own resolution, so nothing changes video
// mode and alt-tab stays instant. raylib remembers and restores the old frame.
inline bool IsFullscreen() { return IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE); }
inline void SetFullscreen(bool on) {
    if (on != IsFullscreen()) ToggleBorderlessWindowed();
}
#endif
inline void ExitFullscreen() { SetFullscreen(false); }
inline void SetFullscreenKeyEnabled(bool) {}
// F toggles. The caller decides whether F is free (not typing into a field).
inline void HandleFullscreenKey() {
    if (IsKeyPressed(KEY_F)) SetFullscreen(!IsFullscreen());
}

// In place of BeginDrawing/EndDrawing for every frame the game shows. Mouse
// mapping is set here, ahead of the draw, because the menus hit-test their
// buttons while drawing them (ui.h is immediate mode).
inline void BeginFrame() {
    auto& s = detail::S();
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    s.letterbox = (sw != CANVAS_W || sh != CANVAS_H) && s.target.id != 0;
    if (!s.letterbox) {
        SetMouseOffset(0, 0);
        SetMouseScale(1.0f, 1.0f);
        BeginDrawing();
        return;
    }
    const float k = fminf((float)sw / CANVAS_W, (float)sh / CANVAS_H);
    s.dst = {((float)sw - CANVAS_W * k) / 2.0f, ((float)sh - CANVAS_H * k) / 2.0f,
             CANVAS_W * k, CANVAS_H * k};
    // raylib maps the mouse as (raw + offset) * scale.
    SetMouseOffset(-(int)s.dst.x, -(int)s.dst.y);
    SetMouseScale(1.0f / k, 1.0f / k);
    BeginTextureMode(s.target);
}

inline void EndFrame() {
    auto& s = detail::S();
    if (!s.letterbox) { EndDrawing(); return; }
    EndTextureMode();
    BeginDrawing();
        ClearBackground(BLACK);
        // Copy, don't blend. Translucent fills drawn into the canvas leave its
        // alpha below 255 (raylib blends alpha like colour), and an ordinary
        // alpha-blended blit would then darken exactly those pixels against the
        // black bars' clear colour.
        rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
        BeginBlendMode(BLEND_CUSTOM);
            // Render textures are stored Y-flipped: negative source height.
            DrawTexturePro(s.target.texture, {0, 0, (float)CANVAS_W, -(float)CANVAS_H},
                           s.dst, {0, 0}, 0.0f, WHITE);
        EndBlendMode();
    EndDrawing();
}

#endif

} // namespace display
