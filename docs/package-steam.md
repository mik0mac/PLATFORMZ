# Package and upload the Steam edition

Commands only - the reasoning is in the Makefile's Steam depot section and the
headers of `packaging/steam/*.sh`. The Mac half uses the same signing setup as
`package-mac.md`; do that page's "Once per Mac" first.

Each platform's depot is a folder under `dist/steam/` (`mac/`, `windows/`).
SteamCMD uploads the folders as they are, with no zip. Neither may ever contain
`steam_appid.txt`.

## Once, on partner.steamgames.com

- Create the app. Note its **App ID**, and create two depots: **macOS** and **Windows**
- Launch options: macOS -> `PLATFORMZ.app`; Windows -> `platformz.exe`
- Put the three numbers in the Makefile: `STEAM_APP_ID`, `STEAM_DEPOT_MAC`, `STEAM_DEPOT_WIN` (not secrets; commit them)
- Make a `beta` branch for testing uploads before the default branch
- `-DPLATFORMZ_STEAM_APPID=<App ID>` replaces 480 (Spacewar) for CMake dev runs

## Once per Mac

- The Steamworks SDK unzipped beside the checkout as `../steamworks_sdk` - never inside it
- Everything under "Once per Mac" in `package-mac.md`

## Build

- `git checkout main && git pull`
- `make steam-depot` -> `dist/steam/mac/PLATFORMZ.app` - signed, notarized, then checked by `check-depot.sh` (needs Apple's servers, 1-5 min)
- `make steam-app` -> the same bundle unsigned; this Mac only, and the check refuses it
- Windows (on a Windows machine, step by step in `build-steam-windows.md`): `cmake --preset steam && cmake --build --preset steam`, then `pwsh packaging/windows/pack.ps1 -Steam` -> `dist\steam\windows`. Copy that folder to `dist/steam/windows` on the Mac

## Upload

- `make steam-upload STEAM_USER=<login> STEAM_PREVIEW=1` - dry run: SteamCMD scans the content and uploads nothing
- `make steam-upload STEAM_USER=<login> STEAM_BRANCH=beta` - upload and set live on `beta`
- SteamCMD asks for the password and the Steam Guard code itself; the first run also updates SteamCMD, which takes a minute
- Players get it when you set the build live on the **default** branch, by hand, on partner.steamgames.com (Builds page)

## Verify

- `packaging/steam/check-depot.sh` - re-runs every check on `dist/steam/` (`upload` always runs it first)
- In Steam: switch to the `beta` branch (Properties -> Betas), install, launch
- Shift+Tab opens the overlay - which also proves Steam started the game with its library. A friend sees "Join game" while you are in a room
