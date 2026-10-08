# Package a signed Mac app

Commands only - the reasoning is in the Makefile's macOS handout section. It
builds whatever is checked out, with the server host and key from `secrets.mk`
baked in, and replaces `dist/PLATFORMZ.app` every time.

## Once per Mac

- `ls ~/raylib-macos13/build-mac/raylib/libraylib.a` - raylib built for macOS 13 (`-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0`), not Homebrew's
- `security find-identity -v -p codesigning` - must list `Developer ID Application: Michael MacAllister (9WM486296X)`
- `xcrun notarytool store-credentials "platformz-notary"` - Apple ID, Team ID `9WM486296X`, app-specific password; type it yourself, never script it
- `xcrun notarytool history --keychain-profile platformz-notary` - a 403 "agreement is missing or has expired" means accept the latest agreement at developer.apple.com/account

## Build

- `git checkout main && git pull`
- `pgrep -lf PLATFORMZ.app` - quit any running copy first
- `make sign` -> `dist/PLATFORMZ.app` - Developer ID signed, not notarized; fine for testing on this Mac
- `make dist-pack` -> `dist/PLATFORMZ-mac-arm64.zip` - signed, notarized and stapled; the one to hand out (needs Apple's servers, 1-5 min)
- `make pack-unsigned` -> `dist/PLATFORMZ-UNSIGNED-mac-arm64.zip` - no credentials needed; this Mac only

## Verify

- `codesign -dv --verbose=2 dist/PLATFORMZ.app 2>&1 | grep Authority` - first line is `Developer ID Application: ...`; `adhoc` in the flags means unsigned
- `xcrun stapler validate dist/PLATFORMZ.app` - `dist-pack` only
- `spctl -a -vvv -t exec dist/PLATFORMZ.app` - `source=Notarized Developer ID`, `dist-pack` only
- `open dist/PLATFORMZ.app`

## Afterwards

- `make -B` - the packaging build left `./platformz` as the handout flavour (static raylib, macOS 13); this rebuilds the dev one
