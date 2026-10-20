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
Close and reopen PowerShell afterwards so it picks up the new tools.

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
git checkout f4-steam-packaging
```

(Once PR #191 is merged, stay on `main` instead of checking out the branch.)

- **The SDK:** copy the whole `steamworks_sdk` folder from the Mac (it sits
  beside the PLATFORMZ folder there) to `C:\dev\steamworks_sdk`.
- **`secrets.mk`:** copy it from the Mac's PLATFORMZ folder into
  `C:\dev\PLATFORMZ\`. **Don't skip this.** It holds the server address and join
  key. Without it the Windows game builds fine but can't find the server. Move it
  by USB or your own Dropbox, not by email or chat, and never commit it.

## 3. Build

From `C:\dev\PLATFORMZ`:

```powershell
cmake --preset steam
cmake --build --preset steam
```

The first run downloads raylib and a couple of other libraries, so it's slow. The
game ends up at `build\cmake-steam\Release\platformz.exe`.

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
- [ ] **Online play works:** join a match on the server.

## 5. Package it

```powershell
pwsh -ExecutionPolicy Bypass -File packaging\windows\pack.ps1 -Steam
```

It checks the build and creates `dist\steam\windows\`. Copy that **whole folder**
to the Mac as `dist/steam/windows/` in the PLATFORMZ folder. After that,
`make steam-upload` sends both Mac and Windows to Steam.

## If something goes wrong

Save the error text. Steps 3 and 5 have never run on Windows, so a hiccup is
likely. The Windows game has no console window, so it can't show its own error
messages. If it fails to start, or Spacewar doesn't appear in Steam, note what
you saw: the next step is a build that writes its messages to a log file.
