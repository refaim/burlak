# Burlak

Far Manager plugin: drag files out of the panel into any Windows drop target — Telegram, a
browser upload box, Explorer, a chat window.

Hold the left mouse button on a panel item, move a few cells, drop it wherever you like.
Ordinary clicks keep working as before.
Drop onto Far's other panel to copy there; hold Shift to move instead, including to or from plugin panels.
To drop into another Far, both Fars need Burlak and the same elevation level; if several Fars share one Windows Terminal window, the drop goes to the tab that most recently had focus.
Files and selections can also be dragged out of archive, FTP/SFTP, and other plugin panels;
after release they are extracted as copies for the target, so these plugin-panel drags do not offer shortcuts.
Files dragged out of a plugin panel are extracted into `%TEMP%\Burlak` and removed about ten minutes later, giving the receiving program time to read them.

Drag with the right button instead and it is up to the target what happens on the drop:
Explorer offers its copy / move / shortcut menu, Telegram just takes the files. A right click
still selects a file as before; sweeping over several files with the right button held now
starts a drag instead of selecting them.

Works in the standard console (cmd.exe / conhost), in OpenConsole and in Windows Terminal.

## Install

Take the archive for your Far build from [Releases](../../releases) and unpack the `Burlak`
folder into Far's `Plugins` directory:

```
Far Manager\Plugins\Burlak\Burlak.dll
```

Restart Far — plugins are read at startup.

## Build

```powershell
cmake --preset release-x64
cmake --build --preset release-x64
pwsh -File scripts/build.ps1 -Arch x86
pwsh -File scripts/build.ps1 -Arch arm64
pwsh -File scripts/package.ps1 -Arch x64  # Burlak-<version>-x64.zip, as on Releases
```

Needs the Visual Studio 2022 Build Tools with the C++ workload; arm64 also needs the
`MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools` component. Far's headers ship in `sdk/`.

The `debug` and `coverage` presets use clang-cl and Ninja. Run the tests and both local gates with:

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
powershell -File scripts/coverage.ps1
powershell -File scripts/lint.ps1 -BuildDir build/debug `
  -ReleaseDirs build/release-x64,build/release-x86,build/release-arm64
```

Coverage accepts only 100% line and branch coverage for every executable file under `src/`.
Lint runs clang-format, clang-tidy, cppcheck, PSScriptAnalyzer and BinSkim.

## Licence

MIT — see [LICENSE](LICENSE).

`sdk/` holds Far Manager's plugin headers (© 1996 Eugene Roshal, © 2000 Far Group), redistributed
under their own BSD-3-clause licence.

Thanks to [karbazol/far-drag-n-drop-plugin](https://github.com/karbazol/far-drag-n-drop-plugin),
whose approach this borrows.
