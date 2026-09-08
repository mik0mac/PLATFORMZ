// uidriver - post real mouse/keyboard events to the PLATFORMZ window on macOS.
//
// WHY THIS EXISTS. `osascript -e 'tell application "System Events" to ...'`
// looks like it should do this job and cannot:
//
//   * `keystroke "v"` posts a unicode CHARACTER, not a key code. raylib's
//     GetCharPressed() sees it, IsKeyPressed() never does - so any code gated on
//     IsKeyPressed (paste chords, hotkeys) looks broken when it is fine.
//   * `click at {x, y}` needs an entitlement the terminal usually lacks and
//     fails with -25211 even when Accessibility is granted.
//
// CGEventPost avoids both. Requires Accessibility for the terminal running
// Claude Code (usually Visual Studio Code) - `uidriver trusted` reports 1/0.
//
//   uidriver trusted
//   uidriver click <x> <y>                  screen POINTS, not pixels
//   uidriver key   <keycode>                a bare key
//   uidriver chord <modkeycode> <keycode> <cmd|ctrl>
//
// Useful key codes: V 9 · TAB 48 · RETURN 36 · DELETE 51 · ESC 53 ·
//                   CMD 55 · SHIFT 56 · CTRL 59 · A 0 · M 46
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void postKey(CGKeyCode kc, bool down, CGEventFlags fl) {
    CGEventRef e = CGEventCreateKeyboardEvent(NULL, kc, down);
    CGEventSetFlags(e, fl);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
    usleep(90000);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: uidriver trusted|click|key|chord ...\n"); return 2; }

    if (strcmp(argv[1], "trusted") == 0) { printf("%d\n", AXIsProcessTrusted()); return 0; }

    if (strcmp(argv[1], "click") == 0 && argc >= 4) {
        CGPoint p = CGPointMake(atof(argv[2]), atof(argv[3]));
        CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, mv); CFRelease(mv);
        usleep(120000);
        CGEventRef dn = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, p, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, dn); CFRelease(dn);
        usleep(90000);
        CGEventRef up = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, up); CFRelease(up);
        return 0;
    }

    if (strcmp(argv[1], "key") == 0 && argc >= 3) {
        CGKeyCode kc = (CGKeyCode)atoi(argv[2]);
        postKey(kc, true, 0); postKey(kc, false, 0);
        return 0;
    }

    // A chord must press the MODIFIER KEY ITSELF first. GLFW registers a
    // modifier from that key's own event, so CGEventSetFlags alone leaves
    // IsKeyDown(KEY_LEFT_SUPER) false and the chord silently does nothing.
    if (strcmp(argv[1], "chord") == 0 && argc >= 5) {
        CGKeyCode mod = (CGKeyCode)atoi(argv[2]);
        CGKeyCode key = (CGKeyCode)atoi(argv[3]);
        CGEventFlags fl = strcmp(argv[4], "cmd") == 0 ? kCGEventFlagMaskCommand
                                                      : kCGEventFlagMaskControl;
        postKey(mod, true,  fl);
        postKey(key, true,  fl);
        postKey(key, false, fl);
        postKey(mod, false, 0);
        return 0;
    }

    fprintf(stderr, "bad arguments\n");
    return 2;
}
