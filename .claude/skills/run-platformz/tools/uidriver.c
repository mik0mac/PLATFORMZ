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
//   uidriver click  <x> <y>                 screen POINTS, not pixels
//   uidriver key    <keycode>               a bare key
//   uidriver chord  <modkeycode> <keycode> <cmd|ctrl>
//   uidriver scroll <x> <y> <lines>         + is up/away, - is down/toward
//   uidriver drag   <x1> <y1> <x2> <y2>     press, move in steps, release
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
    if (argc < 2) { fprintf(stderr, "usage: uidriver trusted|click|key|chord|scroll|drag ...\n"); return 2; }

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

    // The wheel goes to whatever is UNDER THE POINTER, not to the focused
    // window, so the move first is not optional - and a scroll-aware widget
    // usually gates on the pointer being over it anyway.
    if (strcmp(argv[1], "scroll") == 0 && argc >= 5) {
        CGPoint p = CGPointMake(atof(argv[2]), atof(argv[3]));
        CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, mv); CFRelease(mv);
        usleep(120000);
        CGEventRef sc = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1,
                                                      (int32_t)atoi(argv[4]));
        // A scroll event carries no location of its own; without this it lands
        // wherever the system last thought the pointer was.
        CGEventSetLocation(sc, p);
        CGEventPost(kCGHIDEventTap, sc); CFRelease(sc);
        usleep(90000);
        return 0;
    }

    // Press, MOVE IN STEPS, release. A single jump from press to release is not
    // a drag: an immediate-mode widget samples the mouse once a frame, so it
    // would see the button go down and up with no motion between and treat the
    // whole thing as a click.
    if (strcmp(argv[1], "drag") == 0 && argc >= 6) {
        double x1 = atof(argv[2]), y1 = atof(argv[3]);
        double x2 = atof(argv[4]), y2 = atof(argv[5]);
        CGPoint a = CGPointMake(x1, y1);
        CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, a, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, mv); CFRelease(mv);
        usleep(120000);
        CGEventRef dn = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, a, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, dn); CFRelease(dn);
        usleep(120000);
        for (int i = 1; i <= 12; ++i) {
            CGPoint q = CGPointMake(x1 + (x2 - x1) * i / 12.0, y1 + (y2 - y1) * i / 12.0);
            CGEventRef dg = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDragged, q, kCGMouseButtonLeft);
            CGEventPost(kCGHIDEventTap, dg); CFRelease(dg);
            usleep(25000);
        }
        CGPoint b = CGPointMake(x2, y2);
        CGEventRef up = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, b, kCGMouseButtonLeft);
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
