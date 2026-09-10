#!/usr/bin/env python3
"""
Web half of the profile test: proves profile.h's localStorage path actually
round-trips.

The trick is that it does not retype any JavaScript. It lifts the EM_JS bodies
emcc emitted, plus the emscripten string helpers they call, straight out of
web/platformz.js and runs THOSE under node against a fake heap and a localStorage
shim. So it tests what ships, and it goes stale the moment the web build does.

    make web && python3 test/web_profile_test.py

Requires an existing web build (web/platformz.js) and node. Skips, loudly, if
either is missing - a browser is never required.
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLUE = os.path.join(ROOT, "web", "platformz.js")

# Everything the two EM_JS bodies transitively need. Order matters: these are
# `var x = <arrow fn>` statements, so a use before its assignment is a TypeError.
HELPERS = ["UTF8Decoder", "findStringEnd", "UTF8ArrayToString", "UTF8ToString",
           "lengthBytesUTF8", "stringToUTF8Array", "stringToUTF8"]
EM_JS = ["PlatformzProfileRead", "PlatformzProfileWrite"]


def grab_var(src, name):
    """Lift a minified `var name=<expr>;` statement, bracket-balanced."""
    i = src.index("var %s=" % name)
    depth = 0
    for k in range(i, len(src)):
        c = src[k]
        if c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
        elif c == ";" and depth == 0:
            return src[i:k + 1]
    raise SystemExit("unterminated statement for %s" % name)


def grab_fn(src, name):
    """Lift a `function name(...){...}` declaration, brace-balanced."""
    i = src.index("function %s(" % name)
    depth, k = 0, src.index("{", i)
    while True:
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
        if depth == 0:
            return src[i:k + 1]
        k += 1


CHECKS = r"""
var store = {}, throwing = false;
global.window = { localStorage: {
    getItem: k => { if (throwing) throw new Error('SecurityError'); return (k in store) ? store[k] : null; },
    setItem: (k, v) => { if (throwing) throw new Error('QuotaExceededError'); store[k] = String(v); }
}};

// profile.h's ReadRaw()/WriteRaw(), transcribed - so the C-side buffer handling
// (notably the grow-and-retry when the stored value outgrows the first guess) is
// under test too, not just the JavaScript.
//
// base is deliberately NOT 0: UTF8ToString treats a null pointer as the empty
// string, and a real std::string's data() never is one.
function ReadRaw() {
    var cap = 4096, base = 1024;
    var need = PlatformzProfileRead(base, cap);
    if (need < 0) return "";
    if (need > cap) {
        cap = need;
        need = PlatformzProfileRead(base, cap);
        if (need < 0 || need > cap) return "";
    }
    return UTF8ToString(base);
}
function WriteRaw(str) {
    var p = 65536;
    stringToUTF8(str, p, 65536);
    return PlatformzProfileWrite(p) !== 0;
}

var fails = 0;
const check = (c, w) => { console.log((c ? '  ok:   ' : '  FAIL: ') + w); if (!c) fails++; };

console.log('empty store');
check(ReadRaw() === "", 'absent key reads as empty (first launch)');

console.log('\nround trip');
var payload = JSON.stringify({version:1, name:"MIKE",
                              clientId:"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
                              token:"", volumeDb:-12.5, options:{map:"XL", players:6}});
check(WriteRaw(payload), 'write succeeds');
check(store['platformz.profile'] === payload, 'localStorage holds exactly what was written');
check(ReadRaw() === payload, 'read returns exactly what was written');

console.log('\nvalue larger than the first buffer guess');
var big = JSON.stringify({name:"X".repeat(6000)});
store['platformz.profile'] = big;
check(ReadRaw() === big, 'grow-and-retry returns the full value (>4096 bytes)');
check(ReadRaw().length === big.length, 'nothing is truncated');

console.log('\nmulti-byte content (UTF-8 bytes != JS string length)');
var uni = JSON.stringify({name:"MIKÉÉÉÉ"});
store['platformz.profile'] = uni;
check(ReadRaw() === uni, 'lengthBytesUTF8 sizes in bytes, not characters');

console.log('\nstorage disabled (Safari private window throws on both calls)');
throwing = true;
check(ReadRaw() === "", 'a throwing getItem reads as empty; the exception never escapes');
check(WriteRaw(payload) === false, 'a throwing setItem reports failure; the exception never escapes');
throwing = false;

console.log('\n' + (fails ? 'FAILURES' : 'all web-storage checks passed'));
process.exit(fails ? 1 : 0);
"""


def main():
    if not os.path.exists(GLUE):
        print("SKIP: no web build - run `make web RAYLIB_WEB_DIR=$HOME/raylib` first")
        return 0
    if subprocess.call(["which", "node"], stdout=subprocess.DEVNULL) != 0:
        print("SKIP: node not installed")
        return 0

    src = open(GLUE).read()
    for name in EM_JS:
        if "function %s(" % name not in src:
            print("FAIL: %s missing from the web build - did profile.h's EM_JS "
                  "get dropped?" % name)
            return 1

    parts = ["// Generated by test/web_profile_test.py from web/platformz.js.",
             "// Every line below the shim is emcc's own output, lifted verbatim.",
             "var HEAPU8 = new Uint8Array(1 << 17);",
             "function abort(w) { throw new Error('abort: ' + w); }"]
    parts += [grab_var(src, h) for h in HELPERS]
    parts += [grab_fn(src, f) for f in EM_JS]
    parts.append(CHECKS)

    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write("\n".join(parts))
        path = f.name
    try:
        return subprocess.call(["node", path])
    finally:
        os.unlink(path)


if __name__ == "__main__":
    sys.exit(main())
