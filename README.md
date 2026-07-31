# Burlak

Far Manager plugin: drag files out of the panel into any Windows drop target — Telegram, a
browser upload box, Explorer, a chat window.

Hold the left mouse button on a panel item, move a few cells, drop it wherever you like.
Ordinary clicks keep working as before.

## Install

Take the archive for your Far build from [Releases](../../releases) and unpack the `Burlak`
folder into Far's `Plugins` directory:

```
Far Manager\Plugins\Burlak\Burlak.dll
```

Restart Far — plugins are read at startup.

## Build

```powershell
pwsh -File build.ps1                # x64
pwsh -File build.ps1 -Arch x86
pwsh -File build.ps1 -Arch arm64
```

Needs the Visual Studio 2022 Build Tools with the C++ workload; arm64 also needs the
`MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools` component. Far's headers ship in `sdk/`.

## Licence

MIT — see [LICENSE](LICENSE).

`sdk/` holds Far Manager's plugin headers (© 1996 Eugene Roshal, © 2000 Far Group), redistributed
under their own BSD-3-clause licence.

Thanks to [karbazol/far-drag-n-drop-plugin](https://github.com/karbazol/far-drag-n-drop-plugin),
whose approach this borrows.
