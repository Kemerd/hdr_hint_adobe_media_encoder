// ---------------------------------------------------------------------------
// PanelInstaller.cpp - installs the CEP panel into the per-user CEP folder.
//
//   Windows  %APPDATA%\Adobe\CEP\extensions\com.everett.hdrhint
//   macOS    ~/Library/Application Support/Adobe/CEP/extensions/com.everett.hdrhint
//
// The panel ships with the app (<exe>\cep on Windows, HdrHint.app/Contents/
// Resources/cep on macOS) and is copied file-by-file into place. A
// config.json tells the panel where the app lives; PlayerDebugMode="1" on
// CSXS.9..14 lets CEP load an unsigned extension for the current and future
// runtimes (HKCU\Software\Adobe\CSXS.N on Windows, the com.adobe.CSXS.N
// preferences domain on macOS).
// ---------------------------------------------------------------------------
#include "ame/PanelInstaller.h"

#include "ame/AmeProcess.h"
#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/NamedPipe.h"
#include "platform/Utf.h"

#if defined(_WIN32)
#include "platform/Registry.h"
#else
#include <CoreFoundation/CoreFoundation.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace hh::ame {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"PanelInstaller";

/// Bundle id (also the folder name in both locations).
constexpr const wchar_t* kBundleId = L"com.everett.hdrhint";

/// Manifest location inside the bundle (folder, file).
constexpr const wchar_t* kManifestFolder = L"CSXS";
constexpr const wchar_t* kManifestFile = L"manifest.xml";

/// Attribute we read out of the manifest.
constexpr std::wstring_view kVersionAttribute = L"ExtensionBundleVersion=\"";

#if defined(_WIN32)
/// Our own registry key.
constexpr std::wstring_view kHdrHintKey = L"Software\\HdrHint";
#endif

/// CEP majors that get PlayerDebugMode (current runtime is 12; the extra
/// keys keep the install working across future host updates).
constexpr int kFirstCsxsMajor = 9;
constexpr int kLastCsxsMajor = 14;

/// Upper bound on manifest size we are willing to read.
constexpr uint64_t kMaxManifestBytes = 1024ull * 1024ull;

/// Recursion guard for the copy / delete walkers.
constexpr int kMaxTreeDepth = 32;

/// <dir>/CSXS/manifest.xml with this platform's separator.
std::wstring manifestIn(const std::wstring& bundleDir)
{
    return path::join(path::join(bundleDir, kManifestFolder), kManifestFile);
}

#if !defined(_WIN32)
/// CFString from wide text (caller releases).
CFStringRef cfString(std::wstring_view text)
{
    const std::string utf8 = platform::toUtf8(text);
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                   static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
}

/// Reads a string preference of another app's domain ("com.adobe.CSXS.12").
std::wstring readPreferenceString(std::wstring_view domain, std::wstring_view key)
{
    CFStringRef app = cfString(domain);
    CFStringRef name = cfString(key);
    std::wstring out;
    if (app != nullptr && name != nullptr) {
        CFPropertyListRef value = CFPreferencesCopyAppValue(name, app);
        if (value != nullptr) {
            if (CFGetTypeID(value) == CFStringGetTypeID()) {
                char buffer[64] = {};
                if (CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                    out = platform::toWide(buffer);
                }
            } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
                long long number = 0;
                CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number);
                out = std::to_wstring(number);
            }
            CFRelease(value);
        }
    }
    if (app != nullptr) { CFRelease(app); }
    if (name != nullptr) { CFRelease(name); }
    return out;
}

/// Writes a string preference into another app's domain and flushes it.
bool writePreferenceString(std::wstring_view domain, std::wstring_view key, std::wstring_view value)
{
    CFStringRef app = cfString(domain);
    CFStringRef name = cfString(key);
    CFStringRef text = cfString(value);
    bool ok = false;
    if (app != nullptr && name != nullptr && text != nullptr) {
        CFPreferencesSetAppValue(name, text, app);
        ok = CFPreferencesAppSynchronize(app);
    }
    if (app != nullptr) { CFRelease(app); }
    if (name != nullptr) { CFRelease(name); }
    if (text != nullptr) { CFRelease(text); }
    return ok;
}
#endif

/**
 * @brief Escapes a wide string for use inside a JSON string literal and
 *        returns it as UTF-8 (without surrounding quotes).
 */
std::string jsonEscapeUtf8(std::wstring_view text)
{
    std::wstring escaped;
    escaped.reserve(text.size() + 16);
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'\\': escaped += L"\\\\"; break;
        case L'"':  escaped += L"\\\""; break;
        case L'\n': escaped += L"\\n"; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\t': escaped += L"\\t"; break;
        default:
            // Other control characters become \u00XX; everything else is
            // passed through and UTF-8 encoded below.
            if (ch < 0x20) {
                escaped += std::format(L"\\u{:04x}", static_cast<unsigned>(ch));
            } else {
                escaped += ch;
            }
            break;
        }
    }
    return platform::toUtf8(escaped);
}

/**
 * @brief Recursively copies @p from into @p to (overwriting files).
 *
 * Reparse points (junctions/symlinks) are skipped so a stray link inside the
 * bundle can never send the walk somewhere else on disk.
 */
Result<void> copyTree(const std::wstring& from, const std::wstring& to, int depth)
{
    if (depth > kMaxTreeDepth) {
        return Error::text(std::format(L"copy aborted: directory nesting deeper than {} at {}", kMaxTreeDepth, from));
    }
    if (from.empty() || to.empty()) {
        return Error::text(L"copy aborted: empty path");
    }

    // Make sure the destination directory exists before touching files.
    if (auto made = platform::createDirectories(to); !made) {
        return made;
    }

    // Walk the source directory.
    auto listing = platform::listDirectory(from);
    if (!listing) {
        return listing.error();
    }
    for (const platform::DirEntry& entry : listing.value()) {
        if (entry.name.empty() || entry.name == L"." || entry.name == L"..") {
            continue;
        }
        if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            HH_LOG_WARN(kLog, L"skipping reparse point {}", path::join(from, entry.name));
            continue;
        }
        const std::wstring source = path::join(from, entry.name);
        const std::wstring target = path::join(to, entry.name);

        // Directories recurse; files are copied with overwrite.
        if (entry.isDirectory) {
            if (auto sub = copyTree(source, target, depth + 1); !sub) {
                return sub;
            }
            continue;
        }
        if (auto copied = platform::copyFile(source, target, true); !copied) {
            // A locked file while AME runs is the one failure the user can fix.
            if (copied.error().win32 == ERROR_SHARING_VIOLATION && isAmeRunning()) {
                return Error::text(std::format(L"{} is in use - close Adobe Media Encoder and try again", target));
            }
            return copied;
        }
    }
    return Result<void>::success();
}

/**
 * @brief Recursively deletes a directory tree (best effort, first error wins).
 */
Result<void> deleteTree(const std::wstring& dir, int depth)
{
    if (depth > kMaxTreeDepth) {
        return Error::text(std::format(L"delete aborted: directory nesting deeper than {} at {}", kMaxTreeDepth, dir));
    }
    if (dir.empty()) {
        return Error::text(L"delete aborted: empty path");
    }

    // Only descend into real directories; a reparse point is removed as-is
    // (RemoveDirectory deletes the link, never the target).
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesW(platform::toExtendedPath(dir).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Result<void>::success();   // already gone
    }
    const bool isLink = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    const std::string native = platform::toUtf8(dir);
    struct stat st {};
    if (::lstat(native.c_str(), &st) != 0) {
        return Result<void>::success();   // already gone
    }
    const bool isLink = S_ISLNK(st.st_mode);
#endif
    if (!isLink) {
        auto listing = platform::listDirectory(dir);
        if (!listing) {
            return listing.error();
        }
        for (const platform::DirEntry& entry : listing.value()) {
            if (entry.name.empty() || entry.name == L"." || entry.name == L"..") {
                continue;
            }
            const std::wstring child = path::join(dir, entry.name);
            if (entry.isDirectory) {
                if (auto sub = deleteTree(child, depth + 1); !sub) {
                    return sub;
                }
            } else {
                if (auto removed = platform::deleteFile(child); !removed) {
                    return removed;
                }
            }
        }
    }

#if defined(_WIN32)
    // Read-only directories refuse RemoveDirectory; clear the bit first.
    const std::wstring extended = platform::toExtendedPath(dir);
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        ::SetFileAttributesW(extended.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
    if (!::RemoveDirectoryW(extended.c_str())) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return Result<void>::success();
        }
        return Error::fromWin32(error, std::format(L"RemoveDirectory({})", dir));
    }
#else
    // A symlink is unlinked (never followed); a real folder is now empty.
    const int rc = isLink ? ::unlink(native.c_str()) : ::rmdir(native.c_str());
    if (rc != 0) {
        const int err = errno;
        if (err == ENOENT) {
            return Result<void>::success();
        }
        return Error::fromErrno(err, std::format(L"rmdir({})", dir));
    }
#endif
    return Result<void>::success();
}

/**
 * @brief Sets PlayerDebugMode="1" on one CSXS key unless it already is "1".
 */
Result<void> ensurePlayerDebugMode(int major)
{
#if defined(_WIN32)
    const std::wstring key = std::format(L"Software\\Adobe\\CSXS.{}", major);
    auto current = platform::regReadString(HKEY_CURRENT_USER, key, L"PlayerDebugMode");
    if (current && platform::trim(current.value()) == L"1") {
        return Result<void>::success();
    }
    auto written = platform::regWriteString(HKEY_CURRENT_USER, key, L"PlayerDebugMode", L"1");
    if (!written) {
        HH_LOG_WARN(kLog, L"could not set PlayerDebugMode for CSXS.{}: {}", major, written.error().toString());
        return written;
    }
#else
    // macOS keeps it in the com.adobe.CSXS.N preferences domain
    // (the same thing "defaults write com.adobe.CSXS.12 PlayerDebugMode 1" does).
    const std::wstring domain = std::format(L"com.adobe.CSXS.{}", major);
    if (platform::trim(readPreferenceString(domain, L"PlayerDebugMode")) == L"1") {
        return Result<void>::success();
    }
    if (!writePreferenceString(domain, L"PlayerDebugMode", L"1")) {
        HH_LOG_WARN(kLog, L"could not set PlayerDebugMode for {}", domain);
        return Error::text(std::format(L"could not write PlayerDebugMode to {}", domain));
    }
#endif
    HH_LOG_INFO(kLog, L"PlayerDebugMode=1 set for CSXS.{}", major);
    return Result<void>::success();
}

} // namespace

// ---------------------------------------------------------------------------
// Locations
// ---------------------------------------------------------------------------

/**
 * @brief Bundle id of the panel.
 */
const wchar_t* panelBundleId() noexcept
{
    return kBundleId;
}

/**
 * @brief <exeDir>\cep\com.everett.hdrhint (empty when the exe dir is unknown).
 */
std::wstring bundledPanelDirectory()
{
    // The shipped assets live next to the exe (Windows) or in the bundle's Resources (macOS).
    const std::wstring exeDir = platform::resourceDirectory();
    if (exeDir.empty()) {
        HH_LOG_WARN(kLog, L"exe directory unknown; bundled panel path unavailable");
        return {};
    }
    return path::join(path::join(exeDir, L"cep"), kBundleId);
}

/**
 * @brief %APPDATA%\Adobe\CEP\extensions\com.everett.hdrhint (empty when
 *        the roaming folder cannot be resolved).
 */
std::wstring installedPanelDirectory()
{
    const std::wstring roaming = platform::roamingAppDataFolder();
    if (roaming.empty()) {
        HH_LOG_WARN(kLog, L"roaming AppData folder unknown; install path unavailable");
        return {};
    }
    return path::join(path::join(path::join(path::join(roaming, L"Adobe"), L"CEP"), L"extensions"), kBundleId);
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

/**
 * @brief Extracts ExtensionBundleVersion="..." from a manifest.
 *
 * A plain substring search is enough: the attribute is unique in the file
 * and CEP manifests are hand-written XML without entity tricks.
 */
std::wstring readManifestVersion(const std::wstring& manifestPath)
{
    if (manifestPath.empty() || !platform::isFile(manifestPath)) {
        return {};
    }
    auto bytes = platform::readAll(manifestPath, kMaxManifestBytes);
    if (!bytes) {
        HH_LOG_DEBUG(kLog, L"cannot read manifest {}: {}", manifestPath, bytes.error().toString());
        return {};
    }
    const std::vector<uint8_t>& raw = bytes.value();
    if (raw.empty()) {
        return {};
    }

    // Manifests are UTF-8; a BOM decodes to U+FEFF and is harmless here.
    const std::wstring text = platform::toWide(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
    const size_t at = text.find(kVersionAttribute);
    if (at == std::wstring::npos) {
        return {};
    }
    const size_t begin = at + kVersionAttribute.size();
    const size_t end = text.find(L'"', begin);
    if (end == std::wstring::npos || end <= begin) {
        return {};
    }
    return std::wstring(platform::trim(std::wstring_view(text).substr(begin, end - begin)));
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/**
 * @brief Reports what is installed, what is bundled and whether CEP will
 *        load an unsigned panel for the current runtime.
 */
PanelStatus queryPanelStatus()
{
    PanelStatus status;
    status.installPath = installedPanelDirectory();

    // Installed = the manifest exists where CEP looks for it.
    if (!status.installPath.empty()) {
        const std::wstring manifest = manifestIn(status.installPath);
        status.installed = platform::isFile(manifest);
        if (status.installed) {
            status.installedVersion = readManifestVersion(manifest);
        }
    }

    // Bundled version next to the exe.
    const std::wstring bundled = bundledPanelDirectory();
    if (!bundled.empty()) {
        status.bundledVersion = readManifestVersion(manifestIn(bundled));
    }

    // CSXS.12 is the runtime AME 2026 ships; that key decides loading.
#if defined(_WIN32)
    auto debug = platform::regReadString(HKEY_CURRENT_USER, L"Software\\Adobe\\CSXS.12", L"PlayerDebugMode");
    status.debugModeOk = debug && platform::trim(debug.value()) == L"1";
#else
    status.debugModeOk = platform::trim(readPreferenceString(L"com.adobe.CSXS.12", L"PlayerDebugMode")) == L"1";
#endif

    HH_LOG_DEBUG(kLog, L"panel status: installed {} ({}) bundled {} debugMode {}", status.installed,
                 status.installedVersion, status.bundledVersion, status.debugModeOk);
    return status;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

/**
 * @brief Copies the bundled panel into place, writes config.json and sets
 *        the registry values CEP and the panel rely on.
 */
Result<void> installPanel()
{
    // Source and destination must both be resolvable.
    const std::wstring source = bundledPanelDirectory();
    if (source.empty() || !platform::isDirectory(source)) {
        return Error::text(std::format(L"bundled panel not found at {}", source.empty() ? L"<unknown>" : source));
    }
    const std::wstring destination = installedPanelDirectory();
    if (destination.empty()) {
        return Error::text(L"cannot resolve %APPDATA%\\Adobe\\CEP\\extensions");
    }
    const std::wstring bundledVersion = readManifestVersion(manifestIn(source));
    if (bundledVersion.empty()) {
        return Error::text(std::format(L"bundled manifest is missing or has no ExtensionBundleVersion: {}", source));
    }
    HH_LOG_INFO(kLog, L"installing panel {} from {} to {}", bundledVersion, source, destination);

    // 1. Copy the bundle over the installed one.
    if (auto copied = copyTree(source, destination, 0); !copied) {
        HH_LOG_ERROR(kLog, L"panel copy failed: {}", copied.error().toString());
        return copied;
    }

    // 2. config.json: where the exe lives and which pipe to connect to.
    // macOS: the .app bundle, which LaunchServices knows how to start.
    const std::wstring exe = platform::launchablePath();
    if (exe.empty()) {
        return Error::text(L"cannot determine the path of the running executable");
    }
    const std::string config = std::format("{{\"exePath\": \"{}\", \"installedVersion\": \"{}\", \"pipeName\": \"{}\"}}\n",
                                           jsonEscapeUtf8(exe), jsonEscapeUtf8(bundledVersion),
                                           jsonEscapeUtf8(platform::ipcEndpointName(L"HdrHint")));
    const std::wstring configPath = path::join(destination, L"config.json");
    if (auto written = platform::writeAllAtomic(configPath, config); !written) {
        HH_LOG_ERROR(kLog, L"config.json write failed: {}", written.error().toString());
        return written;
    }

    // 3. PlayerDebugMode for every CEP major we care about (never lowered).
    std::vector<std::wstring> failures;
    for (int major = kFirstCsxsMajor; major <= kLastCsxsMajor; ++major) {
        if (auto set = ensurePlayerDebugMode(major); !set) {
            failures.push_back(std::format(L"CSXS.{}", major));
        }
    }

#if defined(_WIN32)
    // 4. Our own key: the panel and the uninstaller read these.
    if (auto set = platform::regWriteString(HKEY_CURRENT_USER, kHdrHintKey, L"ExePath", exe); !set) {
        HH_LOG_ERROR(kLog, L"registry ExePath write failed: {}", set.error().toString());
        return set;
    }
    if (auto set = platform::regWriteString(HKEY_CURRENT_USER, kHdrHintKey, L"Version", bundledVersion); !set) {
        HH_LOG_ERROR(kLog, L"registry Version write failed: {}", set.error().toString());
        return set;
    }
#endif

    // A PlayerDebugMode failure means AME will not load the panel; say so.
    if (!failures.empty()) {
        return Error::text(std::format(L"panel files installed, but PlayerDebugMode could not be set for {}",
                                       platform::join(failures, L", ")));
    }
    HH_LOG_INFO(kLog, L"panel {} installed", bundledVersion);
    return Result<void>::success();
}

/**
 * @brief Removes the installed panel folder and our registry key. CSXS
 *        PlayerDebugMode values are left alone (other panels may need them).
 */
Result<void> uninstallPanel()
{
    const std::wstring destination = installedPanelDirectory();
    if (destination.empty()) {
        return Error::text(L"cannot resolve %APPDATA%\\Adobe\\CEP\\extensions");
    }

    // Files first so a registry failure never leaves a half-removed panel.
    if (platform::exists(destination)) {
        HH_LOG_INFO(kLog, L"removing panel from {}", destination);
        if (auto removed = deleteTree(destination, 0); !removed) {
            HH_LOG_ERROR(kLog, L"panel removal failed: {}", removed.error().toString());
            if (isAmeRunning()) {
                return Error::text(std::format(L"{} - close Adobe Media Encoder and try again", removed.error().toString()));
            }
            return removed;
        }
    }

#if defined(_WIN32)
    // Then our key (absent is fine).
    if (platform::regKeyExists(HKEY_CURRENT_USER, kHdrHintKey)) {
        if (auto deleted = platform::regDeleteKey(HKEY_CURRENT_USER, kHdrHintKey); !deleted) {
            HH_LOG_ERROR(kLog, L"registry key removal failed: {}", deleted.error().toString());
            return deleted;
        }
    }
#endif
    HH_LOG_INFO(kLog, L"panel uninstalled");
    return Result<void>::success();
}

} // namespace hh::ame
