# HDR Hint

**The Adobe Media Encoder companion that finishes your HDR exports for you. On Windows and macOS.**

Export from Premiere Pro or Adobe Media Encoder with hardware encoding. The moment a file
lands, HDR Hint runs mkvmerge to write the Rec. 2100 colour metadata, attaches the `.cube`
LUT YouTube uses to build the SDR version, verifies the result and moves the original to the
Recycle Bin (or the Trash). No re-encode. No clicking. Seconds, not hours.

<p align="center">
  <img src="docs/img/queue-dark.png" width="46%" alt="HDR Hint queue, dark">
  &nbsp;&nbsp;
  <img src="docs/img/queue-light.png" width="46%" alt="HDR Hint queue, light">
</p>

Native C++20 with its own UI toolkit (no UI framework, no web view), one codebase:

| | Windows 10 / 11 | macOS 13.3+ (Apple silicon and Intel) |
|---|---|---|
| Rendering | Direct2D + DirectComposition, Mica | Core Graphics + Core Text, vibrancy |
| Window | custom chrome, docks inside AME | native traffic lights, full-size content |
| Lives in | the tray | the menu bar |
| Starts with | Media Encoder, or Windows | Media Encoder, or Open at Login |
| Deleted originals go to | the Recycle Bin | the Trash |

---

## Why this exists

AME's **"Include HDR10 metadata"** checkbox forces **software** encoding. On a 4K 59.94
timeline that turns a 15-minute NVENC or VideoToolbox export into hours.

So: leave the box off, keep the hardware encoder, and let HDR Hint add the equivalent metadata
at the container level afterwards. That is what YouTube reads anyway, and it takes about a
second. The attached LUT is how YouTube builds the SDR version for viewers without an HDR
display, instead of applying its own flat tone map.

## What it does

| Step | Detail |
|---|---|
| Detects the export | Three independent triggers: AME's own encoding log (`AMEEncodingLog.txt`), your render folders (AME's sidecar files appear minutes before the `.mp4`), and the CEP panel's `onItemEncodeComplete` event on hosts that run panels. |
| Waits for the file to be real | Deny-write probe, stable size, sidecars gone, MP4 box chain complete, log confirmation. AME's exclusive lock during finalize is respected, not fought. |
| Muxes | `mkvmerge` with the preset's colour flags (`--color-transfer-characteristics 16` for PQ, `18` for HLG, mastering display + MaxCLL/MaxFALL for PQ) and the `.cube` attached as `application/x-cube`. |
| Verifies | `mkvmerge -J` on the output: every colour element, track counts, the attachment's name / mime / size, duration. Only then is the file renamed. |
| Cleans up | The original goes to the **Recycle Bin** / **Trash**, never a permanent delete, and only if the file is unchanged since the mux. |

Per job you can change the preset, the LUT, whether to attach it, or hold the job. SDR exports
are shown and skipped. Failed exports show AME's own reason.

<p align="center">
  <img src="docs/img/docked-420.png" width="32%" alt="Docked in AME, 420 dip wide">
  &nbsp;
  <img src="docs/img/settings-dark.png" width="32%" alt="Settings">
  &nbsp;
  <img src="docs/img/guide-dark.png" width="32%" alt="Built-in export guide">
</p>

## Requirements

**Both platforms**

- [MKVToolNix](https://mkvtoolnix.download/) 15 or newer. Auto-detected: `Program Files` on
  Windows; `/Applications/MKVToolNix-*.app`, Homebrew (`brew install mkvtoolnix`) and MacPorts
  on macOS. Settings > Tools takes an explicit path too.
- Adobe Media Encoder 2019 or newer, **optional**: without it HDR Hint runs as a watch-folder
  tool. Developed and tested against AME 2026 (26.2.2).
- Optional: `ffprobe` (`brew install ffmpeg` / `C:\ffmpeg\bin`) for colour-space detection of
  files that never touched AME's log.

**Windows**: Windows 10 / 11 (Mica needs 11 22H2; older builds get a solid background).
To build: Visual Studio 2022 (MSVC v143), CMake 3.24+, Windows SDK 10.0.22621 or newer.

**macOS**: macOS 13.3 Ventura or newer. To build: Xcode 15+ or the Command Line Tools,
CMake 3.24+ (`brew install cmake`), Ninja optional (`brew install ninja`).

## Build

The quickest route on either platform is `make` (GNU make; on Windows the one from Git for
Windows, MSYS2 or Chocolatey works):

```sh
git clone https://github.com/Kemerd/hdr_hint_adobe_media_encoder.git
cd hdr_hint_adobe_media_encoder
make test          # configure, build Release, run the unit tests
```

| Target | Does |
|---|---|
| `make` / `make release` | Release build of the app, the CLI and the tests in `build/` |
| `make debug` | Debug build in `build-debug/` |
| `make test` | build, then `ctest` |
| `make app` / `make cli` | only `HdrHint` / only `hdrhint_cli` |
| `make run` | build and start the app |
| `make screenshots` | every tab, dark and light, rendered offscreen to `build/screenshots/` |
| `make package` | CPack: `HdrHint-<ver>.dmg` on macOS, `HdrHint-<ver>-win64.zip` on Windows |
| `make install PREFIX=…` | `cmake --install` into a folder (default `./dist`) |
| `make panel` / `make unpanel` | install / remove the Media Encoder panel for this user |
| `make icon` | regenerate `resources/macos/HdrHint.icns` (needs Pillow) |
| `make clean` / `make distclean` | clean the build tree / delete it |

Knobs: `CONFIG=Debug|Release|RelWithDebInfo`, `BUILD_DIR=…`, `UNIVERSAL=ON` (arm64 + x86_64
on macOS), `GENERATOR=…`, `JOBS=n`, `EXTRA_CMAKE_FLAGS=…`.

Prefer plain CMake or the platform scripts? Same result:

```sh
# macOS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release [-DHH_UNIVERSAL=ON]
cmake --build build && ctest --test-dir build
scripts/build.sh [--debug] [--universal] [--target HdrHint] [--no-test]
```

```powershell
# Windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release; ctest --test-dir build -C Release
pwsh scripts/build.ps1 [-Config Debug] [-Target HdrHint] [-NoTest]
```

The Windows app is `build\Release\HdrHint.exe` with its LUTs (`luts\`), the CEP panel (`cep\`)
and the guide (`GUIDE.md`) next to it. On macOS it is `build/HdrHint.app`, the same assets in
`Contents/Resources`, ad-hoc signed so it runs on Apple silicon straight from the build.
`-DHH_CORE_ONLY=ON` builds just the engine, the CLI and the tests.

## Install

### Standalone: just watch folders

HDR Hint does not need Adobe software. It watches whatever folders you list and
processes any new video that appears, which is enough on its own if you export from
DaVinci, Handbrake, OBS or anything else.

```powershell
# Windows: %LOCALAPPDATA%\Programs\HdrHint, a Start Menu shortcut, optional start with Windows
pwsh scripts/install_standalone.ps1 -WatchFolder "D:\Renders" -StartWithWindows -Launch
```

```sh
# macOS: ~/Applications/HdrHint.app (or /Applications with --system), optional open at login
scripts/install_standalone.sh --watch ~/Movies/Renders --login --launch
```

Both copy the built app out of the repo (so rebuilding or moving it never breaks the
install), and both have an uninstall switch (`-Uninstall` / `--uninstall`). Nothing is
registered as a service and nothing is written outside your own user account. On macOS the
DMG from `make package` works too: drag HDR Hint to Applications.

Settings has separate switches for the two triggers, **From Media Encoder** and
**From watch folders**, so you can run either half on its own. A file seen by both is
still only processed once: every trigger resolves to the same job.

### Let Media Encoder run it

```powershell
# Windows
pwsh scripts/install_startup.ps1             # AME launches HDR Hint when it starts
.\build\Release\HdrHint.exe --install-panel  # adds Window > Extensions > HDR Hint
```

```sh
# macOS (the Startup folder is inside AME's install folder: sudo when it is root-owned)
sudo scripts/install_startup.sh --app /Applications/HdrHint.app
scripts/install_panel.sh --app /Applications/HdrHint.app     # or: make panel
```

The first line copies a small startup script into Media Encoder's own `Scripts/Startup`
folder. AME evaluates it at launch and starts HDR Hint into the tray / menu bar; HDR Hint
quits when AME quits. **Nothing runs at login and nothing is installed as a service.** The
uninstall switches reverse it. The second line registers the panel (and sets CEP's
`PlayerDebugMode`, which unsigned panels need).

**Windows: docking.** Restart Media Encoder and:

1. **Window > Extensions > HDR Hint.** AME opens an empty panel and HDR Hint covers it within
   a few seconds, following it wherever it goes.
2. Drag the **HDR Hint** tab onto the top edge of the **Queue** panel. AME remembers the
   workspace, so this is a one-time move.

Docking is the default and survives AME restarts. **Dock inside AME** in the footer is the off
switch. Closing the panel floats HDR Hint into its own window and it keeps working from the
log. Running it with no panel at all is fine too: it sits in the tray and watches.

> On AME 2026, Window > Extensions opens an empty frame because Media Encoder is not a CEP
> host and never starts the panel's engine. HDR Hint covers that frame with its own window,
> which is why it looks and behaves like a native panel on every AME version.

**macOS: floating, by design.** Docking relies on Win32 window ownership, which macOS does
not offer across apps, so the Mac build is a floating window (optionally always on top) plus a
menu bar item, and the dock switches are hidden. Everything else is identical: the log
tailer, the watch folders, the queue, the presets, the panel's launch and commands.

## Export settings (the short version)

Full guide with the Premiere colour-management set-up, D-Log M options and troubleshooting:
[docs/GUIDE.md](docs/GUIDE.md), also the **Guide** tab in the app.

| Setting | Value |
|---|---|
| Format | H.265 (HEVC), Performance **Hardware Encoding** |
| Profile / Level / Tier | **Main10** / **5.2** (5.1 if hardware greys out) / **High** |
| Bitrate | VBR 1-pass, **100 Mbps** for 4K 59.94 |
| Export Color Space | **Rec. 2100 PQ**, or HLG, matching the sequence |
| Include HDR10 Metadata | **OFF** — this is the whole point; HDR Hint adds it |
| Render at Maximum Depth / Use Maximum Render Quality | ON / ON |
| Audio | AAC 320 kbps, 48 kHz |

Then upload the resulting `.mkv` to YouTube. The HDR badge and the LUT-derived SDR version
appear after processing.

## Command line

The same flags on both platforms. On macOS the binary is
`HdrHint.app/Contents/MacOS/HdrHint` (or `open -a HdrHint --args …`).

```
HdrHint                                   normal start (window + tray icon / menu bar item)
HdrHint <file.mp4> [...]                  add files by hand (drag and drop works too)
HdrHint --show                            bring the running instance to the front
HdrHint --tray                            start hidden in the tray / menu bar
HdrHint --dock | --undock                 dock onto AME, or float (Windows)
HdrHint --install-panel | --uninstall-panel
HdrHint --process <file> [--preset id] [--lut path|none] [--suffix s]
HdrHint --snap out.png                    capture the running window
HdrHint --screenshot out.png [--tab 0|1|2] [--dark|--light] [--docked-look]

hdrhint_cli --process <file> ...          the same headless path as a console tool
hdrhint_cli --identify <file> | --presets | --mkvmerge
```

| | Windows | macOS |
|---|---|---|
| Settings | `%APPDATA%\HdrHint\settings.ini` | `~/Library/Application Support/HdrHint/settings.ini` |
| Job history, state | `%LOCALAPPDATA%\HdrHint\` | `~/Library/Application Support/HdrHint/` |
| Logs | `%LOCALAPPDATA%\HdrHint\logs\` | `~/Library/Logs/HdrHint/` (Console.app finds them) |
| Panel ↔ app channel | `\\.\pipe\HdrHint` | `~/Library/Application Support/HdrHint/HdrHint.sock` |
| CEP extensions | `%APPDATA%\Adobe\CEP\extensions\` | `~/Library/Application Support/Adobe/CEP/extensions/` |

## Presets

PQ presets write matrix 9 / range 1 / transfer 16 / primaries 9, plus MaxCLL 1000, MaxFALL 400,
Rec. 2020 mastering primaries, D65 white point and 1000 / 0.0001 cd/m² luminance. A 4000-nit
variant is included. HLG presets write transfer 18 and no mastering metadata, because HLG is
display-referred and does not need it. Add your own in `presets.json` next to `settings.ini`.

The mastering values are written explicitly on purpose: without ST 2086 metadata, YouTube
assumes a Sony BVM-X300 reference monitor.

## Verify it without touching AME

```powershell
# Windows
pwsh scripts/make_test_hdr_mp4.ps1                   # PQ / PQ+SEI / HLG / SDR clips via ffmpeg
.\build\Release\hdrhint_cli.exe --process "$env:TEMP\hdrhint_tests\test_pq_noSEI.mp4"
pwsh scripts/simulate_ame_export.ps1 -Sandbox "$env:TEMP\hdrhint_sim" `
     -Clip "$env:TEMP\hdrhint_tests\test_hlg.mp4" -ColorSpace "Rec. 2100 HLG"
```

```sh
# macOS
scripts/make_test_hdr_mp4.sh                         # the same clips, checked with ffprobe
build/hdrhint_cli --process "${TMPDIR}hdrhint_tests/test_pq_noSEI.mp4"
```

The simulator reproduces AME's on-disk behaviour (sidecar files, the exclusive lock while the
container is assembled, the UTF-16 log block) so the whole pipeline can be exercised on any
machine, with no Adobe software installed. `-Fail`, `-Twice`, `-Truncate` and `-NoLog` cover
the unhappy paths.

## Project layout

```
src/platform/     OS layer: files, processes, pipes/sockets, events, folders, trash
                  (Win32 in *.cpp, POSIX/macOS in posix/)
src/core/         engine: log tailer, folder watcher, readiness probe, mkvmerge runner, jobs, IPC
src/ui/           the toolkit (gfx, anim, theme, controls), screens, app view-models;
                  window/ = Win32 hosts (D2D/DComp), mac/ = Cocoa hosts (Core Graphics/Core Text)
src/ame/          AME process + panel installer; Windows docking; mac/ = macOS counterparts
src/app/mac/      the macOS app entry (main.mm); src/main.cpp is the Windows one
cep/              the CEP panel, and the Scripts/Startup launcher AME runs at boot
luts/             the YouTube SDR-hint LUTs shipped with the app
resources/        Windows .rc/.ico/manifest, macOS Info.plist template and .icns
tests/            unit tests (ctest)
scripts/          build, installers, test clip generator, AME simulator (.ps1 Windows, .sh macOS)
docs/             GUIDE.md, ARCHITECTURE.md
```

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the job state machine, the threading model,
the IPC protocol, the docking mechanics and how the two platform layers split the work.

## Credits

Built on the shoulders of [MKVToolNix](https://mkvtoolnix.download/), used as an external tool,
and YouTube's [hdr_metadata](https://github.com/YouTubeHDR/hdr_metadata) approach to the
SDR-hint attachment. Bundles [nlohmann/json](https://github.com/nlohmann/json) (MIT), Adobe's
`CSInterface.js` and Douglas Crockford's `json2.js`. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE).
