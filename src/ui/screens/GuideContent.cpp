// ---------------------------------------------------------------------------
// GuideContent.cpp - the export recipe line and the embedded guide markdown.
// ---------------------------------------------------------------------------
#include "ui/screens/GuideContent.h"

#include <string_view>

namespace hh::ui {

namespace {

// The separator between recipe items. A middle dot reads well in a toast,
// in a chat message and in a plain text file alike.
constexpr std::wstring_view kSeparator = L" · ";

// The export recipe, in the order the Media Encoder dialog presents the
// settings. Kept as individual items so the joiner owns the punctuation.
constexpr std::wstring_view kRecipe[] = {
    L"H.265 (HEVC)",
    L"Hardware Encoding",
    L"Main10",
    L"Level 5.2 (5.1 if hardware greys out)",
    L"Tier High",
    L"VBR 1-pass 100 Mbps (4K 59.94)",
    L"Export Color Space Rec. 2100 PQ (or HLG)",
    L"Include HDR10 metadata OFF",
    L"Render at Maximum Depth ON",
    L"Use Maximum Render Quality ON",
    L"AAC 320 kbps 48 kHz",
};

// The guide is split into a few adjacent literals: MSVC caps the size of a
// single string literal well below what the whole document needs, and the
// pieces are appended back into one document at run time.
constexpr const wchar_t* kGuidePart1 = LR"HHGUIDE(# HDR Hint — Export Guide (Premiere Pro + Adobe Media Encoder)

HDR Hint watches Adobe Media Encoder (AME). The moment an export finishes it runs mkvmerge to (1) write the Rec. 2100 colour metadata into a Matroska container and (2) attach a `.cube` LUT that YouTube uses to build the SDR version of your video. Result: `<name>_REC709_HINT.mkv` next to the original export. Nothing is re-encoded; video and audio are copied bit-for-bit. Seconds, not hours.

Menu labels that could not be verified on every Premiere/AME build are marked *(label may differ)*.

## 1. Premiere Pro set-up

### 1.1 Working colour space

| Step | Where | Value |
|---|---|---|
| 1 | Lumetri Color > **Settings** tab *(label may differ; older builds: Sequence > Sequence Settings > Color Management)* | **Working Color Space: Rec. 2100 PQ** (or **Rec. 2100 HLG** if you deliver HLG). Pick one and keep it everywhere below. |
| 2 | Same panel | **Auto Tone Map Media: ON**, so SDR clips are lifted into the HDR timeline without clipping. |
| 3 | **Preferences > General** *(label may differ)* | **Display Color Management: ON** on an HDR monitor. On an SDR monitor the Program Monitor shows a tone-mapped preview; that is expected, not a bug. |

### 1.2 DJI D-Log M sources (Osmo Pocket 3, Mavic, Air, Mini)

Select the clips in the Project panel, then **Modify > Interpret Footage… > Color Management**. Pick **one** of the two options. Never stack both.

| Option | Media Color Space | Input LUT | What you get |
|---|---|---|---|
| **Option A — simple** | Leave at **Rec. 709** (override off) | DJI's official **D-Log M to Rec.709** LUT (`pocket3_dlogm_to_rec709.cube` ships in `luts\`) | Auto Tone Map lifts the clip into the HDR sequence. Predictable, but highlights are capped at SDR white. |
| **Option B — true HDR** | Tick **Override Media Color Space** and choose **Rec. 2100 HLG** | A **D-Log M to Rec.2100 HLG** LUT | Full highlight range. The LUT's output space and the declared Media Color Space must agree; if they disagree Premiere converts twice and the image is wrong. |

- If the Media Color Space list contains a DJI D-Log M entry *(label may differ; some builds only list "DJI D-Log")* you may pick it instead of a LUT and set Input LUT to None. Not every build lists it, so the LUT route is the safe one.
- The Input LUT stage is the right place for log normalisation: it runs before colour management and before any Lumetri effect.
- **Lumetri > Creative > Look is not for log normalisation.** That stage is for grading after normalisation; using it for log conversion in a Rec. 2100 sequence clips highlights.
- Clips shot in HLG (Pocket 3 "HLG" mode) need nothing; Premiere reads the tags.

### 1.3 Check

Lumetri Scopes, set to **HDR** with nits: highlights should sit above 100 nits without a hard wall at 1000 nits. Everything squashed under 100 nits means an Input LUT or the working space is wrong.

## 2. Export settings (Premiere Export page or AME Export Settings)

| Setting | Value |
|---|---|
| Format | **H.265 (HEVC)** |
| Preset | Start from "Match Source – Adaptive High Bitrate", change the rows below, save it as your own preset once. |
| Frame size / rate | Match the sequence (e.g. 3840x2160, 59.94 fps), Progressive, square pixels |
| Performance | **Hardware Encoding** (NVIDIA / Intel / AMD) |
| Profile | **Main10** |
| Level | **5.2**. If Hardware Encoding greys out at 5.2, try **5.1** (spec-legal for 4K 59.94) before giving up on hardware. |
| Tier | **High** *(label may differ: a "Tier" dropdown or a "High Tier" checkbox)*. If there is no Tier control, the hardware encoder decides; a 100 Mbps target that comes out at ~60 Mbps means Main tier was used. |
| Bitrate Encoding | **VBR, 1 pass** |
| Target Bitrate | **100 Mbps** for 4K 59.94 (60–80 Mbps for 4K 29.97; 35–50 Mbps for 1080p HDR) |
| Export Color Space | **Rec. 2100 PQ** (or **Rec. 2100 HLG**, matching the sequence) |
| Include HDR10 Metadata | **OFF** — see 2.1 |
| Render at Maximum Depth | **ON** |
| Use Maximum Render Quality | **ON** (bottom of the AME dialog / "General" on the Premiere Export page) |
| Audio | **AAC**, Stereo, **48 kHz**, Audio Quality High, **320 kbps** |
| Destination | A **local drive**. Network shares hold handles open and HDR Hint has to wait for them. |

Why Level and Tier matter: HEVC Level 5.1 / 5.2 **Main** tier caps the bitrate at 40 / 60 Mbps; **High** tier lifts that to 160 / 240 Mbps. Without High tier the encoder quietly lowers your 100 Mbps target. Level 6.1 Main tier allows 120 Mbps if you cannot get a High-tier control.

### 2.1 Why "Include HDR10 Metadata" stays OFF

Ticking it flips Performance to **Software Encoding**: Adobe's hardware path does not write the mastering-display and content-light SEI messages. On a 4K 59.94 timeline that turns a 15-minute NVENC export into hours. HDR Hint writes the equivalent metadata at the **container** level in about a second, without re-encoding, and the container is what YouTube reads.
)HHGUIDE";

constexpr const wchar_t* kGuidePart2 = LR"HHGUIDE(
## 3. What HDR Hint writes

Every preset sets the four base colour elements on the video track. PQ presets add mastering-display and light-level metadata; HLG does not use any.

| Preset (id) | Matrix / Range / Transfer / Primaries | MaxCLL / MaxFALL | Mastering display |
|---|---|---|---|
| Generic Rec.2100 PQ / HDR10 (1000 nits) (`generic_pq_1000`), **default for PQ** | 9 BT.2020 nc / 1 limited / 16 PQ / 9 BT.2020 | 1000 / 400 cd/m² | R 0.708,0.292 · G 0.170,0.797 · B 0.131,0.046 · WP 0.3127,0.329 (D65) · 1000 / 0.0001 cd/m² |
| Generic Rec.2100 PQ / HDR10 (4000 nits) (`generic_pq_4000`) | same | 4000 / 1000 | same primaries, 4000 / 0.0001 cd/m² |
| iPhone Dolby Vision / HDR10 (PQ); Sony HDR10 (PQ) | same | 1000 / 400 | same as the 1000-nit preset |
| Generic Rec.2100 HLG (`generic_hlg`), **default for HLG**; DJI Osmo Pocket 3 (`dji_pocket3_hlg`); DJI Mavic / Air / Mini; Sony HLG; Panasonic HLG | 9 / 1 / 18 HLG / 9 | none | none |
| SDR Rec.709 (`sdr_rec709`), only with the "tag SDR" policy | 1 BT.709 / 1 / 1 BT.709 / 1 BT.709 | none | none |
| Custom | Whatever you enter in Settings; empty fields emit no flag at all. | | |

The light-level and mastering values are conventional declared values, not measurements. YouTube treats them as hints.

**LUT attachment.** The selected `.cube` is stored as a Matroska attachment with MIME type `application/x-cube`. YouTube reads it as the HDR-to-SDR conversion for the SDR streams it generates.

- Input must match the export: a PQ export needs a PQ (Rec. 2020) input LUT, an HLG export needs an HLG input LUT.
- Output is Rec. 709, gamma 2.4: `PQ1000_to_Rec709_SDR_g24_YouTubeHint.cube` (bundled) is exactly that for PQ exports and is the default.
- Camera-log LUTs are not hints: `pocket3_dlogm_to_rec709.cube` expects D-Log M input, not PQ. Attached as a hint it produces a crushed, discoloured SDR version. It belongs in Premiere's Input LUT slot (1.2) and nowhere else.
- HLG default is **None**. Leave it unless you have a real HLG-to-Rec.709 LUT. Any `.cube` in the LUT folder can be picked per job.

**Output naming.** `<export name>_REC709_HINT.mkv` in the export folder (suffix and folder are configurable in Settings). If that name already exists the new file gets ` (2)`, ` (3)`, and so on; nothing is overwritten unless you switch the conflict policy.

**Recycle Bin.** With "Move original to Recycle Bin" on (default), the `.mp4` is moved only after the `.mkv` was verified with `mkvmerge -J` and the original is unchanged since the mux. It is always a Recycle Bin move, never a permanent delete. On network drives, or volumes where the Recycle Bin is disabled, the original stays and the job says why.

## 4. YouTube

- Upload the `_REC709_HINT.mkv`, not the `.mp4`. YouTube accepts `.mkv` directly.
- If a file carries no ST 2086 mastering metadata, YouTube states it assumes the values of a **Sony BVM-X300** mastering display. That is why the PQ presets write 1000 / 0.0001 cd/m² explicitly instead of leaving it to guesswork.
- HDR processing lags SDR: the **HDR** badge in the quality menu and the LUT-based SDR version can appear minutes to hours after the upload finishes. The first thing viewers see may be SDR-only.
- Check on an HDR display for the badge; on an SDR display compare the look with your own tone-mapped preview.
- Do not re-upload while processing is still running; changes are invisible until YouTube finishes.
)HHGUIDE";

constexpr const wchar_t* kGuidePart3 = LR"HHGUIDE(
## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| "mkvmerge not found" or "too old" | Install MKVToolNix (mkvtoolnix.download) or point Settings > Tools > mkvmerge path at `mkvmerge.exe`. Version 15 or newer; tested with v82.0. |
| "Waiting for AME to release file" for a long time | AME holds the output until it finalises the `moov` atom. If AME crashed the file is incomplete: re-export. Network drives keep handles open: export locally. |
| Job shows "SDR export", skipped | The AME log said Rec. 709. Check Export Color Space. If the export really is HDR, pick the preset in the row and press Run. |
| "HDR10 metadata already present" | The source already carries in-band SEI (an export with Include HDR10 Metadata ON, or camera HDR10). HDR Hint still writes the Matroska elements, which take precedence in the container; the bitstream is untouched. Switch the in-band policy to "hold" in Settings if you want to decide per file. |
| Panel not listed under Window > Extensions | Settings > Media Encoder > Install panel, restart AME, confirm `HKCU\Software\Adobe\CSXS.12\PlayerDebugMode` is `1`. One malformed character in the manifest hides the panel silently. The reason is in `%TEMP%\CEP12-AME.log` and `%TEMP%\CEPHtmlEngine12-AME-26.2.2-com.everett.hdrhint.log` (plus its `-renderer.log` twin). |
| Panel loads but says "Not installed" | It could not find `HdrHint.exe`. Click Locate… in the panel, or run HDR Hint once so it writes `HKCU\Software\HdrHint\ExePath`. |
| Docked window drifts from the panel | Press Undock, then Dock. If it persists (mixed-DPI setups), attach `%LOCALAPPDATA%\HdrHint\logs\hdrhint.log` to the report. |
| Docked window vanished | It hides while AME is minimised, while a dialog covers the panel, and while the panel tab is in the background. Bring the tab forward, or click the tray icon. |
| No HDR badge on YouTube | `mkvmerge -J file.mkv`: the video track must show `color_transfer_characteristics: 16` (PQ) or `18` (HLG) and there must be an `attachments` entry. If both are there, wait for processing. |
| Nothing happens after an export | Look at the status dot: is the AME log found? Premiere direct exports never write the log, and network exports may not either; add the export folder under Settings > Watch folders. |

## 6. Docking the panel above the Queue

1. Settings > Media Encoder > **Install panel** (or run `scripts\install_panel.ps1`), then restart AME.
2. **Window > Extensions > HDR Hint** *(label may differ)*. A small panel opens and launches `HdrHint.exe`; the native window snaps over the panel and follows it.
3. Drag the **HDR Hint** tab and drop it on the top edge of the **Queue** panel so it sits above the Queue. AME remembers the workspace, so this is a one-time move.
4. Closing the tab undocks HDR Hint into a floating window; it keeps working from the log. Reopen the tab and it re-docks.
5. Panel buttons: **Launch** starts the app if it is not running, **Show** brings the window forward, **Undock / Dock** toggles floating mode. The tray icon does the same from Windows.
)HHGUIDE";

} // namespace

// ---------------------------------------------------------------------------
// guideSummaryText
// ---------------------------------------------------------------------------
std::wstring guideSummaryText(const SettingsView* view)
{
    // Reserve once: the recipe is a couple of hundred characters plus the
    // output line, so a single allocation covers the whole build-up.
    std::wstring out;
    out.reserve(320);

    // Join the fixed recipe items with the separator; the first item gets none.
    for (std::wstring_view item : kRecipe) {
        if (!out.empty()) out.append(kSeparator);
        out.append(item);
    }

    // The output naming rule only makes sense when the caller has settings
    // to read the suffix from; the recipe alone stays generic otherwise.
    if (view) {
        out.append(kSeparator);
        out.append(L"Output: <stem>");
        out.append(view->suffix);
        out.append(L".mkv");
    }
    return out;
}

// ---------------------------------------------------------------------------
// builtInGuideMarkdown
// ---------------------------------------------------------------------------
std::wstring builtInGuideMarkdown()
{
    // Append the pieces into one document. Each part starts on a fresh line
    // so heading detection in the markdown parser never sees a join.
    const std::wstring_view parts[] = {kGuidePart1, kGuidePart2, kGuidePart3};

    // Size the buffer once from the known literal lengths.
    size_t total = 0;
    for (std::wstring_view part : parts) total += part.size();

    std::wstring md;
    md.reserve(total);
    for (std::wstring_view part : parts) md.append(part);
    return md;
}

} // namespace hh::ui
