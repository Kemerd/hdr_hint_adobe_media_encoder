// ---------------------------------------------------------------------------
// Settings.h - typed application settings (%APPDATA%\HdrHint\settings.ini).
//
// Every field has a default that works on a fresh machine. Paths may contain
// %ENV% variables and "<exe>" (the executable's directory); use expand().
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hh {

struct Settings {
    // [app]
    bool firstRun = true;
    std::wstring theme = L"system";            ///< system | dark | light
    std::wstring accent = L"blue";             ///< blue | system
    std::wstring dockMode = L"auto";           ///< auto | docked | floating
    bool floatingAlwaysOnTop = true;
    bool minimizeToTray = true;
    bool startMinimized = false;
    bool startWithWindows = false;             ///< HKCU Run entry; off by default - Media Encoder's startup script launches the app instead
    bool quitWithAme = true;                   ///< when launched from the panel
    bool showOnAmeLaunch = true;               ///< show the window when Media Encoder starts us
    bool reduceTransparency = false;
    std::wstring floatingPlacement;            ///< "x,y,w,h,dpi,max" (px) or empty
    std::wstring dockTarget;                   ///< picked AME panel, "l,t,w,h" relative to AME's client area

    // [ame]
    bool watchLog = true;
    std::vector<std::wstring> logPathOverrides;
    int logPollMs = 2000;
    bool dateDayFirst = false;                 ///< parse ambiguous log dates as D/M/Y
    int catchUpMinutes = 15;
    int watchRecentFoldersCount = 10;
    std::vector<std::wstring> extraWatchFolders;
    std::vector<std::wstring> ignoreFolders;

    // [watch]
    std::vector<std::wstring> extensions{L".mp4", L".mov", L".m4v"};
    int debounceMs = 500;
    bool sidecarDetection = true;

    // [detect]
    int probeIntervalMs = 1500;
    int stableSeconds = 2;
    uint64_t minOutputBytes = 1024;
    bool checkStructure = true;
    bool requireLogConfirmation = true;
    int logConfirmTimeoutS = 20;
    int encodingTimeoutHours = 12;
    int missingGraceS = 30;

    // [output]
    std::wstring suffix = L"_REC709_HINT";
    std::wstring outputFolder;                 ///< empty = next to the source
    std::wstring onConflict = L"increment";    ///< increment | overwrite | skip
    bool setTitle = false;

    // [mkvmerge]
    std::wstring mkvmergePath;
    bool lowerPriority = true;
    bool failOnWarnings = false;
    bool keepPartialOnFailure = false;
    int timeoutHours = 6;
    int minMajorVersion = 15;

    // [lut]
    std::wstring lutFolder;                    ///< default: <exe>\luts
    std::wstring pqLutPath;                    ///< default: <lutFolder>\PQ1000_to_Rec709_SDR_g24_YouTubeHint.cube
    std::wstring hlgLutPath;                   ///< default: empty (no LUT for HLG)
    bool attachLut = true;
    std::wstring attachmentMime = L"application/x-cube";
    std::vector<std::wstring> recentLuts;

    // [hdr]
    std::wstring presetPq = L"generic_pq_1000";
    std::wstring presetHlg = L"generic_hlg";
    std::wstring inbandPolicy = L"preset";     ///< preset | hold
    std::wstring userPresetsFile;              ///< default: %APPDATA%\HdrHint\presets.json

    // [jobs]
    bool autoProcess = true;
    /// Auto-process exports Media Encoder told us about (its log, or the panel).
    bool autoProcessAme = true;
    /// Auto-process files that simply appeared in a watch folder.
    bool autoProcessWatched = true;
    std::wstring sdrPolicy = L"skip";          ///< skip | tag_sdr | hold
    bool holdWhenTransferUnknown = true;
    int historyMax = 500;
    bool toastOnDone = true;
    bool revealOnDone = false;

    // [recycle]
    std::wstring recycleMode = L"auto";        ///< auto | ask | never

    // [ipc]
    bool ipcEnabled = true;
    std::wstring pipeName = L"HdrHint";
    int maxInstances = 4;
    bool writeRegistry = true;

    // [log]
    std::wstring logLevel = L"info";
    int logMaxSizeKb = 2048;
    int logKeepFiles = 5;
    bool logMkvmergeOutput = true;

    // [migration]
    bool migrationDone = false;
    std::wstring migrationSource;

    // ---- behaviour ---------------------------------------------------------

    /// %APPDATA%\HdrHint\settings.ini
    static std::wstring defaultPath();

    /// Fills machine-dependent defaults (LUT folder, presets file) when empty.
    void applyMachineDefaults();

    /// Loads from an INI (missing file = defaults). Unknown keys are ignored.
    Result<void> load(const std::wstring& path);
    /// Saves to an INI, preserving comments of an existing file.
    Result<void> save(const std::wstring& path) const;

    /// Expands %ENV% and "<exe>" in a path.
    [[nodiscard]] std::wstring expand(std::wstring_view path) const;

    /**
     * @brief One-time import from the Python tool's settings.ini.
     * @return true when something was migrated (migrationDone is set either way).
     */
    Result<bool> migrateFromLegacy(const std::wstring& legacyIniPath);

    /// Searches the usual places for the legacy settings.ini (exe dir and parents).
    static std::wstring findLegacyIni();

    /// Effective default LUT for a transfer kind (expanded path or empty).
    [[nodiscard]] std::wstring defaultLutFor(int transferKind) const;
    /// Effective default preset id for a transfer kind.
    [[nodiscard]] std::wstring defaultPresetFor(int transferKind) const;
};

} // namespace hh
