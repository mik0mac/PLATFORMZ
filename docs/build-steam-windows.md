# Build the Steam edition on Windows

Step-by-step, for a Windows machine. It produces the Windows depot folder that
`make steam-upload` on the Mac sends to Steam (see `package-steam.md`). It
doubles as the Windows play test.

## 1. Install the tools (once)

Open **PowerShell** and run:

```powershell
winget install Git.Git
winget install Kitware.CMake
winget install Microsoft.PowerShell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

The last one is Microsoft's C++ compiler. It's a big download and takes a while.

**Close every PowerShell window and open a fresh one afterwards.** A window that
was open during the installs doesn't know the new tools exist, and says `git` is
not recognized. If you're in Windows Terminal or VS Code, quit the whole app.
Check with `git --version`.

## 2. Put the files in place

You need three things, with the game and the SDK **side by side** in the same folder:

```
C:\dev\PLATFORMZ\          <- the game
C:\dev\steamworks_sdk\     <- the Steam SDK, copied from the Mac
```

```powershell
mkdir C:\dev; cd C:\dev
git clone --recurse-submodules https://github.com/mik0mac/PLATFORMZ.git
cd PLATFORMZ
```

- **The SDK:** copy the whole `steamworks_sdk` folder from the Mac (it sits
  beside the PLATFORMZ folder there) to `C:\dev\steamworks_sdk`. Unzipping on
  Windows often adds an extra folder level; check that
  `Test-Path C:\dev\steamworks_sdk\public\steam\steam_api.h` says `True`.
- **`secrets.mk`:** copy it from the Mac's PLATFORMZ folder into
  `C:\dev\PLATFORMZ\`. **Don't skip this.** It holds the server address and join
  key. Without it the Windows game builds fine but can't find the server: the
  title screen says "OFFLINE - START WITH A SERVER URL". Move it by USB or your
  own Dropbox, not by email or chat, and never commit it.
  - **Copy it before step 3.** The build only looks for it when
    `cmake --preset steam` runs. If you copy it later, run that again.
  - Windows hides file extensions, so a copy can quietly become
    `secrets.mk.txt`. `Get-ChildItem secrets*` shows the real name;
    `Rename-Item secrets.mk.txt secrets.mk` fixes it.

## 3. Build

From `C:\dev\PLATFORMZ`:

```powershell
cmake --preset steam
cmake --build --preset steam
```

The first run downloads raylib and a couple of other libraries, so it's slow. The
game ends up at `build\cmake-steam\Release\platformz.exe`.

What to look for in the first command's output:

- **`secrets.mk: 2 client flag(s) baked in`.** It has to be there. If it's
  missing, the game will be offline: see `secrets.mk` in step 2.
- **`Steam edition: SDK at C:/dev/PLATFORMZ/../steamworks_sdk`.** The `..`
  means "one folder up", so that is `C:\dev\steamworks_sdk`, beside the game,
  not inside it.
- **`Could NOT find Python3`** is harmless. Ignore it.

## 4. Play test it (the important part)

Open Steam and log in, then double-click `build\cmake-steam\Release\platformz.exe`.
Check these:

- [ ] **Steam is connected:** your Steam friends list shows you playing
      **"Spacewar"**. That's Valve's test app, which stands in until PLATFORMZ has
      its own App ID.
- [ ] **Shift+Tab** opens the Steam overlay.
- [ ] **The window looks sharp,** not blurry, and **F** switches fullscreen on and
      off cleanly.
- [ ] **Sound works.**
- [ ] **Online play works:** join a match on the server. If Windows Firewall
      asks whether to allow `platformz`, allow it. If the title screen stays
      on "CONNECTING TO SERVER...", allow it under **Windows Security ->
      Firewall & network protection -> Allow an app through firewall**.

## 5. Package it

```powershell
pwsh -ExecutionPolicy Bypass -File packaging\windows\pack.ps1 -Steam
```

It checks the build and creates `dist\steam\windows\`. It reports
"(steam depot content, unsigned)". That's expected: there is no Windows
code-signing certificate yet, and Steam doesn't require one. Copy that **whole folder**
to the Mac as `dist/steam/windows/` in the PLATFORMZ folder. After that,
`make steam-upload` sends both Mac and Windows to Steam.

## If something goes wrong

Save the error text. The Windows game has no console window, so it can't show its own error
messages. If it fails to start, or Spacewar doesn't appear in Steam, note what
you saw: the next step is a build that writes its messages to a log file.
