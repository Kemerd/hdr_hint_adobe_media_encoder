// ---------------------------------------------------------------------------
// KnownFolders.cpp - well-known directories, always without a trailing slash.
//
// SHGetKnownFolderPath is the authority (it honours folder redirection and
// per-user policies); the environment variables are only a fallback for the
// rare broken profile where the shell lookup fails.
// ---------------------------------------------------------------------------
#include "platform/KnownFolders.h"

#include "core/Logger.h"
#include "platform/Handle.h"

#include <shlobj.h>

#include <string>
#include <string_view>

namespace hh::platform {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"KnownFolders";

/// Largest path GetModuleFileNameW / the environment can hand back.
constexpr size_t kMaxPathChars = 32768;

/**
 * @brief Removes trailing separators but never reduces a drive root
 *        ("C:\") to a drive-relative "C:".
 */
std::wstring stripTrailingSeparators(std::wstring path)
{
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        // "X:\" must keep its backslash to stay an absolute root.
        if (path.size() == 3 && path[1] == L':') {
            break;
        }
        path.pop_back();
    }
    return path;
}

/**
 * @brief Reads an environment variable (empty when unset or on failure).
 */
std::wstring environmentVariable(const wchar_t* name)
{
    if (name == nullptr || *name == L'\0') {
        return {};
    }

    // First call measures (including the terminator), second call copies.
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0 || needed > kMaxPathChars) {
        return {};
    }
    std::wstring value(static_cast<size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(static_cast<size_t>(written));
    return stripTrailingSeparators(std::move(value));
}

/**
 * @brief Resolves a known folder through the shell, falling back to up to
 *        two environment variables (checked in order) when the shell fails.
 *
 * @param id        FOLDERID_* constant.
 * @param label     Name used in the log line on failure.
 * @param envFirst  Primary fallback variable (may be nullptr).
 * @param envSecond Secondary fallback variable (may be nullptr).
 */
std::wstring knownFolder(const KNOWNFOLDERID& id, const wchar_t* label,
                         const wchar_t* envFirst, const wchar_t* envSecond)
{
    // The shell allocates the string with CoTaskMemAlloc; CoTaskMemPtr frees it.
    PWSTR raw = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    CoTaskMemPtr<wchar_t> owner(raw);
    if (SUCCEEDED(hr) && raw != nullptr && *raw != L'\0') {
        return stripTrailingSeparators(std::wstring(raw));
    }

    // Shell lookup failed: log once per call and try the environment.
    HH_LOG_WARN(kLog, L"SHGetKnownFolderPath({}) failed: {}", label ? label : L"?", Error::fromHr(hr, L"").toString());
    if (envFirst != nullptr) {
        std::wstring value = environmentVariable(envFirst);
        if (!value.empty()) {
            return value;
        }
    }
    if (envSecond != nullptr) {
        std::wstring value = environmentVariable(envSecond);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

/**
 * @brief Ensures "<base>\HdrHint" exists and returns it (empty when the base
 *        folder itself could not be resolved).
 */
std::wstring appSubfolder(std::wstring base, const wchar_t* label)
{
    if (base.empty()) {
        HH_LOG_ERROR(kLog, L"cannot resolve the {} base folder", label ? label : L"?");
        return {};
    }
    std::wstring folder = std::move(base);
    folder.append(L"\\HdrHint");

    // CreateDirectory is idempotent for our purposes: already-exists is fine.
    if (!::CreateDirectoryW(folder.c_str(), nullptr)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            HH_LOG_WARN(kLog, L"CreateDirectory({}) failed: {}", folder, Error::fromWin32(err, L"").toString());
        }
    }
    return folder;
}

} // namespace

// ---------------------------------------------------------------------------
// Standard folders
// ---------------------------------------------------------------------------

/**
 * @brief The user's Documents folder (redirection-aware).
 */
std::wstring documentsFolder()
{
    std::wstring folder = knownFolder(FOLDERID_Documents, L"Documents", nullptr, nullptr);
    if (!folder.empty()) {
        return folder;
    }
    // Last resort: the conventional location under the profile.
    std::wstring profile = environmentVariable(L"USERPROFILE");
    if (profile.empty()) {
        return {};
    }
    return profile + L"\\Documents";
}

/**
 * @brief %LOCALAPPDATA%.
 */
std::wstring localAppDataFolder()
{
    return knownFolder(FOLDERID_LocalAppData, L"LocalAppData", L"LOCALAPPDATA", nullptr);
}

/**
 * @brief %APPDATA%.
 */
std::wstring roamingAppDataFolder()
{
    return knownFolder(FOLDERID_RoamingAppData, L"RoamingAppData", L"APPDATA", nullptr);
}

/**
 * @brief C:\Program Files (the 64-bit one, regardless of process bitness).
 */
std::wstring programFilesX64Folder()
{
    return knownFolder(FOLDERID_ProgramFilesX64, L"ProgramFilesX64", L"ProgramW6432", L"ProgramFiles");
}

/**
 * @brief C:\Program Files (x86).
 */
std::wstring programFilesX86Folder()
{
    return knownFolder(FOLDERID_ProgramFilesX86, L"ProgramFilesX86", L"ProgramFiles(x86)", nullptr);
}

/**
 * @brief %TEMP% as resolved by GetTempPathW (falls back to TEMP / TMP).
 */
std::wstring tempFolder()
{
    // GetTempPath returns the length without the terminator on success.
    std::wstring buffer(kMaxPathChars, L'\0');
    const DWORD written = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written > 0 && written < buffer.size()) {
        buffer.resize(static_cast<size_t>(written));
        return stripTrailingSeparators(std::move(buffer));
    }

    // Unusual, but the environment is the next best source.
    HH_LOG_WARN(kLog, L"GetTempPathW failed: {}", Error::fromLastError(L"").toString());
    std::wstring value = environmentVariable(L"TEMP");
    if (value.empty()) {
        value = environmentVariable(L"TMP");
    }
    return value;
}

// ---------------------------------------------------------------------------
// Executable location
// ---------------------------------------------------------------------------

/**
 * @brief Full path of the running executable.
 *
 * GetModuleFileNameW truncates silently when the buffer is too small (the
 * return value equals the buffer size), so the buffer grows until the
 * result fits or the hard 32K limit is reached.
 */
std::wstring exePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            HH_LOG_ERROR(kLog, L"GetModuleFileNameW failed: {}", Error::fromLastError(L"").toString());
            return {};
        }
        // Fits: the return value is the length without the terminator.
        if (static_cast<size_t>(written) < buffer.size()) {
            buffer.resize(static_cast<size_t>(written));
            return buffer;
        }
        // Truncated: grow and retry, up to the maximum path length.
        if (buffer.size() >= kMaxPathChars) {
            buffer.resize(static_cast<size_t>(written));
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

/**
 * @brief Directory containing the running executable (no trailing slash,
 *        except for a bare drive root which stays "X:\").
 */
std::wstring exeDirectory()
{
    const std::wstring path = exePath();
    if (path.empty()) {
        return {};
    }

    // Cut at the last separator of either kind.
    const size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos) {
        return {};
    }
    std::wstring dir = path.substr(0, cut);

    // "C:" alone would be drive-relative; keep the root backslash.
    if (dir.size() == 2 && dir[1] == L':') {
        dir.push_back(L'\\');
    }
    return dir;
}

// ---------------------------------------------------------------------------
// Application data folders
// ---------------------------------------------------------------------------

/**
 * @brief %LOCALAPPDATA%\HdrHint, created on demand.
 */
std::wstring appLocalDataFolder()
{
    return appSubfolder(localAppDataFolder(), L"LocalAppData");
}

/**
 * @brief %APPDATA%\HdrHint, created on demand.
 */
std::wstring appRoamingDataFolder()
{
    return appSubfolder(roamingAppDataFolder(), L"RoamingAppData");
}

/**
 * @brief %LOCALAPPDATA%\HdrHint\logs, created on demand.
 */
std::wstring appLogsFolder()
{
    const std::wstring base = appLocalDataFolder();
    if (base.empty()) {
        return {};
    }
    std::wstring folder = base + L"\\logs";
    if (!::CreateDirectoryW(folder.c_str(), nullptr)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            HH_LOG_WARN(kLog, L"CreateDirectory({}) failed: {}", folder, Error::fromWin32(err, L"").toString());
        }
    }
    return folder;
}

/**
 * @brief Windows ships its assets next to the exe.
 */
std::wstring resourceDirectory()
{
    return exeDirectory();
}

/**
 * @brief Windows relaunches the exe itself.
 */
std::wstring launchablePath()
{
    return exePath();
}

} // namespace hh::platform
