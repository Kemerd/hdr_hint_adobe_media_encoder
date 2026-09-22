// ---------------------------------------------------------------------------
// cli_main.cpp - hdrhint_cli: headless front-end for scripts and tests.
//
//   hdrhint_cli --process <file> [--preset <id>] [--lut <path>|none] [--suffix <s>]
//   hdrhint_cli --identify <file>
//   hdrhint_cli --presets
//   hdrhint_cli --mkvmerge
//   hdrhint_cli --version
//
// Output is built with std::format and written through one helper, so the
// same text comes out on the Windows console (UTF-16 text mode) and on a
// POSIX terminal or pipe (UTF-8), with no printf-format differences between
// the two C runtimes ("%s" means a wide string on MSVC, a narrow one on POSIX).
// ---------------------------------------------------------------------------
#include "core/Engine.h"
#include "core/HdrPresets.h"
#include "core/Logger.h"
#include "core/MkvmergeLocator.h"
#include "core/MkvmergeRunner.h"
#include "core/Settings.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"
#include "platform/Win.h"

#if defined(_WIN32)
#include "platform/Handle.h"

#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace {

/**
 * @brief Writes wide text to stdout / stderr in the platform's encoding.
 *
 * Windows: the streams are in _O_U8TEXT mode, so fputws does the UTF-8 work.
 * POSIX: the text is converted to UTF-8 and written as bytes; the streams
 * are never switched to wide orientation.
 */
void emit(std::FILE* stream, const std::wstring& text) {
    if (stream == nullptr || text.empty()) {
        return;
    }
#if defined(_WIN32)
    std::fputws(text.c_str(), stream);
#else
    const std::string utf8 = hh::platform::toUtf8(text);
    std::fwrite(utf8.data(), 1, utf8.size(), stream);
#endif
}

/// Prints usage to stdout.
void printUsage() {
    emit(stdout, L"HDR Hint command line\n\n"
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

/**
 * @brief The whole command-line tool once argv is wide text.
 */
int run(const std::vector<std::wstring>& args) {
    if (args.empty() || hasFlag(args, L"--help") || hasFlag(args, L"-h")) { printUsage(); return 0; }

    if (hasFlag(args, L"--version")) {
        emit(stdout, L"hdrhint_cli 1.0.0\n");
        return 0;
    }

    // Logging goes to the same folder as the app so problems are diagnosable.
#if defined(_WIN32)
    hh::platform::ScopedCoInit com;
#endif
    hh::Logger::instance().open(hh::platform::appLogsFolder(), hh::LogLevel::Info, 2048, 5);

    // Settings + presets exactly as the app loads them.
    hh::Settings settings;
    settings.applyMachineDefaults();
    const std::wstring settingsPath = hh::Settings::defaultPath();
    if (auto r = settings.load(settingsPath); !r) {
        emit(stderr, std::format(L"warning: {}\n", r.error().toString()));
    }
    settings.applyMachineDefaults();
    hh::PresetRegistry presets;
    presets.loadUser(settings.expand(settings.userPresetsFile));

    if (hasFlag(args, L"--presets")) {
        for (const auto& p : presets.all()) {
            emit(stdout, std::format(L"{:<22} {}\n", p.id, p.label));
        }
        return 0;
    }

    if (hasFlag(args, L"--mkvmerge")) {
        const hh::MkvmergeInfo info = hh::locateMkvmerge(settings.expand(settings.mkvmergePath), settings.minMajorVersion);
        emit(stdout, std::format(L"path: {}\nversion: {}\nok: {}\n{}\n", info.path, info.versionLine,
                                 info.ok ? L"yes" : L"no", info.error));
        return info.ok ? 0 : 2;
    }

    if (const std::wstring file = argValue(args, L"--identify"); !file.empty()) {
        const hh::MkvmergeInfo info = hh::locateMkvmerge(settings.expand(settings.mkvmergePath), settings.minMajorVersion);
        if (!info.ok) { emit(stderr, std::format(L"error: {}\n", info.error)); return 2; }
        auto ident = hh::MkvmergeRunner::identify(info.path, file);
        if (!ident) { emit(stderr, std::format(L"error: {}\n", ident.error().toString())); return 3; }
        const auto& id = ident.value();
        emit(stdout, std::format(L"container: {}\nvideo track id: {} ({}, {})\naudio tracks: {}\n", id.containerType,
                                 id.videoTrackId, id.videoCodec, id.pixelDimensions, id.audioTrackCount));
        if (id.hasColour) {
            emit(stdout, std::format(L"colour: matrix {} range {} transfer {} primaries {}\n", id.matrix, id.range,
                                     id.transfer, id.primaries));
        }
        if (id.hasMastering) {
            emit(stdout, std::format(L"mastering: maxCLL {} maxFALL {} maxLum {:.4f} minLum {:.4f} chroma {} white {}\n",
                                     id.maxCll, id.maxFall, id.maxLuminance, id.minLuminance, id.chromaticity,
                                     id.whitePoint));
        }
        for (const auto& a : id.attachments) {
            emit(stdout, std::format(L"attachment: {} ({}, {} bytes)\n", a.fileName, a.mimeType,
                                     static_cast<unsigned long long>(a.size)));
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
                emit(stdout, std::format(L"\rmuxing {:3}%", percent));
                std::fflush(stdout);
            }
        });
        emit(stdout, L"\n");
        if (!result) {
            emit(stderr, std::format(L"error: {}\n", result.error().toString()));
            return 4;
        }
        emit(stdout, std::format(L"created: {}\n", result.value()));
        return 0;
    }

    printUsage();
    return 1;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    // UTF-8 text mode: non-ASCII paths print correctly on the console and when
    // the output is captured by a script (PowerShell reads UTF-8 pipes).
    (void)_setmode(_fileno(stdout), _O_U8TEXT);
    (void)_setmode(_fileno(stderr), _O_U8TEXT);

    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) { if (argv[i]) { args.emplace_back(argv[i]); } }
    return run(args);
}
#else
int main(int argc, char** argv) {
    // A closed pipe (e.g. "| head") must end the tool quietly, not kill it mid-write.
    std::signal(SIGPIPE, SIG_IGN);

    // argv is UTF-8 on macOS; file names are normalised like every other path we read.
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        if (argv[i]) { args.emplace_back(hh::platform::normalizeNfc(hh::platform::toWide(argv[i]))); }
    }
    return run(args);
}
#endif
