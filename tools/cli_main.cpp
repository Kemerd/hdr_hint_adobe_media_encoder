// ---------------------------------------------------------------------------
// cli_main.cpp - hdrhint_cli.exe: headless front-end for scripts and tests.
//
//   hdrhint_cli --process <file> [--preset <id>] [--lut <path>|none] [--suffix <s>]
//   hdrhint_cli --identify <file>
//   hdrhint_cli --presets
//   hdrhint_cli --mkvmerge
//   hdrhint_cli --version
// ---------------------------------------------------------------------------
#include "core/Engine.h"
#include "core/HdrPresets.h"
#include "core/Logger.h"
#include "core/MkvmergeLocator.h"
#include "core/MkvmergeRunner.h"
#include "core/Settings.h"
#include "platform/Handle.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"
#include "platform/Win.h"

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

namespace {

/// Prints usage to stdout.
void printUsage() {
    std::wprintf(L"HDR Hint command line\n\n"
                 L"  hdrhint_cli --process <file> [--preset <id>] [--lut <path>|none] [--suffix <s>]\n"
                 L"  hdrhint_cli --identify <file>\n"
                 L"  hdrhint_cli --presets\n"
                 L"  hdrhint_cli --mkvmerge\n"
                 L"  hdrhint_cli --version\n\n"
                 L"--process runs mkvmerge on an exported file exactly like the app does when AME finishes:\n"
                 L"it writes the HDR colour metadata, attaches the LUT and produces <stem><suffix>.mkv.\n");
}

/// Returns the value following @p flag in argv, or empty.
std::wstring argValue(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (hh::platform::iequals(args[i], flag)) { return args[i + 1]; }
    }
    return {};
}

/// True when @p flag is present.
bool hasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) { if (hh::platform::iequals(a, flag)) { return true; } }
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // UTF-8 text mode: non-ASCII paths print correctly on the console and when
    // the output is captured by a script (PowerShell reads UTF-8 pipes).
    (void)_setmode(_fileno(stdout), _O_U8TEXT);
    (void)_setmode(_fileno(stderr), _O_U8TEXT);

    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) { if (argv[i]) { args.emplace_back(argv[i]); } }
    if (args.empty() || hasFlag(args, L"--help") || hasFlag(args, L"-h")) { printUsage(); return 0; }

    if (hasFlag(args, L"--version")) {
        std::wprintf(L"hdrhint_cli 1.0.0\n");
        return 0;
    }

    // Logging goes to the same folder as the app so problems are diagnosable.
    hh::platform::ScopedCoInit com;
    hh::Logger::instance().open(hh::platform::appLocalDataFolder() + L"\\logs", hh::LogLevel::Info, 2048, 5);

    // Settings + presets exactly as the app loads them.
    hh::Settings settings;
    settings.applyMachineDefaults();
    const std::wstring settingsPath = hh::Settings::defaultPath();
    if (auto r = settings.load(settingsPath); !r) {
        std::fwprintf(stderr, L"warning: %s\n", r.error().toString().c_str());
    }
    settings.applyMachineDefaults();
    hh::PresetRegistry presets;
    presets.loadUser(settings.expand(settings.userPresetsFile));

    if (hasFlag(args, L"--presets")) {
        for (const auto& p : presets.all()) {
            std::wprintf(L"%-22s %s\n", p.id.c_str(), p.label.c_str());
        }
        return 0;
    }

    if (hasFlag(args, L"--mkvmerge")) {
        const hh::MkvmergeInfo info = hh::locateMkvmerge(settings.expand(settings.mkvmergePath), settings.minMajorVersion);
        std::wprintf(L"path: %s\nversion: %s\nok: %s\n%s\n", info.path.c_str(), info.versionLine.c_str(),
                     info.ok ? L"yes" : L"no", info.error.c_str());
        return info.ok ? 0 : 2;
    }

    if (const std::wstring file = argValue(args, L"--identify"); !file.empty()) {
        const hh::MkvmergeInfo info = hh::locateMkvmerge(settings.expand(settings.mkvmergePath), settings.minMajorVersion);
        if (!info.ok) { std::fwprintf(stderr, L"error: %s\n", info.error.c_str()); return 2; }
        auto ident = hh::MkvmergeRunner::identify(info.path, file);
        if (!ident) { std::fwprintf(stderr, L"error: %s\n", ident.error().toString().c_str()); return 3; }
        const auto& id = ident.value();
        std::wprintf(L"container: %s\nvideo track id: %d (%s, %s)\naudio tracks: %d\n", id.containerType.c_str(),
                     id.videoTrackId, id.videoCodec.c_str(), id.pixelDimensions.c_str(), id.audioTrackCount);
        if (id.hasColour) {
            std::wprintf(L"colour: matrix %d range %d transfer %d primaries %d\n", id.matrix, id.range, id.transfer, id.primaries);
        }
        if (id.hasMastering) {
            std::wprintf(L"mastering: maxCLL %d maxFALL %d maxLum %.4f minLum %.4f chroma %s white %s\n", id.maxCll, id.maxFall,
                         id.maxLuminance, id.minLuminance, id.chromaticity.c_str(), id.whitePoint.c_str());
        }
        for (const auto& a : id.attachments) {
            std::wprintf(L"attachment: %s (%s, %llu bytes)\n", a.fileName.c_str(), a.mimeType.c_str(),
                         static_cast<unsigned long long>(a.size));
        }
        return 0;
    }

    if (const std::wstring file = argValue(args, L"--process"); !file.empty()) {
        // Optional overrides for the one-shot run.
        const std::wstring presetId = argValue(args, L"--preset");
        const std::wstring lut = argValue(args, L"--lut");
        const std::wstring suffix = argValue(args, L"--suffix");
        if (!suffix.empty()) { settings.suffix = suffix; }

        hh::Engine engine(settings, presets);
        int lastPercent = -1;
        auto result = engine.processOneShot(file, presetId, lut, [&](float progress) {
            // Console progress on one line.
            const int percent = static_cast<int>(progress * 100.0f + 0.5f);
            if (percent != lastPercent) {
                lastPercent = percent;
                std::wprintf(L"\rmuxing %3d%%", percent);
                std::fflush(stdout);
            }
        });
        std::wprintf(L"\n");
        if (!result) {
            std::fwprintf(stderr, L"error: %s\n", result.error().toString().c_str());
            return 4;
        }
        std::wprintf(L"created: %s\n", result.value().c_str());
        return 0;
    }

    printUsage();
    return 1;
}
