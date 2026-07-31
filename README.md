# Burlak

Drag files out of [Far Manager](https://farmanager.com/)'s panel into anything that accepts a
drop — Telegram, a browser upload box, Explorer, a chat window.

Hold the left mouse button on a panel item, move a few cells, and the shell takes over from
there. Ordinary clicks are untouched: nothing is intercepted until the drag threshold is
actually crossed.

Named after the [burlaks](https://en.wikipedia.org/wiki/Barge_haulers_on_the_Volga) who hauled
barges along the riverbank — the name is about dragging, not about direction, so it still fits
if dropping *into* Far is ever added.

## Install

Download the archive for your Far build from
[Releases](../../releases), and drop the `Burlak` folder into Far's `Plugins` directory:

```
Far Manager\Plugins\Burlak\Burlak.dll
```

Restart Far. Plugins are read at startup, so a running instance will not pick it up.

## Why it is built this way

Far is a console application, and that makes an OLE drag surprisingly awkward. Measured from
inside a plugin on Far's own thread:

- `SetWindowLongPtr(GWLP_WNDPROC)` on the console window fails with `ERROR_ACCESS_DENIED` — the
  console window cannot be subclassed, not even by its own process.
- Thread-local `WH_GETMESSAGE` and `WH_CALLWNDPROC` hooks install fine and then see **zero**
  mouse messages, while `ProcessConsoleInputW` delivers thousands over the same gesture. This
  holds with `ENABLE_MOUSE_INPUT` both on and off.

Mouse input goes to the console input buffer and never reaches the window layer, so
`DoDragDrop` — which runs a modal message loop — has nothing to consume on Far's main thread.

It can, however, run on a *different* thread in the same process. The gesture is re-founded on
a thread that is allowed to own it:

1. Far's main thread spots the drag in `ProcessConsoleInputW`.
2. It synthesises `MOUSEEVENTF_LEFTUP`, ending the console's idea of the gesture.
3. A tool window — owned by a thread of our own with a normal message pump — is raised over Far
   and takes the mouse capture. It is layered at alpha 1: present for hit-testing, invisible in
   practice.
4. `MOUSEEVENTF_LEFTDOWN` is synthesised. The press lands on the tool window, so that thread
   now legitimately owns the mouse.
5. That thread runs `SHDoDragDrop` and pumps its own messages, leaving Far's main thread free.

The approach is taken from [karbazol/far-drag-n-drop-plugin](https://github.com/karbazol/far-drag-n-drop-plugin),
which solved this years ago and is worth reading if you want the drop-*into*-Far direction too.

No COM interfaces are implemented by hand. The data object comes from
`SHCreateShellItemArrayFromIDLists` + `BindToHandler(BHID_DataObject)`, so targets receive the
full set of shell formats rather than a bare `CF_HDROP`; `SHDoDragDrop` supplies the shell's
default `IDropSource` and drag image when passed `nullptr`.

## Building

Needs the Visual Studio 2022 Build Tools with the C++ workload and a Windows SDK. Far's plugin
headers are vendored in `sdk/`, so nothing else has to be installed.

```powershell
pwsh -File build.ps1                # x64 -> build/x64/Burlak.dll
pwsh -File build.ps1 -Arch x86
pwsh -File build.ps1 -Arch arm64    # cross-compiled from an x64 host
```

arm64 needs the `MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools` component; the other two
need only the x64/x86 compilers.

CI runs the very same script for all three architectures, so local and released builds cannot
drift apart.

## Releasing

`src/version.h` is the single source of truth for the version — it feeds the plugin's Far
version, the DLL resource and the release check.

1. Bump `BURLAK_VERSION_*` in `src/version.h`.
2. Commit.
3. Tag `vMAJOR.MINOR.PATCH` and push the tag.

Pushing a `v*` tag builds all three architectures, refuses to publish if the tag and the header
disagree, and attaches the archives to a GitHub release:

```
Burlak-1.0.0-x64.zip      Burlak/Burlak.dll, README.md, LICENSE
Burlak-1.0.0-x86.zip
Burlak-1.0.0-arm64.zip
```

## Known edges

- The drag threshold is fixed at 3 cells (`DRAG_THRESHOLD_CELLS`).
- The tool-window thread starts lazily on the first drag, so the very first gesture after Far
  starts may need a moment.
- Plugin panels (archives, FTP) are skipped: their items have no path the shell could resolve.

## Licence

MIT — see [LICENSE](LICENSE).

`sdk/` contains Far Manager's plugin headers (Copyright © 1996 Eugene Roshal, © 2000 Far Group),
redistributed under their own BSD-3-clause-style licence; the terms are in the file headers.
