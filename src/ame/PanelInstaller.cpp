// ---------------------------------------------------------------------------
// PanelInstaller.cpp - installs the CEP panel into %APPDATA%\Adobe\CEP\extensions.
//
// The panel ships next to the exe (<exe>\cep\com.everett.hdrhint) and is
// copied file-by-file into the per-user CEP extensions folder. A config.json
// tells the panel where the exe lives; PlayerDebugMode="1" on CSXS.9..14
// lets CEP load an unsigned extension for the current and future runtimes.
// ---------------------------------------------------------------------------
#include "ame/PanelInstaller.h"

#include "ame/AmeProcess.h"
#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Registry.h"
#include "platform/Utf.h"

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

/// Manifest location inside the bundle.
constexpr std::wstring_view kManifestRelative = L"\\CSXS\\manifest.xml";

/// Attribute we read out of the manifest.
constexpr std::wstring_view kVersionAttribute = L"ExtensionBundleVersion=\"";

/// Named pipe the panel connects to (as the panel expects it).
constexpr std::wstring_view kPipeName = L"\\\\.\\pipe\\HdrHint";

/// Our own registry key.
constexpr std::wstring_view kHdrHintKey = L"Software\\HdrHint";

/// CEP majors that get PlayerDebugMode (current runtime is 12; the extra
/// keys keep the install working across future host updates).
constexpr int kFirstCsxsMajor = 9;
constexpr int kLastCsxsMajor = 14;

/// Upper bound on manifest size we are willing to read.
constexpr uint64_t kMaxManifestBytes = 1024ull * 1024ull;

/// Recursion guard for the copy / delete walkers.
constexpr int kMaxTreeDepth = 32;

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
            HH_LOG_WARN(kLog, L"skipping reparse point {}\\{}", from, entry.name);
            continue;
        }
        const std::wstring source = from + L"\\" + entry.name;
        const std::wstring target = to + L"\\" + entry.name;

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
    const DWORD attributes = ::GetFileAttributesW(platform::toExtendedPath(dir).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Result<void>::success();   // already gone
    }
    const bool isLink = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (!isLink) {
        auto listing = platform::listDirectory(dir);
        if (!listing) {
            return listing.error();
        }
        for (const platform::DirEntry& entry : listing.value()) {
            if (entry.name.empty() || entry.name == L"." || entry.name == L"..") {
                continue;
            }
            const std::wstring child = dir + L"\\" + entry.name;
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
    return Result<void>::success();
}

/**
 * @brief Sets PlayerDebugMode="1" on one CSXS key unless it already is "1".
 */
Result<void> ensurePlayerDebugMode(int major)
{
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
    const std::wstring exeDir = platform::exeDirectory();
    if (exeDir.empty()) {
        HH_LOG_WARN(kLog, L"exe directory unknown; bundled panel path unavailable");
        return {};
    }
    return exeDir + L"\\cep\\" + kBundleId;
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
    return roaming + L"\\Adobe\\CEP\\extensions\\" + kBundleId;
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
        const std::wstring manifest = status.installPath + std::wstring(kManifestRelative);
        status.installed = platform::isFile(manifest);
        if (status.installed) {
            status.installedVersion = readManifestVersion(manifest);
        }
    }

    // Bundled version next to the exe.
    const std::wstring bundled = bundledPanelDirectory();
    if (!bundled.empty()) {
        status.bundledVersion = readManifestVersion(bundled + std::wstring(kManifestRelative));
    }

    // CSXS.12 is the runtime AME 2026 ships; that key decides loading.
    auto debug = platform::regReadString(HKEY_CURRENT_USER, L"Software\\Adobe\\CSXS.12", L"PlayerDebugMode");
    status.debugModeOk = debug && platform::trim(debug.value()) == L"1";

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
    const std::wstring bundledVersion = readManifestVersion(source + std::wstring(kManifestRelative));
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
    const std::wstring exe = platform::exePath();
    if (exe.empty()) {
        return Error::text(L"cannot determine the path of the running executable");
    }
    const std::string config = std::format("{{\"exePath\": \"{}\", \"installedVersion\": \"{}\", \"pipeName\": \"{}\"}}\n",
                                           jsonEscapeUtf8(exe), jsonEscapeUtf8(bundledVersion), jsonEscapeUtf8(kPipeName));
    const std::wstring configPath = destination + L"\\config.json";
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

    // 4. Our own key: the panel and the uninstaller read these.
    if (auto set = platform::regWriteString(HKEY_CURRENT_USER, kHdrHintKey, L"ExePath", exe); !set) {
        HH_LOG_ERROR(kLog, L"registry ExePath write failed: {}", set.error().toString());
        return set;
    }
    if (auto set = platform::regWriteString(HKEY_CURRENT_USER, kHdrHintKey, L"Version", bundledVersion); !set) {
        HH_LOG_ERROR(kLog, L"registry Version write failed: {}", set.error().toString());
        return set;
    }

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

    // Then our key (absent is fine).
    if (platform::regKeyExists(HKEY_CURRENT_USER, kHdrHintKey)) {
        if (auto deleted = platform::regDeleteKey(HKEY_CURRENT_USER, kHdrHintKey); !deleted) {
            HH_LOG_ERROR(kLog, L"registry key removal failed: {}", deleted.error().toString());
            return deleted;
        }
    }
    HH_LOG_INFO(kLog, L"panel uninstalled");
    return Result<void>::success();
}

} // namespace hh::ame
