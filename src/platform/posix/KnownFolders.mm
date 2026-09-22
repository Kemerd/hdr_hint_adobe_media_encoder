// ---------------------------------------------------------------------------
// posix/KnownFolders.mm - well-known directories on macOS.
//
// Everything comes from NSFileManager so redirected folders (iCloud
// "Desktop & Documents", managed accounts) resolve exactly as the Finder
// shows them. The app's own folders follow Apple's layout:
//
//   ~/Library/Application Support/HdrHint   settings, jobs, tail state
//   ~/Library/Logs/HdrHint                  rolling log (Console.app finds it)
// ---------------------------------------------------------------------------
#include "platform/KnownFolders.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#import <Foundation/Foundation.h>

#include <mach-o/dyld.h>
#include <sys/param.h>

#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"KnownFolders";

/// NSString -> wide string (nil -> empty).
std::wstring fromNsString(NSString* s) {
    if (s == nil) {
        return {};
    }
    const char* utf8 = [s fileSystemRepresentation];
    return utf8 ? posix::fromNative(utf8) : std::wstring();
}

/// Drops trailing slashes but never turns "/" into "".
std::wstring stripTrailingSlashes(std::wstring path) {
    while (path.size() > 1 && path.back() == L'/') {
        path.pop_back();
    }
    return path;
}

/// First URL NSFileManager reports for a user-domain directory.
std::wstring userDirectory(NSSearchPathDirectory which) {
    @autoreleasepool {
        NSArray<NSURL*>* urls = [[NSFileManager defaultManager] URLsForDirectory:which inDomains:NSUserDomainMask];
        if (urls.count == 0) {
            return {};
        }
        return stripTrailingSlashes(fromNsString(urls.firstObject.path));
    }
}

/// $HOME/<leaf> fallback when Foundation has nothing to say.
std::wstring homeRelative(const wchar_t* leaf) {
    @autoreleasepool {
        std::wstring home = fromNsString(NSHomeDirectory());
        if (home.empty()) {
            return {};
        }
        return stripTrailingSlashes(home) + L"/" + leaf;
    }
}

/// base/HdrHint, created on demand (mkdir -p semantics).
std::wstring appSubfolder(const std::wstring& base, const wchar_t* label) {
    if (base.empty()) {
        HH_LOG_ERROR(kLog, L"cannot resolve the {} base folder", label ? label : L"?");
        return {};
    }
    std::wstring folder = base + L"/HdrHint";
    if (auto made = createDirectories(folder); !made) {
        HH_LOG_WARN(kLog, L"mkdir {} failed: {}", folder, made.error().toString());
    }
    return folder;
}

/**
 * @brief The .app bundle that contains this binary, or empty.
 *
 * The binary sits at X.app/Contents/MacOS/<name>; anything else (the build
 * tree, the CLI tool, /usr/local/bin) is not inside a bundle.
 */
std::wstring enclosingBundle() {
    const std::wstring exe = exePath();
    const std::wstring marker = L".app/Contents/MacOS/";
    const size_t at = exe.rfind(marker);
    if (at == std::wstring::npos) {
        return {};
    }
    // The part after MacOS/ must be the binary itself, not a nested folder.
    if (exe.find(L'/', at + marker.size()) != std::wstring::npos) {
        return {};
    }
    return exe.substr(0, at + 4);   // up to and including ".app"
}

} // namespace

// ---------------------------------------------------------------------------
// Well-known folders
// ---------------------------------------------------------------------------

std::wstring documentsFolder() {
    std::wstring dir = userDirectory(NSDocumentDirectory);
    return dir.empty() ? homeRelative(L"Documents") : dir;
}

std::wstring localAppDataFolder() {
    std::wstring dir = userDirectory(NSApplicationSupportDirectory);
    return dir.empty() ? homeRelative(L"Library/Application Support") : dir;
}

std::wstring roamingAppDataFolder() {
    // macOS has one per-user application-support folder; settings and state share it.
    return localAppDataFolder();
}

std::wstring programFilesX64Folder() {
    return L"/Applications";
}

std::wstring programFilesX86Folder() {
    // The per-user Applications folder is the closest thing to a second install root.
    return homeRelative(L"Applications");
}

std::wstring tempFolder() {
    @autoreleasepool {
        std::wstring dir = stripTrailingSlashes(fromNsString(NSTemporaryDirectory()));
        if (!dir.empty()) {
            return dir;
        }
    }
    if (const char* tmp = ::getenv("TMPDIR"); tmp != nullptr && *tmp != '\0') {
        return stripTrailingSlashes(posix::fromNative(tmp));
    }
    return L"/tmp";
}

// ---------------------------------------------------------------------------
// This executable
// ---------------------------------------------------------------------------

std::wstring exePath() {
    // _NSGetExecutablePath may hand back a relative or symlinked path; resolve it once.
    static const std::wstring s_path = [] {
        uint32_t size = 0;
        ::_NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(static_cast<size_t>(size) + 1, '\0');
        if (size == 0 || ::_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return std::wstring();
        }
        char resolved[PATH_MAX] = {};
        if (::realpath(buffer.data(), resolved) != nullptr) {
            return posix::fromNative(resolved);
        }
        return posix::fromNative(buffer.data());
    }();
    return s_path;
}

std::wstring exeDirectory() {
    const std::wstring exe = exePath();
    const size_t slash = exe.find_last_of(L'/');
    if (slash == std::wstring::npos) {
        return {};
    }
    return slash == 0 ? std::wstring(L"/") : exe.substr(0, slash);
}

std::wstring resourceDirectory() {
    const std::wstring bundle = enclosingBundle();
    if (!bundle.empty()) {
        const std::wstring resources = bundle + L"/Contents/Resources";
        if (isDirectory(resources)) {
            return resources;
        }
    }
    return exeDirectory();
}

std::wstring launchablePath() {
    const std::wstring bundle = enclosingBundle();
    return bundle.empty() ? exePath() : bundle;
}

// ---------------------------------------------------------------------------
// Application data folders
// ---------------------------------------------------------------------------

std::wstring appLocalDataFolder() {
    return appSubfolder(localAppDataFolder(), L"Application Support");
}

std::wstring appRoamingDataFolder() {
    return appSubfolder(roamingAppDataFolder(), L"Application Support");
}

std::wstring appLogsFolder() {
    std::wstring logs = userDirectory(NSLibraryDirectory);
    logs = logs.empty() ? homeRelative(L"Library/Logs") : logs + L"/Logs";
    return appSubfolder(logs, L"Logs");
}

} // namespace hh::platform
