# Burlak — architecture

Status: v1 design, 2026-09-15. Owner: orchestrator (Claude). Implementers: Codex agents.
Reviewers: Codex review runs. The rules that constrain this design live in `AGENTS.md`; if the two
disagree, stop and report. Interface shapes below are canonical in intent, not in every
signature: keep the names and the responsibilities, adjust parameter lists when the code shows a
better cut, and say so in the report.

## 0. What Burlak is, and where it is going

A Far Manager 3 plugin, one DLL per architecture (x64, x86, arm64), that turns a mouse gesture on
a panel item into an OLE drag-and-drop of the selected files into any Windows drop target.
Version 1.3.0 does this from panels with real file names and from plugin panels, with the left or
the right button, and also makes Far a drop target over panels backed by real directories.

The three 1.3.0 capabilities, against which the layering below is judged, are:

1. **Drag from plugin panels** (Arclite, Observer, NetBox, ...). The panel has no paths on disk.
   At the threshold the plugin creates zero-byte placeholder files with the item names in a temp
   folder and starts the drag with them; when the button is released over a willing target
   (`IDropSource::QueryContinueDrag` is where OLE asks us whether to drop), it extracts the real
   files over the placeholders by calling the owning plugin's `GetFilesW` on Far's main thread
   (`PanelInfo::PluginHandle` is exactly the `hPanel` Far passes; `OwnerGuid` names the plugin;
   `PCTL_FINDPLUGIN` + `PCTL_GETPLUGININFORMATION` give its module and `GlobalInfo::Instance`),
   exactly as Far itself does for plugin-to-plugin copy (`FileList::PluginGetFiles`, `OPM_SILENT`
   into a temp directory), then answers "drop". The call is wrapped in SEH so a crashing plugin
   aborts the drag with a message instead of taking Burlak down. A completed ordinary OLE drop
   refreshes the run timestamp and retains extracted files for three minutes so asynchronous
   targets can read them. A one-minute tool-window timer plus startup and shutdown sweeps remove
   old own runs and old runs whose owner process is dead. A sweep first opens every regular file
   with read access and no sharing; one sharing violation marks the whole run in use and preserves
   it until a later sweep. The original `FCTL_GETSELECTEDPANELITEM` buffers stay alive in the
   plan and are passed back as complete `PluginPanelItem` records, matching
   `FileList::CreatePluginItemList`. Release revalidates the panel, complete selection and owner
   module on Far's thread. A case-insensitive duplicate name cannot be represented faithfully in
   one temp directory and prevents the drag from starting. A rewritten `GetFilesInfo::DestPath`
   cancels the drop because OLE already advertises the original directory. Placeholder-backed
   drags offer copy and move, but not link: cleanup would otherwise leave a broken shortcut.
2. **Drop into the other panel of the same Far.** The tool window that covers the terminal during
   a drag registers an `IDropTarget`. Over the panel that is not the source it offers
   copy (move with Shift); elsewhere nothing. On drop it replays Far's own panel-to-panel mouse
   drag through `WriteConsoleInputW`: a left press at the cell the gesture started on, a release
   at the drop cell. Far's native `KEY_DRAGCOPY`/`KEY_DRAGMOVE` path does the copy, including
   plugin panels on either side. Burlak copies nothing itself here.
3. **Far as a drop target.** `RegisterDragDrop` cannot register the terminal window because another
   process owns it. Instead, the same alpha-1 tool popup used for source drags becomes a temporary
   OLE target over the eligible panel pixels while an external drag is over this Far. A 50 ms
   tool-thread poll records the point and root window on the physical left/right-button down edge,
   then asks pure `ExternalDragPolicy` whether the held drag moved from outside our host and tool
   window onto our host. The point's root must be the host or console, so an unrelated window
   overlapping the host wins. A drag-selection from another application can satisfy the physical
   test, but its capture keeps OLE from entering the popup; button-up hides it harmlessly.

   Each set-focus `FOCUS_EVENT` marks the host window with a registered Burlak window property
   containing this process id. Of several Fars sharing a Windows Terminal host, only the last
   focused process accepts the candidate. An absent property is accepted for conhost and for a
   property write refused by UIPI; a property naming a process that is no longer alive is likewise
   treated as absent. The next focus-in overwrites that stale value. `ExitFARW` removes the property
   only when it still contains this process id.

   A candidate posts Far synchro. Far's thread returns a pointer-free `ReceiveSnapshot`: whether the
   panels window is current, both panels' visibility/type/real-name flags, rectangles and
   directories, the host, and cell geometry at the sampled point. The tool thread covers the union
   of visible real-name file-panel rectangles, directly above the host in z-order, without mouse
   capture. Dialogs, editor/viewer, command line, key bar, menus, and plugin panels remain uncovered,
   so the terminal keeps its existing path-paste behaviour there. Far's own panel hit testing also
   admits only item rows (`far/filelist.cpp`, `FileList::ProcessMouse`).

   `IDropTarget::DragEnter` accepts only `CF_HDROP`; receive and source are distinct states of one
   tool-window state machine. `ReceivePolicy` maps an item pixel to an eligible panel directory and
   intersects copy or Shift-move with the effect mask supplied by the source at `DragEnter` (a
   copy-only source therefore remains copy-only with Shift). The right-drop menu disables choices
   outside that mask; `TrackPopupMenu` accepts only an owner window of the calling thread, so the
   menu is owned by the tool window itself, never by the host that conhost or Windows Terminal
   owns (a foreign owner fails at once with `ERROR_INVALID_PARAMETER`, which would cancel every
   right-button drop). At drop time the foreground belongs to the drag source, so a bare
   `SetForegroundWindow` on the overlay is refused and the popup would ignore an outside click or
   Escape while the source sits blocked in `DoDragDrop`; the adapter therefore attaches the tool
   thread's input to the foreground thread's around `SetForegroundWindow` + `TrackPopupMenu`,
   detaches afterwards, and posts `WM_NULL` to the owner (KB135788), still showing the menu if the
   foreground call is refused. `Drop` reads at most 4096 absolute paths, refuses any path longer than 32767
   code units, and bounds the aggregate path buffers to 1 MiB before allocation. Immediately before
   `IFileOperation`, the tool thread posts a second bounded synchro request; Far's thread rebuilds
   the snapshot and the drop is cancelled with one message unless `ReceivePolicy::sameIdentity`
   still matches for the drop point: the destination panel's identity (visible, file panel, real
   names, rectangle, handle, owner) and directory, the host, the cell geometry and the panels
   window. The destination's cursor and selection and the whole other panel may change freely, so
   a background refresh of the panel not receiving the drop does not cancel it. The tool thread
   then calls `IFileOperation` synchronously with the host as owner,
   so shell progress, conflicts and elevation prompts belong to the receiving Far while Far's
   thread remains free. It must finish before `Drop` returns because the source acts on the returned
   effect immediately. Following [Handling Shell Data Transfer
   Scenarios, Handling Optimized Move Operations](https://learn.microsoft.com/windows/win32/shell/datascenarios#handling-optimized-move-operations),
   a completed copy returns `DROPEFFECT_COPY`; a completed move returns `DROPEFFECT_NONE` and writes
   `CFSTR_PERFORMEDDROPEFFECT = DROPEFFECT_NONE`, so the source does not delete files already moved
   by the receiver. Failure or cancellation returns none without setting the performed effect.
   Successful work posts a by-value panel side for Far-thread update/redraw. A source under
   `%TEMP%\Burlak` is ordinary input: a same-volume move naturally becomes a rename, and retention
   plus the timer sweep handles anything left behind.

   Receive mode is explicit: `ReceiveArmed` means the overlay is visible but OLE has not entered,
   `ReceiveEntered` begins at `DragEnter`, and `ReceiveDropping` covers the whole synchronous `Drop`.
   Button-up hides only an armed overlay. `DragLeave` returns Entered to Armed (including source-side
   Escape cancellation), while button-up in Entered waits for the source's possibly slow extraction
   and the later `Drop`. As a safety net for a source that crashes after releasing the button and
   never sends `Drop` or `DragLeave`, two minutes in that condition return the window to idle
   (`receiveEnteredTimeout`): a plugin extracting a large archive inside `QueryContinueDrag` can
   legitimately take longer than ten seconds, and the price of the wait is that the alpha-1 layered
   overlay, which is hit-tested, swallows every mouse click on Far's panels for up to two minutes
   after a source dies (the keyboard still reaches Far). While the first drag lingers in Entered a
   second drag's `DragEnter` is refused and the target forgets the first drag's entry, file data,
   right button and effect mask, so that second drag's `DragOver` and `Drop` are answered none and
   cannot take the Dropping transition. Only one OLE drag exists per desktop, so any `DragLeave`
   in receive mode belongs to the only live drag and always reaches the lifecycle: a lingering
   receive therefore ends on the next `DragLeave` from either drag (the poll then hides the armed
   overlay on the released button) rather than waiting for the ceiling. The right-drop menu pumps
   COM, so the mask a drop was entered with is copied before `TrackPopupMenu` and applied to the
   choice afterwards. Poll and sweep timers do nothing in Dropping, preventing
   re-entry while a popup menu or `IFileOperation` pumps messages. `Drop` always returns to idle.
   OLE does not re-hit-test after `DRAGDROP_S_DROP`, so a `DragOver` or `Drop` can still reach the
   overlay after the window returned to idle (timeout, exception, hidden overlay): the same-Far
   replay path is taken only while the window is in `SourceDragging` (`IReceiveLifecycle::sourceMode`),
   every other state answers none, and `Session` drops its hover context the moment a source drag
   ends so no stale selection can be replayed; a new gesture also discards a replay still queued by
   the previous drag. Source arming and `Session::begin` refuse every receive state, and receive
   detection is disabled throughout a source drag. A source release over any foreign window follows
   ordinary OLE `Drop` (or `ExtractThenDrop` for a plugin-panel plan). A Far-to-Far drag consequently
   uses the same standard OLE path as Explorer and requires Burlak in both Fars.

None of these needs hooks or injected DLLs. Dropping into plugin panels is not yet supported, and
Windows UIPI blocks a non-elevated source from dragging into an elevated Far just as it blocks other
elevated drop targets.

## 1. Layers and the dependency rule

```
burlak/
  src/core/       decisions. Pure C++: no <windows.h>, no Far headers, no COM. Own value types.
  src/adapters/   one-to-one wrappers over foreign APIs, no decisions:
    far/          PluginStartupInfo (panels, windows, synchro, plugins control, messages),
                  the call into another plugin's GetFilesW (with its SEH guard)
    win/          user32 / kernel32: cursor, buttons, windows, console geometry and input,
                  input injection, temp files, receiver property and the right-drop menu
    shell/        shell32 / ole32: data objects, CF_HDROP, the drag loop, IFileOperation and
                  performed-drop-effect reporting
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
  SEH-guarded, returns the plugin's effective destination).
- `IScreen` — `cursor()`, `buttonDown(Button)`, root `windowAt(Point)`, console and host handles,
  `hostWindow[At]()` (rect + handle, the terminal window under the cursor or owning the console),
  and `cellGeometry[At]()` (pixel size of a cell and the origin of cell (0,0), for point↔cell
  mapping).
- `IInput` — `release(Button)`, `press(Button)` (mouse_event forwarders), `replay(std::span<const
  MouseEvent>)` (WriteConsoleInputW).
- `IShell` — `makeDataObject(paths)`, `runDrag(...)` (`SHDoDragDrop` with our `IDropSource`), and
  `copy(paths, destination, Effect, owner)` (`IFileOperation`).
- `IDropData` — the injected native-data-object bridge: test `CF_HDROP`, read its bounded path list,
  and publish `CFSTR_PERFORMEDDROPEFFECT`. Its implementation belongs to `adapters/shell`; the drag
  layer has no adapter include or link dependency.
- `IFiles` — temp directory for this run, `placeholder(name, directory)`, `removeTree`, `touch`,
  process-liveness probing, and a grace-period sweep of old own runs and old dead-owner runs at
  startup, once a minute while the tool window is idle, and at exit. The adapter gathers the
  file-in-use fact; core decides.
- `IWindowProperties` — set/read/remove the process id on a host window and report our process id.
- `IDropMenu` — map the native Copy here / Move here / Cancel popup to a core menu choice; the
  drag layer hands it the tool window as owner (the only window of the tool thread), and the
  adapter attaches that thread's input to the foreground thread's for the call so the popup can
  be dismissed by an outside click or Escape.

Core logic:

- `Gesture` (exists today): the state machine over `MouseEvent`s, returning `Verdict {Pass, Hold,
  Replace(MouseEvent)}` and, through a callback or an out-value, the request to start a drag with
  a button. Same behaviour as 1.2.0; the tests pin it.
- `Geometry`: cell ↔ pixel mapping, "which panel is this cell on", the item-row test that today
  lives in `InsidePanel`.
- `DragPlan`: given the source panel, decides the source kind (real paths vs placeholders + the
  extraction recipe) and builds the path list; today's `SelectedPaths` plus the TmpPanel case
  (an absolute `FileName` is taken as is).
- `ReleasePolicy`: given the button state, escape state, last feedback effect, extraction need and
  whether the point is over our own tool window, decides `Continue`, `Drop`, `Cancel`, or
  `ExtractThenDrop` — the single place `DragSource::QueryContinueDrag` consults.
- `DropPolicy`: for the own `IDropTarget`: effect for a point (feature 2) and the replay records.
- `ExternalDragPolicy`: the pure receiver-candidate decision over sampled button/window/property
  facts.
- `ReceiveSnapshot` and `ReceivePolicy`: eligible identity, overlay union, point effect and
  destination. The optimized-move outcome and menu-choice mapping are pure functions.
- `shouldSweepRun`: decides from ownership, owner liveness, age and in-use facts; the one grace
  constant is three minutes and the tool-window cadence is one minute.
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

`ToolWindow` (the class, thread, message loop, layered 1-alpha popup, external-drag and sweep
timers), `DragSource` (`IDropSource`: `QueryContinueDrag` asks `ReleasePolicy`; `GiveFeedback`
records the last effect), and `DropTarget` (`IDropTarget`: dispatches between same-Far replay and
external receive mode). One state machine owns `Idle`, `SourcePrepared`, `SourceArmed`,
`SourceDragging`, `ReceivePending`, `ReceiveArmed`, `ReceiveEntered`, `ReceiveDropping`, and
`ReceiveRejected`; only `Idle` admits a new source session or timer sweep. Messages between Far's
thread and the tool thread stay explicit
(`WM_PREPARE_DRAG`, `WM_START_DRAG`, `WM_ABORT_DRAG`, `WM_RECEIVE_SNAPSHOT`, and timers) and are
the only cross-thread traffic. Every private `WM_USER`
message carries zero parameters: the Far-thread caller first stores its by-value request in a
mutex-protected slot owned by the tool-window state, and the tool thread validates both parameters
before consuming that slot. A nonzero parameter or an unsolicited message with no queued request
is inert, so another same-integrity process cannot consume a pending request or make the responder
interpret an un-marshalled pointer. Native data-object work is injected through `IDropData`, keeping
the drag library dependent on core only. Far's API is called on Far's thread only, which the tool thread
reaches through `IFarHost::postSynchro`. The receive copy is the deliberate exception to waiting
for Far: it needs no Far API and stays synchronous on the tool thread inside `IDropTarget::Drop`.

### plugin

`Exports.cpp`: the seven exports (`GetGlobalInfoW`, `SetStartupInfoW`, `GetPluginInfoW`, `OpenW`,
`ProcessConsoleInputW`, `ProcessSynchroEventW`, `ExitFARW`), each a few lines of marshalling into
the composition root behind a catch-all firewall (nothing propagates into Far). `OpenW` stays
(Renewal recognises a plugin DLL by that export; `OPEN_FROMMACRO` returns nothing).
`Composition.cpp`: constructs the adapters and the `Session` factory once, owns them for the
process lifetime. `version.h` stays the single source of the version.

## 2. Threads

There are two threads. Far's main thread runs the exports, gesture, every Far API call, plugin-panel
extraction, receive snapshot construction and panel redraw. The tool thread owns the popup,
`OleInitialize`, both timers, the OLE drag loop and COM objects. A Far API call from the tool thread
is a bug (the guard test cannot see it, so review must).

For plugin-panel extraction the tool thread posts synchro and waits while pumping COM; the wait lasts
as long as the owning plugin's `GetFilesW`. A Far-thread scope guard publishes failure if an allowed
exception reaches the export firewall, otherwise the main thread stores the result and signals. The
same mutex protects the extraction request, retained native item storage, same-Far pending drop,
receive snapshot and refresh point/value, receive identity, and pending redraw side.

For a same-Far drag, the prepare message copies an immutable panel/geometry snapshot to the tool
thread for hover feedback. On `Drop`, that thread stores a by-value
`PendingDrop { point, cell, effect }`, posts synchro and returns to OLE. Far's handler re-reads both
panels, directories, source selection, current window, host and cell geometry, then replays only if
the full identity still matches. Shell data-object preparation is all-or-nothing. A stale identity
or incomplete console-input write is reported; a one-record partial write is followed by a
buttonless release so Far's panel-drag state is not left armed.

For an external drag, the 50 ms poll and all `IDropTarget` methods run on the tool thread. Once core
accepts a candidate, the tool stores only the sampled `Point` and posts synchro. Far's thread builds
the complete by-value `ReceiveSnapshot`; the composition moves it into the guarded tool-window slot,
and a zero-parameter private message consumes it. Hover reads only that immutable snapshot. Inside
`ReceiveDropping`, `Drop` asks Far's thread for a fresh snapshot and waits at most ten seconds while
pumping COM/window calls; an identity mismatch or timeout cancels before any filesystem operation.
It then reads bounded `CF_HDROP` data through `IDropData` and runs `IFileOperation` synchronously on
the tool thread: the source consumes the returned and performed effects as soon as `Drop` returns,
while the operation needs no Far API. The COM entry points contain allocation and invariant
exceptions. On success the tool stores only the by-value `PanelSide` and posts synchro for
Far-thread update/redraw.

The one-minute sweep timer also runs on the tool thread, but only in `Idle`; startup and exit call the
same adapter from Far's thread. `Session::begin` never sweeps. The three-minute age makes an active
drag ineligible, retention touches its run before returning to `Idle`, and the adapter's sharing probe
protects a file already being read.

Before teardown posts `WM_QUIT` and joins the tool thread, the main thread signals any extraction
wait with failure. The join has no timeout and pumps COM/window calls because a target apartment can
still be involved after `Drop` returns. The worker creates its message queue, initializes OLE,
creates/registers the window and starts both timers before signalling readiness. If the bounded
startup wait expires, the main thread sets a stop flag before the unbounded join; the worker observes
that flag even if an earlier `WM_QUIT` could not be posted.

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
- `tests/drag/`: the tool window and thread for real (create, show over a test rect, source arm,
  external-drag and sweep timers, guarded messages), `DropTarget` driven in source and receive
  modes with a data object built from temp files, and `DragSource` with a fake policy.
- `tests/e2e/`: `LoadLibrary` of the built `Burlak.dll`, the seven exports called through a
  `PluginStartupInfo` whose function pointers are test stubs; real OLE drags cover a Burlak source
  into a test target and a shell data object into Burlak's temporary receive overlay, asserting the
  destination file and returned effect. The same cursor-owned technique covers input injection.
  Restore the cursor afterwards. `BURLAK_NO_DESKTOP=1` skips these desktop-owning integration
  checks while injected boundaries keep production coverage complete; a runner on which OLE never
  enters after the initial desktop checks produces the established visible warning.
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
