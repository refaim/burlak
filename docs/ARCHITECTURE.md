# Burlak — architecture

Status: v1 design, 2026-09-13. Owner: orchestrator (Claude). Implementers: Codex agents.
Reviewers: Codex review runs. The rules that constrain this design live in `AGENTS.md`; if the two
disagree, stop and report. Interface shapes below are canonical in intent, not in every
signature: keep the names and the responsibilities, adjust parameter lists when the code shows a
better cut, and say so in the report.

## 0. What Burlak is, and where it is going

A Far Manager 3 plugin, one DLL per architecture (x64, x86, arm64), that turns a mouse gesture on
a panel item into an OLE drag-and-drop of the selected files into any Windows drop target.
Today (1.2.0) it does this from panels with real file names, with the left or the right button.

Three features are next, and the layering below is judged against them:

1. **Drag from plugin panels** (Arclite, Observer, NetBox, ...). The panel has no paths on disk.
   At the threshold the plugin creates zero-byte placeholder files with the item names in a temp
   folder and starts the drag with them; when the button is released over a willing target
   (`IDropSource::QueryContinueDrag` is where OLE asks us whether to drop), it extracts the real
   files over the placeholders by calling the owning plugin's `GetFilesW` on Far's main thread
   (`PanelInfo::PluginHandle` is exactly the `hPanel` Far passes; `OwnerGuid` names the plugin;
   `PCTL_FINDPLUGIN` + `PCTL_GETPLUGININFORMATION` give its module and `GlobalInfo::Instance`),
   exactly as Far itself does for plugin-to-plugin copy (`FileList::PluginGetFiles`, `OPM_SILENT`
   into a temp directory), then answers "drop". The call is wrapped in SEH so a crashing plugin
   aborts the drag with a message instead of taking Burlak down. Temp files are removed at the
   next start and at exit, and opportunistically after the drag (targets read them after the
   drop, some asynchronously).
2. **Drop into the other panel of the same Far.** The tool window that covers the terminal during
   a drag registers an `IDropTarget`. Over the panel that is not the source it offers
   copy (move with Shift); elsewhere nothing. On drop it replays Far's own panel-to-panel mouse
   drag through `WriteConsoleInputW`: a left press at the cell the gesture started on, a release
   at the drop cell. Far's native `KEY_DRAGCOPY`/`KEY_DRAGMOVE` path does the copy, including
   plugin panels on either side. Burlak copies nothing itself here.
3. **Drop into another Far window.** A drag broadcasts "Burlak is dragging"; every other Burlak
   answers with the window of its terminal (`GetConsoleWindow()` under conhost; under Windows
   Terminal `GetWindow(GetConsoleWindow(), GW_OWNER)`, verified on WT 1.24) and the time it last
   had console focus (several Fars in one WT window share the owner; the most recently focused
   pane is the visible one). At release, if the window under the cursor belongs to a peer, the
   source cancels the OLE drop (so the terminal does not paste the path into the command line)
   and hands the peer the paths, the screen point and the effect over `WM_COPYDATA`. The peer
   maps the point to a panel and copies with `IFileOperation` into that panel's directory, then
   `FCTL_UPDATEPANEL`/`FCTL_REDRAWPANEL`. Right-button menus are shown by the source (it holds
   the foreground); the chosen effect travels with the message.

None of these needs hooks, injected DLLs or a drop target on the terminal's own window (which
belongs to another process and already has one).

## 1. Layers and the dependency rule

```
burlak/
  src/core/       decisions. Pure C++: no <windows.h>, no Far headers, no COM. Own value types.
  src/adapters/   one-to-one wrappers over foreign APIs, no decisions:
    far/          PluginStartupInfo (panels, windows, synchro, plugins control, messages),
                  the call into another plugin's GetFilesW (with its SEH guard)
    win/          user32 / kernel32: cursor, buttons, windows, console geometry and input,
                  input injection, temp files, focus, broadcast / WM_COPYDATA transport
    shell/        shell32 / ole32: data object from paths, the drag loop, IFileOperation
  src/drag/       the tool window and its thread: window class, message loop, DragSource
                  (IDropSource), DropTarget (IDropTarget). Win32/COM-facing by nature, but every
                  decision is asked from core.
  src/plugin/     the C ABI: Exports.cpp (marshalling + firewall), the composition root that wires
                  real adapters into core, version.h, Burlak.def, Burlak.rc
  tests/          core/ adapters/ drag/ plugin/ e2e/ guard/   (doctest)
  third_party/doctest/   the single header, vendored, unmodified
  sdk/            Far's plugin headers, vendored verbatim
  scripts/        coverage.ps1 lint.ps1 package.ps1
  docs/           this file
```

Dependencies point inward: `plugin` → `drag`, `adapters`, `core`; `drag` → `core` (and the Win32
it needs); `adapters` → `core` (for the interfaces they implement) and their foreign API; `core`
→ nothing but the standard library. `sdk/plugin.hpp` is included only under `src/adapters/far/`
and `src/plugin/`. `<windows.h>` is included under `src/adapters/`, `src/drag/` and `src/plugin/`,
never under `src/core/`. A guard test scans the sources and fails on a violation.

### core

Value types (`src/core/Types.hpp`): `Cell {int x, y}`, `CellRect`, `Point {int x, y}` (pixels),
`PixelRect`, `Button {Left, Right}`, `MouseEvent {Cell at; bool left, right, moved, wheel; Modifiers
mods}`, `Effect {None, Copy, Move}`, `PanelSide {Active, Passive}`, `PanelInfo {bool visible,
realNames, plugin; CellRect rect; ...}`, `Item {std::wstring name; std::uint64_t size; bool
directory; UserData}`.

Interfaces core depends on (all pure virtual, all under `src/core/`, implemented under
`src/adapters/`):

- `IPanels` — `panel(PanelSide)`, `selectedItems(PanelSide)`, `directory(PanelSide)`,
  `currentWindowIsPanels()`, `updateAndRedraw(PanelSide)`.
- `IFarHost` — `postSynchro()`, `message(title, lines)`, `pluginModule(guid)` (module path and
  `GlobalInfo::Instance`), `extract(hPanel, items, module, destination)` (the `GetFilesW` call,
  SEH-guarded, returns `std::expected<void, Error>`).
- `IScreen` — `cursor()`, `buttonDown(Button)`, `hostWindow()` (rect + handle, the terminal
  window under the cursor or owning the console), `cellGeometry()` (pixel size of a cell and the
  origin of cell (0,0), for point↔cell mapping).
- `IInput` — `release(Button)`, `press(Button)` (mouse_event forwarders), `replay(std::span<const
  MouseEvent>)` (WriteConsoleInputW).
- `IShell` — `makeDataObject(paths)`, `runDrag(...)` (SHDoDragDrop with our IDropSource), `copy
  (paths, destination, Effect)` (IFileOperation).
- `IFiles` — temp directory for this run, `placeholder(name, directory)`, `removeTree`, `sweep
  (older runs)`.
- `IPeers` — `announce()`, `peers()` (window, last-focus time, process), `send(peer, Drop)`; the
  transport is the adapter's business.

Core logic:

- `Gesture` (exists today): the state machine over `MouseEvent`s, returning `Verdict {Pass, Hold,
  Replace(MouseEvent)}` and, through a callback or an out-value, the request to start a drag with
  a button. Same behaviour as 1.2.0; the tests pin it.
- `Geometry`: cell ↔ pixel mapping, "which panel is this cell on", the item-row test that today
  lives in `InsidePanel`.
- `DragPlan`: given the source panel, decides the source kind (real paths vs placeholders + the
  extraction recipe) and builds the path list; today's `SelectedPaths` plus the TmpPanel case
  (an absolute `FileName` is taken as is).
- `ReleasePolicy`: given the point of release, the last feedback effect, the peer list and the
  own host window, decides one of `DropHere`, `Cancel`, `HandToPeer(peer, effect)`,
  `ExtractThenDrop` (feature 1) — the single place `DragSource::QueryContinueDrag` consults.
- `DropPolicy`: for the own `IDropTarget`: effect for a point (feature 2) and the replay records.
- `PeerRegistry` and the wire format of the peer protocol (feature 3), as plain structs and
  encode/decode functions.
- `Session`: the object that owns one drag from threshold to cleanup and sequences the calls to
  the interfaces above; the composition root creates it with the real adapters, the tests with
  fakes.

### adapters

One function per foreign call, no branches beyond turning a failure into `std::expected`/`bool`.
`adapters/far/PluginCall.cpp` holds the one `__try/__except` in the tree, in a function with no
C++ objects, and nothing else. `adapters/win/Console.cpp` reads cell geometry: under conhost from
`GetCurrentConsoleFont`/`GetConsoleScreenBufferInfo` and the window's client origin; under a
ConPTY host from the client rect divided by the buffer's window size (an error of a few pixels is
fine for hitting a panel).

### drag

`ToolWindow` (the class, the thread, the message loop, the layered 1-alpha popup), `DragSource`
(`IDropSource`: `QueryContinueDrag` asks `ReleasePolicy`; `GiveFeedback` records the last effect),
`DropTarget` (`IDropTarget` registered on the tool window: asks `DropPolicy`). Messages between
Far's thread and the tool thread stay explicit (`WM_PREPARE_DRAG`, `WM_START_DRAG`,
`WM_ABORT_DRAG`, the arm timer) and are the only cross-thread traffic; Far's API is called on
Far's thread only, which the tool thread reaches through `IFarHost::postSynchro`.

### plugin

`Exports.cpp`: the seven exports (`GetGlobalInfoW`, `SetStartupInfoW`, `GetPluginInfoW`, `OpenW`,
`ProcessConsoleInputW`, `ProcessSynchroEventW`, `ExitFARW`), each a few lines of marshalling into
the composition root behind a catch-all firewall (nothing propagates into Far). `OpenW` stays
(Renewal recognises a plugin DLL by that export; `OPEN_FROMMACRO` returns nothing).
`Composition.cpp`: constructs the adapters and the `Session` factory once, owns them for the
process lifetime. `version.h` stays the single source of the version.

## 2. Threads

Two threads, as today. Far's main thread runs the exports, the gesture, every Far API call and
(feature 1) the extraction. The tool thread owns the tool window, `OleInitialize`, the drag loop
and the COM objects. Rules: a Far API call from the tool thread is a bug (the guard test cannot
see it, the reviewer must); the tool thread asks for main-thread work with `postSynchro` and,
when it needs a result, waits on an event with a timeout; the main thread asks the tool thread for
work with `SendMessage` / `PostMessage` to the tool window. `Session` state that both threads
read is guarded by a mutex or handed over by value in the messages; document which. For a
same-Far drag, the prepare message copies an immutable panel/geometry snapshot to the tool thread
for cosmetic hover feedback. On `Drop`, that thread puts a by-value
`PendingDrop { point, effect }` behind the session mutex, posts synchro, and returns the effect to
OLE; it neither calls Far nor reads later main-thread state. Far's synchro handler re-reads both
panels, the host and cell geometry, and replays only if the active source still has the original
handle and rectangle and both sides are file panels. Otherwise it cancels with a one-line Far
message. OLE owns ordinary keyboard input during the drag, but a macro, timer, panel swap or
console resize can still make the hover snapshot stale. Replay also runs on Far's thread; an
incomplete write is reported, and a one-record partial write is followed by a buttonless release
at the press cell so Far's panel-drag state is not left armed.

## 3. Testing

doctest, one executable per layer group plus e2e:

- `tests/core/`: everything in core with fakes for the interfaces; the gesture's every path (the
  1.2.0 review findings are the checklist: hold under the threshold, right click replay, right
  double click, passive panel, plugin panel without real names, threshold on the edge rows,
  the `Spent` reset, wheel passes).
- `tests/adapters/`: the real adapters on real resources inside the test process: a console
  (`AllocConsole` if none), real windows, real temp files, a real shell data object from real
  temp files, `IFileOperation` between temp directories, `WriteConsoleInputW` then
  `ReadConsoleInputW` on the test's own console, the `GetFilesW` call against a stub plugin
  module built for the test (a tiny DLL exporting `GetFilesW` that writes files, and one that
  crashes, to exercise the SEH path).
- `tests/drag/`: the tool window and thread for real (create, show over a test rect, arm timer,
  the messages), `DropTarget` driven by calling its methods with a data object built from temp
  files, `DragSource` with a fake policy.
- `tests/e2e/`: `LoadLibrary` of the built `Burlak.dll`, the seven exports called through a
  `PluginStartupInfo` whose function pointers are test stubs; plus one real drag: park the cursor
  over a test window that registers an `IDropTarget` (`SetCursorPos`), call `SHDoDragDrop` with
  no button held — OLE's first `QueryContinueDrag` sees no button and drops at once — and assert
  the target received the paths. The same trick covers `IInput::press/release`: with the cursor
  parked over the test's own window, an injected click lands on it and nothing else. Restore
  the cursor afterwards.
- `tests/guard/`: the source scan for the dependency rule and the ownership rules.

Coverage: `scripts/coverage.ps1` builds the `coverage` preset (clang-cl, `-fprofile-instr-generate
-fcoverage-mapping`, atomic counters), runs ctest, merges every `.profraw` (the e2e process writes
two: its own and the DLL's), and fails unless every file under `src/**` appears in the report at
100 % lines and 100 % branches. No exclusion markers, no uninstrumented files; if a branch cannot
be exercised, restructure so it does not exist.

## 4. Build and delivery

CMake ≥ 3.28 with presets. Release presets `release-x64`, `release-x86`, `release-arm64` use the
`Visual Studio 17 2022` generator (MSVC cl.exe, the compiler of every shipped Burlak so far; the
generator finds the toolchain, no vcvars replay) with today's flags: `/W4 /WX /O2 /MT /GS
/permissive- /guard:cf /DUNICODE /D_UNICODE`. `debug` and `coverage` presets use Ninja + clang-cl
x64 (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin`, no
vcvars needed), `-std=c++23`, the same warnings as errors. The code must compile clean under
both compilers. `build.ps1` becomes a thin wrapper over the presets (or goes away, with README and
CI updated). `scripts/package.ps1` keeps its contract: `Burlak-<version>-<arch>.zip` with the
`Burlak\` folder (DLL, ChangeLog, readme_en.txt, readme_ru.txt, LICENSE).

CI (`.github/workflows/ci.yml`): keep the job names `build (x64)`, `build (x86)`, `build (arm64)`
— the master ruleset requires exactly those checks — and their export check; add `test` (the
coverage preset, ctest, the 100 % gate) and `lint` (`scripts/lint.ps1`: clang-format, clang-tidy,
cppcheck, PSScriptAnalyzer, BinSkim over the three release DLLs); `release` needs all of them.
