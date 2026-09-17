// ---------------------------------------------------------------------------
// test_settings.cpp - Settings: defaults, INI round trip, path expansion and
// the one-time import of the Python tool's settings.ini.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/JobModel.h"
#include "core/Settings.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"
#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <vector>

using hh::Settings;
using hh::TransferKind;

namespace {

/// The Python tool's settings.ini with a given output_prefix and delete flag.
std::string legacyIni(const char* outputPrefix, const char* deleteOriginal) {
    std::string text;
    text += "[Paths]\n";
    text += "last_video_path = B:/YouTube Renders/EverettEngineers\n";
    text += "last_lut_path = A:/YouTube/EverettEngineers/Rec2100_PQ_to_Rec709_SDR_G24_YouTubeHint.cube\n";
    text += "\n";
    text += "[Preferences]\n";
    text += "show_info = True\n";
    text += std::string("delete_original = ") + deleteOriginal + "\n";
    text += "save_video_path = True\n";
    text += "save_lut_path = True\n";
    text += std::string("output_prefix = ") + outputPrefix + "\n";
    text += "\n";
    text += "[HDR]\n";
    text += "tag_hdr = True\n";
    text += "colour_matrix = 9\n";
    text += "colour_range = 1\n";
    text += "transfer = 16\n";
    text += "primaries = 9\n";
    text += "max_cll = 1000\n";
    text += "max_fall = 400\n";
    text += "chromaticity = 0.708,0.292,0.170,0.797,0.131,0.046\n";
    text += "white_point = 0.3127,0.329\n";
    text += "max_luminance = 1000\n";
    text += "min_luminance = 0.0001\n";
    return text;
}

/// ExpandEnvironmentStringsW as an oracle for Settings::expand.
std::wstring expandEnv(const std::wstring& text) {
    wchar_t buffer[4096] = {};
    const DWORD n = ::ExpandEnvironmentStringsW(text.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n > std::size(buffer)) { return {}; }
    return std::wstring(buffer);
}

} // namespace

// ---- defaults --------------------------------------------------------------------------

HH_TEST(Settings_defaultsWorkOnAFreshMachine) {
    const Settings s;
    CHECK(s.firstRun);
    CHECK_WEQ(s.theme, L"system");
    CHECK_WEQ(s.accent, L"blue");
    CHECK_WEQ(s.dockMode, L"auto");
    CHECK(s.watchLog);
    CHECK_EQ(s.logPollMs, 2000);
    CHECK_FALSE(s.dateDayFirst);
    CHECK_EQ(s.catchUpMinutes, 15);
    if (CHECK_EQ(s.extensions.size(), 3)) {
        CHECK_WEQ(s.extensions[0], L".mp4");
        CHECK_WEQ(s.extensions[1], L".mov");
        CHECK_WEQ(s.extensions[2], L".m4v");
    }
    CHECK_EQ(s.debounceMs, 500);
    CHECK_EQ(s.stableSeconds, 2);
    CHECK_EQ(s.minOutputBytes, 1024ull);
    CHECK(s.requireLogConfirmation);
    CHECK_WEQ(s.suffix, L"_REC709_HINT");
    CHECK(s.outputFolder.empty());
    CHECK_WEQ(s.onConflict, L"increment");
    CHECK(s.mkvmergePath.empty());
    CHECK(s.lowerPriority);
    CHECK_EQ(s.minMajorVersion, 15);
    CHECK(s.attachLut);
    CHECK_WEQ(s.attachmentMime, L"application/x-cube");
    CHECK_WEQ(s.presetPq, L"generic_pq_1000");
    CHECK_WEQ(s.presetHlg, L"generic_hlg");
    CHECK_WEQ(s.inbandPolicy, L"preset");
    CHECK(s.autoProcess);
    CHECK_WEQ(s.sdrPolicy, L"skip");
    CHECK(s.holdWhenTransferUnknown);
    CHECK_EQ(s.historyMax, 500);
    CHECK_WEQ(s.recycleMode, L"auto");
    CHECK(s.ipcEnabled);
    CHECK_WEQ(s.pipeName, L"HdrHint");
    CHECK_WEQ(s.logLevel, L"info");
    CHECK_EQ(s.logMaxSizeKb, 2048);
    CHECK_FALSE(s.migrationDone);
    CHECK(s.migrationSource.empty());
}

HH_TEST(Settings_defaultPathIsUnderRoamingAppData) {
    const std::wstring p = Settings::defaultPath();
    CHECK_FALSE(p.empty());
    CHECK(hh::platform::iendsWith(p, L"\\HdrHint\\settings.ini"));
    const std::wstring roaming = hh::platform::roamingAppDataFolder();
    if (!roaming.empty()) { CHECK(hh::platform::istartsWith(p, roaming)); }
}

HH_TEST(Settings_applyMachineDefaultsFillsOnlyEmptyFields) {
    Settings s;
    s.applyMachineDefaults();

    // LUT folder and the PQ LUT are derived from the exe location.
    CHECK_FALSE(s.lutFolder.empty());
    CHECK_FALSE(s.pqLutPath.empty());
    CHECK(hh::platform::iendsWith(s.pqLutPath, L"PQ1000_to_Rec709_SDR_g24_YouTubeHint.cube"));
    // HLG stays LUT-less by default; the presets file lands in %APPDATA%.
    CHECK(s.hlgLutPath.empty());
    CHECK_FALSE(s.userPresetsFile.empty());
    CHECK(hh::platform::iendsWith(s.userPresetsFile, L"presets.json"));

    // Already-set values are left alone.
    Settings custom;
    custom.lutFolder = L"D:\\my luts";
    custom.pqLutPath = L"D:\\my luts\\pq.cube";
    custom.userPresetsFile = L"D:\\p.json";
    custom.applyMachineDefaults();
    CHECK_WEQ(custom.lutFolder, L"D:\\my luts");
    CHECK_WEQ(custom.pqLutPath, L"D:\\my luts\\pq.cube");
    CHECK_WEQ(custom.userPresetsFile, L"D:\\p.json");
}

// ---- load / save ----------------------------------------------------------------------

HH_TEST(Settings_saveLoadRoundTrip) {
    hh::test::ScratchDir dir(L"settings");
    if (!CHECK(dir.valid())) { return; }
    const std::wstring file = dir.file(L"settings.ini");

    // Touch a field in (almost) every section.
    Settings s;
    s.firstRun = false;
    s.theme = L"dark";
    s.dockMode = L"floating";
    s.floatingPlacement = L"10,20,800,600,144,0";
    s.watchLog = false;
    s.logPathOverrides = {L"C:\\logs\\AMEEncodingLog.txt"};
    s.logPollMs = 1234;
    s.dateDayFirst = true;
    s.extraWatchFolders = {L"C:\\a", L"D:\\b c"};
    s.ignoreFolders = {L"C:\\a\\ignored"};
    s.extensions = {L".mp4", L".mkv"};
    s.debounceMs = 750;
    s.probeIntervalMs = 999;
    s.minOutputBytes = 4096;
    s.encodingTimeoutHours = 3;
    s.suffix = L"_X";
    s.outputFolder = L"D:\\hints";
    s.onConflict = L"skip";
    s.setTitle = true;
    s.mkvmergePath = L"C:\\Program Files\\MKVToolNix\\mkvmerge.exe";
    s.failOnWarnings = true;
    s.timeoutHours = 2;
    s.lutFolder = L"D:\\luts";
    s.pqLutPath = L"D:\\luts\\pq.cube";
    s.hlgLutPath = L"D:\\luts\\hlg.cube";
    s.attachLut = false;
    s.recentLuts = {L"D:\\luts\\pq.cube", L"D:\\luts\\old.cube"};
    s.presetPq = L"my_pq_user";
    s.presetHlg = L"my_hlg_user";
    s.inbandPolicy = L"hold";
    s.autoProcess = false;
    s.sdrPolicy = L"tag_sdr";
    s.holdWhenTransferUnknown = false;
    s.historyMax = 42;
    s.recycleMode = L"ask";
    s.ipcEnabled = false;
    s.pipeName = L"HdrHintTest";
    s.maxInstances = 2;
    s.logLevel = L"debug";
    s.logKeepFiles = 9;
    s.migrationDone = true;
    s.migrationSource = L"L:\\legacy\\settings.ini";

    const auto saved = s.save(file);
    if (!CHECK(saved.ok())) { return; }
    CHECK(dir.exists(L"settings.ini"));

    Settings back;
    const auto loaded = back.load(file);
    if (!CHECK(loaded.ok())) { return; }

    CHECK_FALSE(back.firstRun);
    CHECK_WEQ(back.theme, L"dark");
    CHECK_WEQ(back.dockMode, L"floating");
    CHECK_WEQ(back.floatingPlacement, L"10,20,800,600,144,0");
    CHECK_FALSE(back.watchLog);
    if (CHECK_EQ(back.logPathOverrides.size(), 1)) { CHECK_WEQ(back.logPathOverrides[0], L"C:\\logs\\AMEEncodingLog.txt"); }
    CHECK_EQ(back.logPollMs, 1234);
    CHECK(back.dateDayFirst);
    if (CHECK_EQ(back.extraWatchFolders.size(), 2)) {
        CHECK_WEQ(back.extraWatchFolders[0], L"C:\\a");
        CHECK_WEQ(back.extraWatchFolders[1], L"D:\\b c");
    }
    if (CHECK_EQ(back.ignoreFolders.size(), 1)) { CHECK_WEQ(back.ignoreFolders[0], L"C:\\a\\ignored"); }
    if (CHECK_EQ(back.extensions.size(), 2)) {
        CHECK_WEQ(back.extensions[0], L".mp4");
        CHECK_WEQ(back.extensions[1], L".mkv");
    }
    CHECK_EQ(back.debounceMs, 750);
    CHECK_EQ(back.probeIntervalMs, 999);
    CHECK_EQ(back.minOutputBytes, 4096ull);
    CHECK_EQ(back.encodingTimeoutHours, 3);
    CHECK_WEQ(back.suffix, L"_X");
    CHECK_WEQ(back.outputFolder, L"D:\\hints");
    CHECK_WEQ(back.onConflict, L"skip");
    CHECK(back.setTitle);
    CHECK_WEQ(back.mkvmergePath, L"C:\\Program Files\\MKVToolNix\\mkvmerge.exe");
    CHECK(back.failOnWarnings);
    CHECK_EQ(back.timeoutHours, 2);
    CHECK_WEQ(back.lutFolder, L"D:\\luts");
    CHECK_WEQ(back.pqLutPath, L"D:\\luts\\pq.cube");
    CHECK_WEQ(back.hlgLutPath, L"D:\\luts\\hlg.cube");
    CHECK_FALSE(back.attachLut);
    if (CHECK_EQ(back.recentLuts.size(), 2)) { CHECK_WEQ(back.recentLuts[1], L"D:\\luts\\old.cube"); }
    CHECK_WEQ(back.presetPq, L"my_pq_user");
    CHECK_WEQ(back.presetHlg, L"my_hlg_user");
    CHECK_WEQ(back.inbandPolicy, L"hold");
    CHECK_FALSE(back.autoProcess);
    CHECK_WEQ(back.sdrPolicy, L"tag_sdr");
    CHECK_FALSE(back.holdWhenTransferUnknown);
    CHECK_EQ(back.historyMax, 42);
    CHECK_WEQ(back.recycleMode, L"ask");
    CHECK_FALSE(back.ipcEnabled);
    CHECK_WEQ(back.pipeName, L"HdrHintTest");
    CHECK_EQ(back.maxInstances, 2);
    CHECK_WEQ(back.logLevel, L"debug");
    CHECK_EQ(back.logKeepFiles, 9);
    CHECK(back.migrationDone);
    CHECK_WEQ(back.migrationSource, L"L:\\legacy\\settings.ini");
}

HH_TEST(Settings_loadMissingFileKeepsDefaults) {
    hh::test::ScratchDir dir(L"settings_missing");
    if (!CHECK(dir.valid())) { return; }

    Settings s;
    const auto r = s.load(dir.file(L"nope.ini"));
    CHECK(r.ok());
    CHECK(s.firstRun);
    CHECK_WEQ(s.suffix, L"_REC709_HINT");
    CHECK_EQ(s.extensions.size(), 3);
}

HH_TEST(Settings_loadIgnoresUnknownKeysAndKeepsOtherDefaults) {
    hh::test::ScratchDir dir(L"settings_partial");
    if (!CHECK(dir.valid())) { return; }
    CHECK(dir.writeFile(L"p.ini",
                        "[app]\n"
                        "bogus_key = 1\n"
                        "theme = light\n"
                        "\n"
                        "[nonsense]\n"
                        "x = y\n"));

    Settings s;
    const auto r = s.load(dir.file(L"p.ini"));
    CHECK(r.ok());
    CHECK_WEQ(s.theme, L"light");
    // Everything not mentioned stays at its default.
    CHECK_WEQ(s.accent, L"blue");
    CHECK_EQ(s.logPollMs, 2000);
    CHECK_WEQ(s.suffix, L"_REC709_HINT");
}

HH_TEST(Settings_savePreservesCommentsOfExistingFile) {
    hh::test::ScratchDir dir(L"settings_comments");
    if (!CHECK(dir.valid())) { return; }
    CHECK(dir.writeFile(L"c.ini",
                        "; hand-written note at the top\n"
                        "[app]\n"
                        "; keep me\n"
                        "theme = dark\n"));

    Settings s;
    CHECK(s.load(dir.file(L"c.ini")).ok());
    s.theme = L"light";
    CHECK(s.save(dir.file(L"c.ini")).ok());

    const std::string bytes = dir.readFile(L"c.ini");
    CHECK(bytes.find("hand-written note at the top") != std::string::npos);
    CHECK(bytes.find("; keep me") != std::string::npos);
    CHECK(bytes.find("theme = light") != std::string::npos);
}

// ---- expand ---------------------------------------------------------------------------------

HH_TEST(Settings_expandExeAndEnvironment) {
    const Settings s;
    const std::wstring exeDir = hh::platform::exeDirectory();
    CHECK_FALSE(exeDir.empty());

    // "<exe>" is the executable's directory.
    CHECK_WEQ(s.expand(L"<exe>\\luts"), exeDir + L"\\luts");
    CHECK_WEQ(s.expand(L"<exe>"), exeDir);

    // %ENV% goes through the usual Windows expansion.
    CHECK_WEQ(s.expand(L"%TEMP%\\x"), expandEnv(L"%TEMP%\\x"));

    // Both in one path, plain paths untouched, empty stays empty.
    CHECK_WEQ(s.expand(L"%TEMP%\\<exe>"), expandEnv(L"%TEMP%\\") + exeDir);
    CHECK_WEQ(s.expand(L"C:\\plain\\file.cube"), L"C:\\plain\\file.cube");
    CHECK(s.expand(L"").empty());
}

HH_TEST(Settings_defaultLutAndPresetForTransfer) {
    Settings s;
    s.pqLutPath = L"<exe>\\luts\\pq.cube";
    s.hlgLutPath.clear();
    s.presetPq = L"pq_custom";
    s.presetHlg = L"hlg_custom";

    const std::wstring exeDir = hh::platform::exeDirectory();
    CHECK_WEQ(s.defaultLutFor(static_cast<int>(TransferKind::PQ)), exeDir + L"\\luts\\pq.cube");
    CHECK(s.defaultLutFor(static_cast<int>(TransferKind::HLG)).empty());
    CHECK(s.defaultLutFor(static_cast<int>(TransferKind::Unknown)).empty());

    CHECK_WEQ(s.defaultPresetFor(static_cast<int>(TransferKind::PQ)), L"pq_custom");
    CHECK_WEQ(s.defaultPresetFor(static_cast<int>(TransferKind::HLG)), L"hlg_custom");
    // Garbage transfer kinds must not crash; whatever comes back is a string.
    (void)s.defaultLutFor(-1);
    (void)s.defaultLutFor(9999);
    (void)s.defaultPresetFor(-1);
    (void)s.defaultPresetFor(9999);
}

// ---- legacy migration -------------------------------------------------------------------

HH_TEST(Settings_migrateFromLegacyDefaultPrefixIsNotMigrated) {
    hh::test::ScratchDir dir(L"legacy_default");
    if (!CHECK(dir.valid())) { return; }
    CHECK(dir.writeFile(L"settings.ini", legacyIni("_with_sdr_lut", "True")));

    Settings s;
    s.recycleMode = L"never";   // so we can see delete_original = True win
    const auto r = s.migrateFromLegacy(dir.file(L"settings.ini"));
    if (!CHECK(r.ok())) { return; }

    // The Python default prefix is not carried over: our suffix stays.
    CHECK_WEQ(s.suffix, L"_REC709_HINT");
    // delete_original = True -> recycle automatically.
    CHECK_WEQ(s.recycleMode, L"auto");
    // migrationDone is set either way.
    CHECK(s.migrationDone);
}

HH_TEST(Settings_migrateFromLegacyCustomPrefixIsMigrated) {
    hh::test::ScratchDir dir(L"legacy_custom");
    if (!CHECK(dir.valid())) { return; }
    CHECK(dir.writeFile(L"settings.ini", legacyIni("_custom", "True")));

    Settings s;
    const auto r = s.migrateFromLegacy(dir.file(L"settings.ini"));
    if (!CHECK(r.ok())) { return; }

    CHECK(r.value());
    CHECK_WEQ(s.suffix, L"_custom");
    CHECK_WEQ(s.recycleMode, L"auto");
    CHECK(s.migrationDone);
}

HH_TEST(Settings_migrateFromLegacyPythonBooleanSpelling) {
    // "True" with a capital T is what configparser writes; make sure the
    // spelling does not matter and a default prefix plus True still lands
    // on "auto" when the setting started elsewhere.
    hh::test::ScratchDir dir(L"legacy_bool");
    if (!CHECK(dir.valid())) { return; }
    CHECK(dir.writeFile(L"settings.ini", legacyIni("_with_sdr_lut", "true")));

    Settings s;
    s.recycleMode = L"ask";
    const auto r = s.migrateFromLegacy(dir.file(L"settings.ini"));
    if (!CHECK(r.ok())) { return; }
    CHECK_WEQ(s.recycleMode, L"auto");
    CHECK_WEQ(s.suffix, L"_REC709_HINT");
    CHECK(s.migrationDone);
}
