// ---------------------------------------------------------------------------
// Settings.cpp - typed application settings (%APPDATA%\HdrHint\settings.ini).
//
// load() reads what is present and keeps the compiled-in default for the
// rest; enumerations fall back to their default when the file holds junk and
// numbers are clamped to sane ranges. save() re-reads the existing file first
// so comments and key order the user added survive.
// ---------------------------------------------------------------------------
#include "core/Settings.h"

#include "core/IniFile.h"
#include "core/JobModel.h"
#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

namespace hh {

namespace {

constexpr const wchar_t* kLog = L"Settings";

// Section names, spelled once.
constexpr const wchar_t* kApp = L"app";
constexpr const wchar_t* kAme = L"ame";
constexpr const wchar_t* kWatch = L"watch";
constexpr const wchar_t* kDetect = L"detect";
constexpr const wchar_t* kOutput = L"output";
constexpr const wchar_t* kMkvmerge = L"mkvmerge";
constexpr const wchar_t* kLut = L"lut";
constexpr const wchar_t* kHdr = L"hdr";
constexpr const wchar_t* kJobs = L"jobs";
constexpr const wchar_t* kRecycle = L"recycle";
constexpr const wchar_t* kIpc = L"ipc";
constexpr const wchar_t* kLogSec = L"log";
constexpr const wchar_t* kMigration = L"migration";

// The Python tool's defaults, needed to tell "left alone" from "customised".
constexpr const wchar_t* kLegacyDefaultPrefix = L"_with_sdr_lut";
constexpr const wchar_t* kDefaultPqLutName = L"PQ1000_to_Rec709_SDR_g24_YouTubeHint.cube";
constexpr const wchar_t* kExeToken = L"<exe>";

/**
 * @brief String key: present -> trimmed value, absent -> default kept.
 */
void readString(const IniFile& ini, const wchar_t* sec, const wchar_t* key, std::wstring& target) {
    if (ini.has(sec, key)) {
        target = std::wstring(platform::trim(ini.get(sec, key)));
    }
}

/**
 * @brief Bool key: junk keeps the default.
 */
void readBool(const IniFile& ini, const wchar_t* sec, const wchar_t* key, bool& target) {
    target = ini.getBool(sec, key, target);
}

/**
 * @brief Int key clamped to [lo, hi]; junk keeps the default.
 */
void readInt(const IniFile& ini, const wchar_t* sec, const wchar_t* key, int& target, int lo, int hi) {
    const long long v = ini.getInt(sec, key, target);
    const long long clamped = std::clamp<long long>(v, lo, hi);
    if (clamped != v) {
        HH_LOG_WARN(kLog, L"[{}] {} = {} is out of range; clamped to {}", sec, key, v, clamped);
    }
    target = static_cast<int>(clamped);
}

/**
 * @brief Unsigned 64-bit key clamped to [lo, hi]; negatives keep the default.
 */
void readU64(const IniFile& ini, const wchar_t* sec, const wchar_t* key, uint64_t& target, uint64_t lo, uint64_t hi) {
    if (!ini.has(sec, key)) {
        return;
    }
    const long long v = ini.getInt(sec, key, -1);
    if (v < 0) {
        HH_LOG_WARN(kLog, L"[{}] {} is not a valid number; default kept", sec, key);
        return;
    }
    target = std::clamp<uint64_t>(static_cast<uint64_t>(v), lo, hi);
}

/**
 * @brief List key: present replaces the default (even when empty).
 */
void readList(const IniFile& ini, const wchar_t* sec, const wchar_t* key, std::vector<std::wstring>& target) {
    if (ini.has(sec, key)) {
        target = ini.getList(sec, key);
    }
}

/**
 * @brief Enumeration key: the lower-cased value must be one of @p allowed,
 *        otherwise the default stays and a warning is logged.
 */
void readEnum(const IniFile& ini, const wchar_t* sec, const wchar_t* key, std::wstring& target,
              std::initializer_list<const wchar_t*> allowed) {
    if (!ini.has(sec, key)) {
        return;
    }
    const std::wstring value = platform::toLowerInvariant(platform::trim(ini.get(sec, key)));
    for (const wchar_t* option : allowed) {
        if (value == option) {
            target = value;
            return;
        }
    }
    HH_LOG_WARN(kLog, L"[{}] {} = \"{}\" is not a known value; default \"{}\" kept", sec, key, value, target);
}

/**
 * @brief Drops empties and duplicates (case-insensitive) from a path list.
 */
std::vector<std::wstring> dedupePaths(const std::vector<std::wstring>& in) {
    std::vector<std::wstring> out;
    out.reserve(in.size());
    for (const std::wstring& raw : in) {
        const std::wstring_view t = platform::trim(raw);
        if (t.empty()) {
            continue;
        }
        const bool dup = std::any_of(out.begin(), out.end(),
                                     [t](const std::wstring& have) { return platform::iequals(have, t); });
        if (!dup) {
            out.emplace_back(t);
        }
    }
    return out;
}

/**
 * @brief Extension list normalised to ".ext" lower-case; empty -> defaults.
 */
std::vector<std::wstring> normaliseExtensions(const std::vector<std::wstring>& in) {
    std::vector<std::wstring> out;
    for (const std::wstring& raw : in) {
        std::wstring e = platform::toLowerInvariant(platform::trim(raw));
        if (e.empty()) {
            continue;
        }
        if (e.front() != L'.') {
            e.insert(e.begin(), L'.');
        }
        if (e.size() < 2 || path::hasInvalidFileNameChars(e)) {
            continue;
        }
        const bool dup = std::any_of(out.begin(), out.end(), [&e](const std::wstring& have) { return have == e; });
        if (!dup) {
            out.push_back(std::move(e));
        }
    }
    if (out.empty()) {
        out = {L".mp4", L".mov", L".m4v"};
    }
    return out;
}

/// True for the DJI D-Log M conversion LUT, which is not an HDR hint LUT.
bool isDlogLutName(std::wstring_view fileName) {
    return platform::icontains(fileName, L"dlog") || platform::icontains(fileName, L"d-log") ||
           platform::icontains(fileName, L"d_log");
}

/// Case-insensitive membership test for a path list.
bool containsPath(const std::vector<std::wstring>& list, std::wstring_view value) {
    return std::any_of(list.begin(), list.end(),
                       [value](const std::wstring& have) { return platform::iequals(have, value); });
}

/// True when the INI looks like the Python tool's file rather than ours.
bool looksLikeLegacyIni(const std::wstring& path) {
    IniFile ini;
    if (!ini.load(path)) {
        return false;
    }
    return ini.has(L"Paths", L"last_video_path") || ini.has(L"Paths", L"last_lut_path") ||
           ini.has(L"Preferences", L"output_prefix") || ini.has(L"HDR", L"transfer");
}

} // namespace

// ---------------------------------------------------------------------------
// Paths and defaults
// ---------------------------------------------------------------------------

/**
 * @brief %APPDATA%\HdrHint\settings.ini (local app data, then the exe folder,
 *        when the roaming folder cannot be resolved).
 */
std::wstring Settings::defaultPath() {
    std::wstring dir = platform::appRoamingDataFolder();
    if (dir.empty()) {
        HH_LOG_WARN(kLog, L"roaming app data folder unavailable; falling back to local app data");
        dir = platform::appLocalDataFolder();
    }
    if (dir.empty()) {
        HH_LOG_WARN(kLog, L"local app data folder unavailable; falling back to the exe folder");
        dir = platform::exeDirectory();
    }
    return path::join(dir, L"settings.ini");
}

/**
 * @brief Fills the machine-dependent defaults that are left empty. Paths
 *        keep the "<exe>" token so the file stays valid when the app moves.
 */
void Settings::applyMachineDefaults() {
    if (platform::trim(lutFolder).empty()) {
        lutFolder = std::wstring(kExeToken) + L"\\luts";
    }
    if (platform::trim(pqLutPath).empty()) {
        pqLutPath = path::join(lutFolder, kDefaultPqLutName);
    }
    if (platform::trim(userPresetsFile).empty()) {
        userPresetsFile = L"%APPDATA%\\HdrHint\\presets.json";
    }
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

/**
 * @brief Reads every known key; a missing file leaves the defaults in place.
 */
Result<void> Settings::load(const std::wstring& path) {
    IniFile ini;
    if (auto loaded = ini.load(path); !loaded) {
        HH_LOG_ERROR(kLog, L"load: {}", loaded.error().toString());
        return loaded.error();
    }

    // [app]
    readBool(ini, kApp, L"first_run", firstRun);
    readEnum(ini, kApp, L"theme", theme, {L"system", L"dark", L"light"});
    readEnum(ini, kApp, L"accent", accent, {L"blue", L"system"});
    readEnum(ini, kApp, L"dock_mode", dockMode, {L"auto", L"docked", L"floating"});
    readBool(ini, kApp, L"floating_always_on_top", floatingAlwaysOnTop);
    readBool(ini, kApp, L"minimize_to_tray", minimizeToTray);
    readBool(ini, kApp, L"start_minimized", startMinimized);
    readBool(ini, kApp, L"start_with_windows", startWithWindows);
    readBool(ini, kApp, L"quit_with_ame", quitWithAme);
    readBool(ini, kApp, L"reduce_transparency", reduceTransparency);
    readString(ini, kApp, L"floating_placement", floatingPlacement);
    readString(ini, kApp, L"dock_target", dockTarget);

    // [ame]
    readBool(ini, kAme, L"watch_log", watchLog);
    readList(ini, kAme, L"log_path_override", logPathOverrides);
    logPathOverrides = dedupePaths(logPathOverrides);
    readInt(ini, kAme, L"log_poll_ms", logPollMs, 250, 60000);
    readBool(ini, kAme, L"date_day_first", dateDayFirst);
    readInt(ini, kAme, L"catch_up_minutes", catchUpMinutes, 0, 24 * 60);
    readInt(ini, kAme, L"watch_recent_folders_count", watchRecentFoldersCount, 0, 100);
    readList(ini, kAme, L"extra_watch_folders", extraWatchFolders);
    extraWatchFolders = dedupePaths(extraWatchFolders);
    readList(ini, kAme, L"ignore_folders", ignoreFolders);
    ignoreFolders = dedupePaths(ignoreFolders);

    // [watch]
    readList(ini, kWatch, L"extensions", extensions);
    extensions = normaliseExtensions(extensions);
    readInt(ini, kWatch, L"debounce_ms", debounceMs, 50, 10000);
    readBool(ini, kWatch, L"sidecar_detection", sidecarDetection);

    // [detect]
    readInt(ini, kDetect, L"probe_interval_ms", probeIntervalMs, 250, 60000);
    readInt(ini, kDetect, L"stable_seconds", stableSeconds, 0, 600);
    readU64(ini, kDetect, L"min_output_bytes", minOutputBytes, 0, 1ull << 40);
    readBool(ini, kDetect, L"check_structure", checkStructure);
    readBool(ini, kDetect, L"require_log_confirmation", requireLogConfirmation);
    readInt(ini, kDetect, L"log_confirm_timeout_s", logConfirmTimeoutS, 0, 3600);
    readInt(ini, kDetect, L"encoding_timeout_hours", encodingTimeoutHours, 1, 24 * 7);
    readInt(ini, kDetect, L"missing_grace_s", missingGraceS, 0, 3600);

    // [output]
    {
        std::wstring candidate = suffix;
        readString(ini, kOutput, L"suffix", candidate);
        if (path::hasInvalidFileNameChars(candidate)) {
            HH_LOG_WARN(kLog, L"[output] suffix \"{}\" contains invalid characters; default kept", candidate);
        } else {
            suffix = candidate;
        }
    }
    readString(ini, kOutput, L"folder", outputFolder);
    readEnum(ini, kOutput, L"on_conflict", onConflict, {L"increment", L"overwrite", L"skip"});
    readBool(ini, kOutput, L"set_title", setTitle);

    // [mkvmerge]
    readString(ini, kMkvmerge, L"path", mkvmergePath);
    readBool(ini, kMkvmerge, L"lower_priority", lowerPriority);
    readBool(ini, kMkvmerge, L"fail_on_warnings", failOnWarnings);
    readBool(ini, kMkvmerge, L"keep_partial_on_failure", keepPartialOnFailure);
    readInt(ini, kMkvmerge, L"timeout_hours", timeoutHours, 1, 24 * 7);
    readInt(ini, kMkvmerge, L"min_major_version", minMajorVersion, 0, 999);

    // [lut]
    readString(ini, kLut, L"lut_folder", lutFolder);
    readString(ini, kLut, L"pq_path", pqLutPath);
    readString(ini, kLut, L"hlg_path", hlgLutPath);
    readBool(ini, kLut, L"attach", attachLut);
    readString(ini, kLut, L"mime", attachmentMime);
    if (attachmentMime.empty()) {
        attachmentMime = L"application/x-cube";
    }
    readList(ini, kLut, L"recent", recentLuts);
    recentLuts = dedupePaths(recentLuts);

    // [hdr]
    readString(ini, kHdr, L"preset_pq", presetPq);
    if (presetPq.empty()) {
        presetPq = L"generic_pq_1000";
    }
    readString(ini, kHdr, L"preset_hlg", presetHlg);
    if (presetHlg.empty()) {
        presetHlg = L"generic_hlg";
    }
    readEnum(ini, kHdr, L"inband_policy", inbandPolicy, {L"preset", L"hold"});
    readString(ini, kHdr, L"user_presets_file", userPresetsFile);

    // [jobs]
    readBool(ini, kJobs, L"auto_process", autoProcess);
    readEnum(ini, kJobs, L"sdr_policy", sdrPolicy, {L"skip", L"tag_sdr", L"hold"});
    readBool(ini, kJobs, L"hold_when_transfer_unknown", holdWhenTransferUnknown);
    readInt(ini, kJobs, L"history_max", historyMax, 10, 100000);
    readBool(ini, kJobs, L"toast_on_done", toastOnDone);
    readBool(ini, kJobs, L"reveal_on_done", revealOnDone);

    // [recycle]
    readEnum(ini, kRecycle, L"mode", recycleMode, {L"auto", L"ask", L"never"});

    // [ipc]
    readBool(ini, kIpc, L"enabled", ipcEnabled);
    {
        std::wstring candidate = pipeName;
        readString(ini, kIpc, L"pipe_name", candidate);
        if (candidate.empty() || path::hasInvalidFileNameChars(candidate)) {
            HH_LOG_WARN(kLog, L"[ipc] pipe_name \"{}\" is not usable; default kept", candidate);
        } else {
            pipeName = candidate;
        }
    }
    readInt(ini, kIpc, L"max_instances", maxInstances, 1, 64);
    readBool(ini, kIpc, L"write_registry", writeRegistry);

    // [log]
    readEnum(ini, kLogSec, L"level", logLevel, {L"trace", L"debug", L"info", L"warn", L"error"});
    readInt(ini, kLogSec, L"max_size_kb", logMaxSizeKb, 64, 1024 * 1024);
    readInt(ini, kLogSec, L"keep_files", logKeepFiles, 0, 50);
    readBool(ini, kLogSec, L"log_mkvmerge_output", logMkvmergeOutput);

    // [migration]
    readBool(ini, kMigration, L"done", migrationDone);
    readString(ini, kMigration, L"source", migrationSource);

    HH_LOG_INFO(kLog, L"loaded settings from {}", path);
    return {};
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

/**
 * @brief Writes every key. The existing file is read first so user comments
 *        and ordering survive; a few explanatory comments are added for the
 *        keys people are most likely to edit by hand.
 */
Result<void> Settings::save(const std::wstring& path) const {
    if (platform::trim(path).empty()) {
        return Error::text(L"Settings::save: empty path");
    }

    IniFile ini;
    if (auto loaded = ini.load(path); !loaded) {
        HH_LOG_WARN(kLog, L"save: could not re-read {} ({}); comments will be lost", path,
                    loaded.error().toString());
        ini.clear();
    }

    // [app]
    ini.setBool(kApp, L"first_run", firstRun);
    ini.set(kApp, L"theme", theme);
    ini.set(kApp, L"accent", accent);
    ini.set(kApp, L"dock_mode", dockMode);
    ini.setBool(kApp, L"floating_always_on_top", floatingAlwaysOnTop);
    ini.setBool(kApp, L"minimize_to_tray", minimizeToTray);
    ini.setBool(kApp, L"start_minimized", startMinimized);
    ini.setBool(kApp, L"start_with_windows", startWithWindows);
    ini.setBool(kApp, L"quit_with_ame", quitWithAme);
    ini.setBool(kApp, L"reduce_transparency", reduceTransparency);
    ini.set(kApp, L"floating_placement", floatingPlacement);
    ini.set(kApp, L"dock_target", dockTarget);

    // [ame]
    ini.setBool(kAme, L"watch_log", watchLog);
    ini.setList(kAme, L"log_path_override", logPathOverrides);
    ini.setInt(kAme, L"log_poll_ms", logPollMs);
    ini.setBool(kAme, L"date_day_first", dateDayFirst);
    ini.setInt(kAme, L"catch_up_minutes", catchUpMinutes);
    ini.setInt(kAme, L"watch_recent_folders_count", watchRecentFoldersCount);
    ini.setList(kAme, L"extra_watch_folders", extraWatchFolders);
    ini.setList(kAme, L"ignore_folders", ignoreFolders);

    // [watch]
    ini.setList(kWatch, L"extensions", extensions);
    ini.setInt(kWatch, L"debounce_ms", debounceMs);
    ini.setBool(kWatch, L"sidecar_detection", sidecarDetection);

    // [detect]
    ini.setInt(kDetect, L"probe_interval_ms", probeIntervalMs);
    ini.setInt(kDetect, L"stable_seconds", stableSeconds);
    ini.setInt(kDetect, L"min_output_bytes", static_cast<long long>(std::min<uint64_t>(minOutputBytes, 1ull << 62)));
    ini.setBool(kDetect, L"check_structure", checkStructure);
    ini.setBool(kDetect, L"require_log_confirmation", requireLogConfirmation);
    ini.setInt(kDetect, L"log_confirm_timeout_s", logConfirmTimeoutS);
    ini.setInt(kDetect, L"encoding_timeout_hours", encodingTimeoutHours);
    ini.setInt(kDetect, L"missing_grace_s", missingGraceS);

    // [output]
    ini.set(kOutput, L"suffix", suffix);
    ini.setComment(kOutput, L"suffix",
                   L"Appended to the source file's stem for the hint file, e.g. clip_REC709_HINT.mkv.\n"
                   L"A stem that already ends with the suffix is not doubled.");
    ini.set(kOutput, L"folder", outputFolder);
    ini.set(kOutput, L"on_conflict", onConflict);
    ini.setBool(kOutput, L"set_title", setTitle);

    // [mkvmerge]
    ini.set(kMkvmerge, L"path", mkvmergePath);
    ini.setBool(kMkvmerge, L"lower_priority", lowerPriority);
    ini.setBool(kMkvmerge, L"fail_on_warnings", failOnWarnings);
    ini.setBool(kMkvmerge, L"keep_partial_on_failure", keepPartialOnFailure);
    ini.setInt(kMkvmerge, L"timeout_hours", timeoutHours);
    ini.setInt(kMkvmerge, L"min_major_version", minMajorVersion);

    // [lut]
    ini.set(kLut, L"lut_folder", lutFolder);
    ini.set(kLut, L"pq_path", pqLutPath);
    ini.setComment(kLut, L"pq_path",
                   L"LUT attached to PQ / HDR10 exports. Paths may use %ENV% variables and <exe>\n"
                   L"(the folder HdrHint.exe lives in). Leave empty to attach nothing.");
    ini.set(kLut, L"hlg_path", hlgLutPath);
    ini.setComment(kLut, L"hlg_path", L"LUT attached to HLG exports. Empty by default: HLG plays fine without one.");
    ini.setBool(kLut, L"attach", attachLut);
    ini.set(kLut, L"mime", attachmentMime);
    ini.setList(kLut, L"recent", recentLuts);

    // [hdr]
    ini.set(kHdr, L"preset_pq", presetPq);
    ini.set(kHdr, L"preset_hlg", presetHlg);
    ini.set(kHdr, L"inband_policy", inbandPolicy);
    ini.set(kHdr, L"user_presets_file", userPresetsFile);

    // [jobs]
    ini.setBool(kJobs, L"auto_process", autoProcess);
    ini.set(kJobs, L"sdr_policy", sdrPolicy);
    ini.setBool(kJobs, L"hold_when_transfer_unknown", holdWhenTransferUnknown);
    ini.setInt(kJobs, L"history_max", historyMax);
    ini.setBool(kJobs, L"toast_on_done", toastOnDone);
    ini.setBool(kJobs, L"reveal_on_done", revealOnDone);

    // [recycle]
    ini.set(kRecycle, L"mode", recycleMode);
    ini.setComment(kRecycle, L"mode",
                   L"auto = move the AME original to the Recycle Bin once the hint file is verified,\n"
                   L"ask = prompt each time, never = always keep the original.");

    // [ipc]
    ini.setBool(kIpc, L"enabled", ipcEnabled);
    ini.set(kIpc, L"pipe_name", pipeName);
    ini.setInt(kIpc, L"max_instances", maxInstances);
    ini.setBool(kIpc, L"write_registry", writeRegistry);

    // [log]
    ini.set(kLogSec, L"level", logLevel);
    ini.setInt(kLogSec, L"max_size_kb", logMaxSizeKb);
    ini.setInt(kLogSec, L"keep_files", logKeepFiles);
    ini.setBool(kLogSec, L"log_mkvmerge_output", logMkvmergeOutput);

    // [migration]
    ini.setBool(kMigration, L"done", migrationDone);
    ini.set(kMigration, L"source", migrationSource);

    if (auto saved = ini.save(path); !saved) {
        HH_LOG_ERROR(kLog, L"save: {}", saved.error().toString());
        return saved.error();
    }
    HH_LOG_DEBUG(kLog, L"saved settings to {}", path);
    return {};
}

// ---------------------------------------------------------------------------
// Path expansion
// ---------------------------------------------------------------------------

/**
 * @brief "<exe>" -> the executable's folder, then %ENV% expansion. Nothing
 *        else is touched (no absolute-path conversion).
 */
std::wstring Settings::expand(std::wstring_view path) const {
    const std::wstring_view t = platform::trim(path);
    if (t.empty()) {
        return {};
    }
    std::wstring s(t);

    // "<exe>" token, any case, any number of times (bounded so a pathological
    // value can never spin here).
    const std::wstring exeDir = platform::exeDirectory();
    for (int guard = 0; guard < 16; ++guard) {
        const size_t pos = platform::ifind(s, kExeToken);
        if (pos == std::wstring::npos) {
            break;
        }
        s.replace(pos, 5, exeDir);
    }

    // %ENV% variables via the Win32 expander (size query first).
    if (s.find(L'%') != std::wstring::npos) {
        const DWORD needed = ::ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring buffer(static_cast<size_t>(needed), L'\0');
            const DWORD got = ::ExpandEnvironmentStringsW(s.c_str(), buffer.data(), needed);
            if (got > 0 && got <= needed) {
                buffer.resize(static_cast<size_t>(got) - 1);  // drop the terminator
                s = std::move(buffer);
            } else {
                HH_LOG_WARN(kLog, L"ExpandEnvironmentStringsW failed for \"{}\" (error {})", s, ::GetLastError());
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Legacy migration
// ---------------------------------------------------------------------------

/**
 * @brief Imports the useful bits of the Python tool's settings.ini:
 *        watch folder, LUT, suffix and the delete-original preference.
 *        migrationDone/migrationSource are set even when nothing changed.
 */
Result<bool> Settings::migrateFromLegacy(const std::wstring& legacyIniPath) {
    if (platform::trim(legacyIniPath).empty()) {
        return Error::text(L"migrateFromLegacy: empty path");
    }
    if (!platform::isFile(legacyIniPath)) {
        return Error::text(L"migrateFromLegacy: file not found: " + legacyIniPath);
    }

    IniFile legacy;
    if (auto loaded = legacy.load(legacyIniPath); !loaded) {
        return loaded.error();
    }

    bool changed = false;
    const std::wstring transfer = std::wstring(platform::trim(legacy.get(L"HDR", L"transfer")));

    // [Paths] last_video_path: a folder (or a file's folder) to keep watching.
    {
        const std::wstring raw(platform::trim(legacy.get(L"Paths", L"last_video_path")));
        if (!raw.empty()) {
            std::wstring folder = platform::fullPath(path::normalizeSeparators(raw));
            if (folder.empty()) {
                folder = path::normalizeSeparators(raw);
            }
            if (platform::isFile(folder)) {
                folder = path::parent(folder);
            }
            if (platform::isDirectory(folder)) {
                if (!containsPath(extraWatchFolders, folder)) {
                    extraWatchFolders.push_back(folder);
                    changed = true;
                    HH_LOG_INFO(kLog, L"migrated watch folder {}", folder);
                }
            } else {
                HH_LOG_INFO(kLog, L"legacy last_video_path {} no longer exists; skipped", raw);
            }
        }
    }

    // [Paths] last_lut_path: recent LUT plus the default for its transfer.
    {
        const std::wstring raw(platform::trim(legacy.get(L"Paths", L"last_lut_path")));
        if (!raw.empty()) {
            std::wstring lut = platform::fullPath(path::normalizeSeparators(raw));
            if (lut.empty()) {
                lut = path::normalizeSeparators(raw);
            }
            if (platform::isFile(lut)) {
                if (!containsPath(recentLuts, lut)) {
                    recentLuts.insert(recentLuts.begin(), lut);
                    changed = true;
                }
                if (transfer == L"18" && platform::trim(hlgLutPath).empty()) {
                    hlgLutPath = lut;
                    changed = true;
                    HH_LOG_INFO(kLog, L"migrated HLG LUT {}", lut);
                } else if (transfer == L"16" && !isDlogLutName(path::fileName(lut))) {
                    if (!platform::iequals(pqLutPath, lut)) {
                        pqLutPath = lut;
                        changed = true;
                        HH_LOG_INFO(kLog, L"migrated PQ LUT {}", lut);
                    }
                }
            } else {
                HH_LOG_INFO(kLog, L"legacy last_lut_path {} no longer exists; skipped", raw);
            }
        }
    }

    // [Preferences] output_prefix -> suffix, only when the user changed it.
    {
        const std::wstring prefix(platform::trim(legacy.get(L"Preferences", L"output_prefix")));
        if (!prefix.empty() && prefix != kLegacyDefaultPrefix && !path::hasInvalidFileNameChars(prefix)) {
            if (suffix != prefix) {
                suffix = prefix;
                changed = true;
                HH_LOG_INFO(kLog, L"migrated output suffix \"{}\"", prefix);
            }
        }
    }

    // [Preferences] delete_original -> recycle mode.
    if (legacy.has(L"Preferences", L"delete_original")) {
        const std::optional<bool> del = platform::parseBool(platform::trim(legacy.get(L"Preferences", L"delete_original")));
        if (del.has_value()) {
            const std::wstring mode = *del ? L"auto" : L"never";
            if (recycleMode != mode) {
                recycleMode = mode;
                changed = true;
                HH_LOG_INFO(kLog, L"migrated delete_original -> recycle mode {}", mode);
            }
        }
    }

    migrationDone = true;
    migrationSource = legacyIniPath;
    return changed;
}

/**
 * @brief <exe dir>\settings.ini (when it is the Python tool's file), then up
 *        to five parent levels holding both hdr_gui.py and settings.ini.
 */
std::wstring Settings::findLegacyIni() {
    const std::wstring exeDir = platform::exeDirectory();
    if (exeDir.empty()) {
        return {};
    }

    // Right next to the executable.
    const std::wstring beside = path::join(exeDir, L"settings.ini");
    if (platform::isFile(beside) && looksLikeLegacyIni(beside)) {
        return beside;
    }

    // Walk up: a build tree usually sits a few levels below the repo root.
    std::wstring dir = exeDir;
    for (int level = 0; level < 5; ++level) {
        const std::wstring up = path::parent(dir);
        if (up.empty() || platform::iequals(up, dir)) {
            break;
        }
        dir = up;
        const std::wstring ini = path::join(dir, L"settings.ini");
        if (platform::isFile(path::join(dir, L"hdr_gui.py")) && platform::isFile(ini)) {
            return ini;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Per-transfer defaults
// ---------------------------------------------------------------------------

/**
 * @brief Expanded LUT path for PQ / HLG; empty for SDR / Unknown or when unset.
 */
std::wstring Settings::defaultLutFor(int transferKind) const {
    switch (static_cast<TransferKind>(transferKind)) {
    case TransferKind::PQ:
        return platform::trim(pqLutPath).empty() ? std::wstring() : expand(pqLutPath);
    case TransferKind::HLG:
        return platform::trim(hlgLutPath).empty() ? std::wstring() : expand(hlgLutPath);
    case TransferKind::SDR:
    case TransferKind::Unknown:
        break;
    default:
        HH_LOG_WARN(kLog, L"defaultLutFor: unknown transfer kind {}", transferKind);
        break;
    }
    return {};
}

/**
 * @brief Preset id for PQ / HLG from the settings, "sdr_rec709" for SDR,
 *        empty for Unknown (such jobs are held for the user).
 */
std::wstring Settings::defaultPresetFor(int transferKind) const {
    switch (static_cast<TransferKind>(transferKind)) {
    case TransferKind::PQ:
        return platform::trim(presetPq).empty() ? std::wstring(L"generic_pq_1000") : presetPq;
    case TransferKind::HLG:
        return platform::trim(presetHlg).empty() ? std::wstring(L"generic_hlg") : presetHlg;
    case TransferKind::SDR:
        return L"sdr_rec709";
    case TransferKind::Unknown:
        break;
    default:
        HH_LOG_WARN(kLog, L"defaultPresetFor: unknown transfer kind {}", transferKind);
        break;
    }
    return {};
}

} // namespace hh
