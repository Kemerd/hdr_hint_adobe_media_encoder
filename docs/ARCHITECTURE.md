# HDR Hint — Architecture

Condensed from the approved design. The headers under `src/core` and `src/platform` are the contract; this document explains how the pieces fit, what runs on which thread, how the panel talks to the app, and what still has to be confirmed against a live AME.

## 1. Overview

```
 Adobe Media Encoder.exe (system-DPI-aware)                 HdrHint.exe (PMv2, C++20, Direct2D)
 ┌──────────────────────────────────────────┐              ┌──────────────────────────────────────────────┐
 │ ExtendScript engine  ── host.jsx ──┐     │  named pipe  │ IpcServer thread ──► EventQueue ──► Engine   │
 │ (encode events → CSXSEvent)        ▼     │◄────────────►│ (UI thread owns JobStore/state machine)      │
 │ CEP panel "HDR Hint" (panel.js, Node)    │  \\.\pipe\   │ AmeLogTailer thread  (UTF-16 log, both logs) │
 │   • forwards events, bounds, pid, skin   │   HdrHint    │ FolderWatcher thread (sidecars, outputs)     │
 │   • launches exe if pipe absent          │              │ FileReadiness thread (deny-write probe, moov)│
 │   DroverLord panel host HWND ◄───────────┼── owned ─────│ MuxWorker thread (mkvmerge -J / mux / verify)│
 │   (CEF child HWND inside it)             │  window,     │ WindowHost (D2D/DComp, docked or floating)   │
 └──────────────────────────────────────────┘  tracked     └──────────────────────────────────────────────┘
                     AMEEncodingLog.txt ──────────────────────────────►(tailed; also the no-panel trigger)
```

`HdrHint.exe` is a native C++20 / Direct2D application. It is never a child window of AME: while docked it is a top-level window *owned* by AME's frame and positioned over the CEP panel's HWND. The CEP panel is only the dock target, the event bridge and the launcher. Everything else (log tailing, folder watching, muxing) works with no panel at all.

Names: exe `HdrHint.exe`, CEP bundle `com.everett.hdrhint`, pipe `\\.\pipe\HdrHint`, registry `HKCU\Software\HdrHint`, settings `%APPDATA%\HdrHint\settings.ini`, state and logs `%LOCALAPPDATA%\HdrHint\`.

## 2. The three trigger sources

| Source | Carries | Latency | Blind spot |
|---|---|---|---|
| **CEP bridge** (`host.jsx` → `panel.js` → pipe) | `encodingStarted` / `encodeComplete` with absolute paths, 1 Hz progress, batch item status (Failed / Stopped / Done) | milliseconds | only while the panel is open; the `result` vocabulary is ambiguous ("True"/"False" in the docs, "Done!" in the binary) |
| **AME encoding log** (`AmeLogTailer`) | authoritative colour space from the `Video:` line, success / failure, preset name, output path | about 1 s after completion (the block is appended only at the end) | Premiere direct exports never log; failures appear in both `AMEEncodingLog.txt` and `AMEEncodingErrorLog.txt` (deduped by path + timestamp) |
| **Render-folder watcher** (`FolderWatcher`) | sidecars `<stem>.<pid>.<tid>.m4v` / `.aac` (earliest "Encoding" row), the output `.mp4` appearing | immediate | cannot tell PQ from HLG; the folder must be known (learned from the log history, or configured) |

All three only *signal*. `FileReadiness` decides when the file is safe to read: exists → deny-write open (`GENERIC_READ`, share `READ|DELETE`) → size stable 2 s → no sidecars left → `Mp4Boxes` chain check (`moov` + `mdat` present, top-level boxes end at EOF, last chunk inside the file) → log / CEP confirmation or a 20 s timeout. AME creates the `.mp4` with an exclusive handle, writes the 24-byte `ftyp`, assembles `moov` + `mdat`, deletes the sidecars, releases the lock, then appends the log block, so the probe reports "Waiting for AME to release file" until the lock drops.

Dedupe key: `GetFullPathNameW` + strip `\\?\` + `CharUpperW`. A terminal job re-signalled by a *new* export (fresh sidecars, `FILE_ACTION_ADDED`, a newer log timestamp, or a different file id in `SourceStamp`) becomes `generation + 1` of the same key.

## 3. Job state machine (`JobModel.h`)

States: `Discovered, Encoding, Ready, Held, Muxing, Verifying, Done, Failed, SkippedSdr, Cancelled` (the last four are terminal). Transitions happen on the UI thread only; workers never touch `JobStore`.

```
SidecarSeen / cep encodingStarted ───────────────► Encoding
OutputSeen (locked) ─────────────────────────────► Encoding      OutputSeen (unlocked) ► Discovered ► probe
Encoding ── probe Ready (+log/cep confirm or 20 s timeout) ──► Ready ── LogCompleted(Failed) ──► Failed
Ready ── transfer==SDR ──► SkippedSdr     Ready ── hold | !auto | plan error | transfer Unknown ──► Held
Ready/Held ── dispatch ──► Muxing ── exit 0/1 ──► Verifying ── ok ──► Done ── (recycle original)
Muxing/Verifying ── error/cancel ──► Failed/Cancelled (partial deleted)     user Retry/Run ──► Ready (re-probed)
```

Transfer inference, in priority order: CEP / log `Video:` line → `MediaProbe` (ffprobe, optional; also reports in-band HDR10 SEI) → `mkvmerge -J` colour properties → user choice (`Held` while unknown). Policy by transfer: PQ → preset `generic_pq_1000` + the bundled PQ LUT; HLG → `generic_hlg` + no LUT; SDR → `SkippedSdr` (or tag / hold per settings).

Persisted to `%LOCALAPPDATA%\HdrHint\jobs.json` (atomic write, debounced). On reload `Muxing` / `Verifying` become `Failed("Interrupted")`; `Held` survives.

## 4. Threads

| Thread | Owns | Posts (`EngineEvents.h`) |
|---|---|---|
| **UI** | `Engine`, `JobStore`, settings, every widget, the HWND | drains `EngineEventQueue` on the `WM_APP` kick (`Engine::onEventMessage`); `Engine::tick()` at 1 Hz |
| `AmeLogTailer` | log discovery, UTF-16LE tail with odd-byte carry, one `AmeLogParser` per file, offsets in `state.json` | `LogItemEvent`, `LogQueueEvent`, `LogFoldersEvent`, `LogStatusEvent` |
| `FolderWatcher` | one overlapped `ReadDirectoryChangesW` per folder, 500 ms coalescing | `SidecarEvent`, `OutputEvent`, `FolderAvailabilityEvent` |
| `FileReadiness` | scheduled probes (1.5 s → 5 s back-off, 12 h cap) | `ProbeEvent` |
| `MuxWorker` | one mkvmerge job at a time: `-J` identify → mux → `-J` verify → rename | `MuxEvent` (Started / Progress / Finished) |
| `IpcServer` | the named pipe, 4 overlapped instances, line splitting, liveness | `IpcMessageEvent`, `IpcClientEvent` |
| any worker | | `WorkerNoteEvent` |

Rules: workers post through `IEngineSink::post` and never touch UI state; commands to workers carry copies; errors are `Result<T>` values and never cross a thread as exceptions. Shutdown: stop event → cancel I/O → join with timeout → save state → `DestroyWindow`; a running mux either finishes or is cancelled (partial deleted). Because a cross-process owner/owned relationship attaches input queues, the UI thread must never block: a stall in HdrHint would freeze AME's input while docked.

mkvmerge invocation (track options before the input, attachment options before `--attach-file`; the video track id comes from `-J`, never assumed):

```
mkvmerge --ui-language en --gui-mode --priority lower --output "<hint>.hdrhint-partial.mkv"
  [--attachment-mime-type application/x-cube --attachment-name <lut> --attach-file "<lut>"]
  --color-matrix-coefficients T:9 --color-range T:1 --color-transfer-characteristics T:16|18 --color-primaries T:9
  [--max-content-light T:1000 --max-frame-light T:400 --chromaticity-coordinates T:0.708,0.292,0.170,0.797,0.131,0.046
   --white-color-coordinates T:0.3127,0.329 --max-luminance T:1000 --min-luminance T:0.0001]
  "<AME output .mp4>"
```

## 5. IPC protocol (`IpcProtocol.h`, `IpcServer.h`, `Engine.h`)

Transport: `\\.\pipe\HdrHint`, byte mode, newline-delimited UTF-8 JSON objects, every object has `kind`, 1 MiB line cap, DACL = current user + SYSTEM, `PIPE_REJECT_REMOTE_CLIENTS`, liveness 15 s (the panel pings every 5 s). `ipc::parseLine` returns `std::nullopt` for anything that is not an object with a `kind`; field access goes through `ipc::str / wstr / num / boolean`, so a malformed message can never throw.

Panel → app:

| kind | fields | handled by |
|---|---|---|
| `hello` | `pid` (the renderer's `process.pid`), `extPath`, `version`, `skin{panelBg,isDark}`, `host{appName,appVersion,appLocale}` | `Engine::onPanelMessage` → DockController (browser pid = parent of `pid`) |
| `ame` | `type` ∈ `encodingStarted, encodeComplete, progress, batchItemStatus, queueStatus, bridgeReady, appBeforeQuit, extensionUnloaded, workspaceChanged, visibility`; plus `result, sourceFilePath, outputFilePath, progress, groupIndex, itemIndex, status, seq, t, registered[], errors[], ameVersion` as applicable | `Engine::applyAmeMessage`; `(groupIndex,itemIndex)` is paired to a path via the preceding `encodingStarted` (`cepStartedPaths_`); lifecycle types are forwarded to `onPanelMessage` |
| `panelBounds` | `x, y, w, h` (CSS px), `dpr`, `visible` (`__adobe_cep__.isWindowVisible()`), `seq` | `onPanelMessage` → DockController (fallback geometry only) |
| `command` | `cmd` ∈ `show, dock, undock` | `onPanelMessage` |
| `ping` | — | `Engine` answers `pong` |

App → panel (builders in `IpcProtocol.h`):

| kind | fields |
|---|---|
| `welcome` | `appVersion`, `protocol` (1), `docked` |
| `status` | `docked`, counts `encoding / ready / muxing / done / failed`, `lastJob{name,state}`, `mkvmerge{version,ok}` (`makeStatus`) |
| `jobs` | the job list as `jobToJson` objects (`makeJobsEvent`) |
| `ack` | echo of the request plus `ok` and `error` (`makeAck`) |
| `requestBounds` | — |
| `pong` | — |

Connection events arrive as `IpcClientEvent` → `Engine::onPanelConnection`; `panelConnected()` counts live panels. Single instance: a named mutex; a second launch forwards its argv by `WM_COPYDATA` to a permanent message-only window (it survives HWND recreation).

## 6. Docking mechanics (`src/ame`)

**What AME 2026 actually does with the panel (verified 2026-09-16).** AME registers the CEP manifest and shows *Window > Extensions > HDR Hint*, but never starts `CEPHtmlEngine.exe` for it: the menu item only creates an empty floating frame (top-level `DroverLord - Window Class`, real Windows caption, owned by the main frame, one bare `OS_ViewContainer`, no `DroverLord - TabPanel Window` inside). The CEP pipe/`hello` path below therefore never fires on 2026 and is kept for versions that do host CEP. The default flow is **overlay docking without CEP**:

- `DockController::tick()` (1 Hz) calls `dockToDefaultTarget()` while floating: the extension frame's content container first (found by the shape above, `findExtensionFrame`), then the panel saved from a previous pick (`[app] dock_target = l,t,w,h` relative to AME's client area, matched by IoU ≥ 0.5 among visible Drover tab panels / frames). The extension frame is saved as `dock_target = extension` because it floats wherever the user left it.
- **Dock inside AME** with nothing to attach to enters *pick mode*: the next left click on any Media Encoder window chooses the panel to cover (Esc cancels, 30 s timeout). `--dock`, `--undock` and `--dock-at x,y` drive the same paths for scripts.
- Docking onto the frame's content container (not the frame) leaves AME's tab strip visible, so the user can still drag the tab into the workspace or close it.
- Foreign windows over the dock rect never hide the window (we live in AME's z-band as an owned window); only other AME windows (dialogs, other frames) do. AME's own move loop no longer hides the window either: `LOCATIONCHANGE` keeps it following during the drag.
- **Media Encoder launches the app.** `scripts/install_startup.ps1` copies `cep/startup/HdrHintLauncher.jsx` into AME's `Scripts\Startup` folder (plus `HdrHintExePath.txt` as a fallback path); AME evaluates every `.jsx` there at launch. The script reads `exePath` from `%APPDATA%\HdrHint\launcher.json` and calls `File.execute()`. ExtendScript cannot pass arguments, so `main.cpp` checks its parent process: an `Adobe Media Encoder` parent implies `--from-panel --tray` (quiet tray start, dock on sight, quit with AME via `[app] quit_with_ame`). Nothing runs while AME is closed.
- `[app] start_with_windows` (default **off**) mirrors into `HKCU\...\CurrentVersion\Run\HdrHint = "<exe>" --tray`. Only needed when the startup script is not installed.

The CEP path, for hosts that do run the panel:

1. **Find AME**: visible top-level windows of `Adobe Media Encoder.exe` with `WS_CAPTION | WS_THICKFRAME`, largest area. The class name is version-suffixed (`Adobe Media Encoder 2026`), so match by prefix only. AME is System-DPI-aware; HdrHint is Per-Monitor-V2 and works in physical pixels end to end.
2. **Find the panel**: `hello.pid` is the renderer; its parent `CEPHtmlEngine.exe` without `--type=` is the browser whose HWNDs live inside AME. Enumerate AME's top-level windows and their children for HWNDs owned by that pid; the panel host is the nearest `DroverLord - Window Class` ancestor; the owner root is `GetAncestor(GA_ROOT)` (main frame, or the floating frame of a torn-off panel).
3. **Dock rect** = physical `GetWindowRect` of the CEF HWND ∩ the owner root's client rect. Fallback while no HWND is known: JS bounds × `dpr` through `LogicalToPhysicalPointForPerMonitorDPI(ameMain)`.
4. **Ownership**: `SetLastError(0); SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, ownerRoot)`. The window stays top-level, sits above AME, hides on minimise and never appears on the taskbar. Docked chrome: `WS_POPUP`, `WS_EX_TOOLWINDOW`, no rounding, no backdrop, background = AME's `panelBackgroundColor` from `hello`.
5. **Tracking**: `SetWinEventHook` scoped to the panel host's thread and the root's thread (`LOCATIONCHANGE`, `SHOW/HIDE`, `REPARENT`, `DESTROY`, `MINIMIZESTART/END`, `MOVESIZESTART/END`), `OBJID_WINDOW` filter, callbacks only post a coalesced tick. Hidden while the CEF HWND is invisible (background tab), while `WindowFromPoint` at the rect's centre / corners belongs to neither us nor the owner (AME dialogs), and during AME's own move loop.
6. **Hazards handled**: an owner destroyed by AME (quit, workspace rebuild) destroys owned windows first, so an unsolicited `WM_DESTROY` never posts `WM_QUIT`; the HWND is recreated un-owned in floating mode, the DComp surface rebound and the tray icon re-added. Un-own proactively on `appBeforeQuit`, `extensionUnloaded` and pipe close. Elevation mismatch (UIPI) is detected and reported instead of silently failing. The exe spawned from Node may sit in a kill-on-close job object: on start it re-executes itself with `CREATE_BREAKAWAY_FROM_JOB` when needed, and `panel.js` falls back to ExtendScript `File.execute()` if the child dies within 5 s.

## 7. File layout

```
hdr_hint_adobe_media_encoder/
  CMakeLists.txt                 targets hdrhint_core (static), hdrhint_ui (static), HdrHint (WIN32 exe), hdrhint_tests (ctest)
  cmake/MsvcFlags.cmake          /W4 /permissive- /utf-8 /EHsc /Zc:preprocessor /guard:cf; UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  resources/                     HdrHint.manifest (PerMonitorV2, longPathAware, comctl32 v6), HdrHint.rc, HdrHint.ico, resource.h, guide.md (RCDATA copy)
  docs/GUIDE.md                  export guide, also rendered in the Guide tab
  docs/ARCHITECTURE.md           this file
  THIRD_PARTY_NOTICES.md         nlohmann/json, CSInterface.js, json2.js, MKVToolNix, ffprobe
  third_party/nlohmann/          json.hpp 3.11.3 + LICENSE.MIT
  cep/com.everett.hdrhint/       CSXS/manifest.xml, .debug, index.html, css/panel.css, js/CSInterface.js, js/panel.js, jsx/host.jsx, jsx/json2.js, icons/
  cep/startup/HdrHintLauncher.jsx AME Scripts\Startup script: launches the exe when Media Encoder starts
  scripts/                       check_tu.ps1, install_panel.ps1, uninstall_panel.ps1, install_startup.ps1, install_standalone.ps1, make_test_hdr_mp4.ps1, simulate_ame_export.ps1, screenshot.ps1
  src/main.cpp                   wWinMain: job-object breakaway → single instance → logger → settings → engine → UI → loop → ordered shutdown
  src/platform/                  Win.h, Handle.h, Utf.h, KnownFolders.h, FileIo.h, DirectoryWatch.h, Process.h, RecycleBin.h, Registry.h,
                                 SingleInstance.h, NamedPipe.h, Time.h, WinVersion.h
  src/core/                      Expected.h, Logger.h, PathUtil.h, IniFile.h, Settings.h, HdrPresets.h, EventQueue.h, EngineEvents.h, JobModel.h,
                                 JobStore.h, Engine.h, AmeLogLocator.h, AmeLogParser.h, AmeLogTailer.h, FolderWatcher.h, FileReadiness.h,
                                 Mp4Boxes.h, MediaProbe.h, MkvmergeLocator.h, MkvmergeRunner.h, MuxWorker.h, IpcProtocol.h, IpcServer.h
  src/ame/                       AmeProcess, PanelWindowLocator, DockController, PanelInstaller
  src/ui/                        gfx/ (D3D11, DXGI, D2D, DWrite, DComp), anim/ (springs, timeline), theme/, core/ (widgets, layout, focus),
                                 window/ (WindowHost, PopupWindow, TrayIcon), controls/, screens/ (Queue, Settings, Guide)
  luts/                          the YouTube SDR-hint .cube LUTs, copied next to the exe at build time
  tests/                         test_main.cpp + test_ame_log_parser, test_path_util, test_ini, test_settings, test_presets, test_quoting,
                                 test_mp4_boxes, test_ipc_protocol
```

## 8. Build and test

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release

pwsh scripts/check_tu.ps1 -File src/core/AmeLogParser.cpp      # one TU at /W4, no link, no CMake

pwsh scripts/make_test_hdr_mp4.ps1                                       # %TEMP%\hdrhint_tests\test_*.mp4 (PQ, PQ+SEI, HLG, SDR, unicode, >260 chars)
hdr_hint\build\Release\HdrHint.exe --process %TEMP%\hdrhint_tests\test_pq_noSEI.mp4   # headless one-shot
"C:\Program Files\MKVToolNix\mkvmerge.exe" -J %TEMP%\hdrhint_tests\test_pq_noSEI_REC709_HINT.mkv

pwsh scripts/simulate_ame_export.ps1 -Sandbox %TEMP%\hdrhint_sim -Clip %TEMP%\hdrhint_tests\test_pq_noSEI.mp4 [-Fail] [-Truncate] [-Twice] [-NoLog]
pwsh scripts/screenshot.ps1 -Out shot.png                                # PrintWindow(PW_RENDERFULLCONTENT) of the main window
pwsh scripts/install_panel.ps1                                           # CEP panel + PlayerDebugMode
```

Post-build copies `luts/*.cube`, `cep/` and `GUIDE.md` next to the exe. The unit tests cover the log parser on real log excerpts (split writes, odd byte, failure blocks), presets → exact argv for all 11, argv quoting, INI round-trip, hint-path rules, the box walker on synthetic trees, and IPC JSON.

## 8b. Verified without AME (2026-09-16, build from this tree)

Everything below ran on the developer machine (AME 26.2.2 installed, mkvmerge v82.0) with the
real executable, driven by `scripts/simulate_ame_export.ps1` and the synthetic clips from
`scripts/make_test_hdr_mp4.ps1`.

| Check | Result |
|---|---|
| `cmake --build` of every target at `/W4` | 0 errors, 0 warnings |
| `hdrhint_tests` | 121 passed, 0 failed |
| `hdrhint_cli --process test_pq_noSEI.mp4` | `_REC709_HINT.mkv` with matrix 9 / range 1 / transfer 16 / primaries 9, MaxCLL 1000, MaxFALL 400, mastering 1000 / 0.0001, D65, one `application/x-cube` attachment (7 415 123 bytes), 1 audio track, duration 3.03 s |
| Simulated PQ export (sidecars → exclusive-lock finalize → UTF-16 log block) | job Discovered → Encoding → Finalizing → Ready (log-confirmed, transfer PQ from the `Video:` line) → Muxing → Verifying → Done; original moved to the Recycle Bin (bin count 17 → 18) |
| Simulated HLG export (`-ColorSpace "Rec. 2100 HLG"`) | preset `generic_hlg`, no LUT, Done |
| Simulated SDR export (`-ColorSpace "Rec. 709"`) | `SkippedSdr`, no mux, "Run anyway" offered |
| Simulated failure (`-Fail`, block in both logs) | job Failed with "The Operation was interrupted by user"; no mux; no phantom second generation on later file events |
| Manual file by command line (`HdrHint.exe "test_ünïcode 日本.mp4"`) | forwarded to the running instance, transfer PQ from ffprobe, Done, recycled |
| Settings migration from the Python tool's `settings.ini` | suffix, PQ LUT, watch folder and Recycle Bin policy imported once |
| Window: X hides to the tray, `--show` restores, single instance enforced | pass |
| UI screenshots (`--screenshot`, mock data, dark/light, 520 and 720 wide, 420 docked look) | all tabs render; popups wrap in the narrow layout |
| CEP panel install (`scripts/install_panel.ps1`) | files under `%APPDATA%\Adobe\CEP\extensions\com.everett.hdrhint`, `config.json`, `HKCU\Software\HdrHint\ExePath`, `PlayerDebugMode = 1` |

## 9. Spike results (fill in after the AME live test)

Run each check on AME 26.2.2 and record the outcome here before trusting the assumption in code.

| # | Check | Expected | Result | Notes |
|---|---|---|---|---|
| 1 | Panel loads: Window > Extensions > HDR Hint, one new `CEPHtmlEngine.exe` browser process, `%TEMP%\CEP12-AME.log` lists `com.everett.hdrhint` | loads | **no** | AME 26.2.2 lists the menu item but never starts `CEPHtmlEngine.exe`; the panel stays an empty black frame. Overlay docking (§6) covers that frame instead. |
| 2 | Node available in the panel: `typeof cep_node`, `cep_node.require('net')`, `process.versions.node` | object / module / 17.x | | |
| 3 | `CEPHtmlEngine.exe` command lines: which process carries `--type=`; does the extension id appear | browser has no `--type=` | | |
| 4 | Parent of `hello.pid` is the browser `CEPHtmlEngine.exe` | yes | | |
| 5 | HWND tree: a CEF HWND owned by the browser pid under a `DroverLord - Window Class` host; class names | present | no CEF HWND | Extension frame = top-level `DroverLord - Window Class` (WS_CAPTION, owned by main) → `DroverLord - Frame Window` → `OS_ViewContainer`. Real panels add a `DroverLord - TabPanel Window`. |
| 6 | `GetWindowRect(cef)` equals the visible panel area at 100 % and 150 % (ruler on screenshot) | ±2 px | | |
| 7 | `GetProcessDpiAwareness(browser pid)` and `GetDpiForWindow(ameMain)` | record | | |
| 8 | Background tab: `IsWindowVisible(cef)`, `__adobe_cep__.isWindowVisible()`, `WindowVisibilityChanged` fires | record | | |
| 9 | Ownership: `GWLP_HWNDPARENT` popup hides on AME minimise, returns on restore | yes | pass | Owner = the extension frame (or the main frame for picked panels). Menu item id 32766 (`WM_COMMAND`) opens the frame; a tray-started app docks within the 5 s redock interval. |
| 10 | Export Settings dialog opens above the owned popup (occlusion check hides ours) | hidden | | |
| 11 | Close AME while owned: does the popup receive `WM_DESTROY`? Recreation path works, tray icon re-added | no crash | pass | Owner destroyed → `GWLP_HWNDPARENT` clear fails with an invalid handle → window recreated floating, toast "panel went away", re-docks when AME and the frame are back. |
| 12 | Workspace switch: is AME's main HWND recreated? | record | | |
| 13 | ExtendScript events on a 5 s export: which of `onItemEncodingStarted`, `onEncodingItemProgressUpdate`, `onItemEncodeComplete`, `onBatchItemStatusChanged` fire; exact `result` string; absolute backslash paths | record | | |
| 14 | Pipe round-trip from the panel: `hello` → `welcome`, pings / pongs at 5 s | yes | | |
| 15 | Spawned exe: `IsProcessInJob`, kill-on-close flag, survives panel close; breakaway re-exec or `File.execute()` fallback used | survives | | |
| 16 | `index.html` loads `CSInterface.js` (no CSP block); status line changes from "Starting…" | yes | | |
| 17 | Log block lands within about 1 s of the CEP completion event | ≤ 1 s | | |
| 18 | Elevation mismatch (AME elevated, HdrHint not): message shown instead of a silent dock failure | message | | |
| 19 | End-to-end: 20 s PQ export with "HEVC 4k 59.94 HDR" (HDR10 metadata OFF) → `<name>_REC709_HINT.mkv` within about 10 s; `mkvmerge -J` shows transfer 16 / primaries 9 / CLL / one `application/x-cube` attachment | pass | | |
| 20 | Docked window follows drag, resize, workspace switch, monitor change; hides on minimise / dialog / background tab; floats on panel close; re-docks on reopen | pass | partial | Follows `MoveWindow` of the frame and stays visible during the drag; clicks work while docked (real-input test on the Settings tab). Workspace switch and monitor change not yet exercised live. |

## 10. Two platforms, one codebase

Everything above the OS line is shared: the engine, the job model, the parsers, the IPC
protocol, the UI toolkit (widgets, layout, springs, themes), the screens and the view-models.
Below it each platform has its own layer behind the same headers.

| Concern | Header | Windows | macOS |
|---|---|---|---|
| Type vocabulary | `platform/Win.h` | `<windows.h>` | `DWORD`, `RECT`, the `ERROR_*` numbers and friends, defined over POSIX; errno maps onto the Win32 codes |
| Events / waiting | `platform/Event.h` | `CreateEvent`, `WaitForMultipleObjects` | a self-pipe per event, `poll()` |
| Files, locks | `platform/FileIo.h` | `CreateFileW`, `\?\` long paths, deny-write probe | `open`/`F_FULLFSYNC`, `renamex_np`, libproc's open-for-write scan as the deny-write probe |
| Folder watching | `platform/DirectoryWatch.h` | `ReadDirectoryChangesW` | FSEvents, reconciled against a snapshot into the same `FILE_ACTION_*` records |
| Processes | `platform/Process.h` | `CreateProcessW`, job objects, Toolhelp | `posix_spawn`, kqueue exit watch, libproc |
| Panel ↔ app channel | `platform/NamedPipe.h` | `\.\pipe\HdrHint` | Unix socket in Application Support (mode 0600) |
| Single instance | `platform/SingleInstance.h` | named mutex + `WM_COPYDATA` | `flock` + a socket that forwards argv |
| Trash / reveal | `platform/RecycleBin.h` | `IFileOperation`, `SHOpenFolderAndSelectItems` | `NSFileManager trashItemAtURL`, `NSWorkspace` |
| Known folders | `platform/KnownFolders.h` | `%APPDATA%`, `%LOCALAPPDATA%`, exe folder | Application Support, `~/Library/Logs`, the bundle's Resources |
| OS nouns in text | `platform/Terms.h` | Recycle Bin, Explorer, tray | Trash, Finder, menu bar |
| Drawing | `ui/gfx/Canvas.h`, `TextCache.h` | Direct2D, DirectWrite | Core Graphics, Core Text (`ui/mac/CanvasMac.mm`, `TextCacheMac.mm`) |
| Windows | `ui/core/WindowServices.h` | `WindowHost`, `PopupWindow` (HWND, DComp swap chains) | `MacWindowHost`, `MacPopupWindow` (NSWindow / NSPanel + a flipped layer-backed view) |
| Tray | — | `TrayIcon` (Shell_NotifyIcon, balloons) | `MacStatusItem` (NSStatusItem, Notification Center) |
| AME docking | `ame/DockControl.h` | `DockController` (window ownership, WinEvent hooks) | `MacDockControl`: `supported() == false`, the dock toggles hide |
| Entry point | — | `src/main.cpp` (`wWinMain`) | `src/app/mac/main.mm` (`NSApplication`, menu bar agent) |

Conventions that keep it that way:

- Windows-only units in shared folders end in `Win.cpp`; macOS units live in `mac/` folders or
  `platform/posix/`. CMake picks by path, so adding a file never needs a CMake edit.
- Widgets speak Win32 virtual keys and `WHEEL_DELTA` units everywhere. The Cocoa view
  translates at the boundary: Command is `Modifiers::ctrl`, Option+arrows are the word
  moves, Command+arrows are Home/End, touchpad deltas are scaled so one point of finger travel
  is one dip, and the momentum phase is dropped because `ScrollView` runs its own inertia.
- Frames are demand-driven on both: the Win32 loop blocks on messages or the swap chain's
  latency waitable; the Cocoa host re-arms one common-modes run-loop timer for "now", "next
  60 Hz tick" or "next timeline deadline", and does nothing while the UI is idle.
- `--screenshot` renders offscreen (a CGBitmapContext on macOS), so CI captures every tab on a
  headless runner and uploads the PNGs as an artifact.
