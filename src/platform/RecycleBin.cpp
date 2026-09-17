// ---------------------------------------------------------------------------
// RecycleBin.cpp - moving files to the Recycle Bin and revealing them.
//
// The one rule: never delete permanently. recycleFile() therefore refuses
// up front in every situation where the shell would silently nuke the file
// instead of recycling it:
//
//   * the file is already gone                     -> KeptNotFound
//   * network volume (no bin)                       -> KeptRemote
//   * volume without a bin / NukeOnDelete / policy  -> KeptNoBin
//   * file larger than the bin's MaxCapacity        -> KeptTooLarge
//
// Only then does IFileOperation run, with FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE
// and an "early failure" flag so nothing continues past a problem. The
// result is verified afterwards: the file must be gone AND the bin's item
// count must have gone up, otherwise the outcome is Failed with a message
// that says exactly what was observed.
// ---------------------------------------------------------------------------
#include "platform/RecycleBin.h"

#include "core/Expected.h"
#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Handle.h"
#include "platform/Utf.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"RecycleBin";

/// Per-volume Recycle Bin settings live under this HKCU key, one subkey per
/// volume GUID (without braces).
constexpr const wchar_t* kBitBucketVolumeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\Volume";

/// Group policy that turns every delete into a permanent one.
constexpr const wchar_t* kExplorerPolicyKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
constexpr const wchar_t* kNoRecycleFilesValue = L"NoRecycleFiles";

/// Longest registry key name we will read while enumerating volumes.
constexpr DWORD kMaxKeyNameChars = 256;

/// Longest volume GUID path GetVolumeNameForVolumeMountPointW returns
/// ("\\?\Volume{...}\" is 49 chars + NUL; 50 is the documented minimum).
constexpr DWORD kVolumeGuidPathChars = 64;

/**
 * @brief Builds a RecycleResult in one line.
 */
RecycleResult makeResult(RecycleOutcome outcome, std::wstring message) {
    RecycleResult result;
    result.outcome = outcome;
    result.message = std::move(message);
    return result;
}

/**
 * @brief Formats an HRESULT as "text (0x8007xxxx)" for messages.
 */
std::wstring describeHr(HRESULT hr) {
    return std::format(L"{} ({:#010x})", hresultText(hr), static_cast<uint32_t>(hr));
}

/**
 * @brief Parent folder of a path ("C:\a\b\clip.mp4" -> "C:\a\b").
 *        Returns the root ("C:\") for files directly under it.
 */
std::wstring parentFolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    // Keep the separator when the parent is a root like "C:\".
    if (slash == 2 && path.size() > 2 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, slash);
}

/**
 * @brief Volume GUID (no braces, no prefix) for a root like "C:\".
 *        Empty when the lookup fails.
 */
std::wstring volumeGuidForRoot(const std::wstring& root) {
    if (root.empty()) {
        return {};
    }
    // The API insists on a trailing backslash.
    std::wstring mountPoint = root;
    if (mountPoint.back() != L'\\') {
        mountPoint.push_back(L'\\');
    }
    wchar_t buffer[kVolumeGuidPathChars] = {};
    if (!::GetVolumeNameForVolumeMountPointW(mountPoint.c_str(), buffer, kVolumeGuidPathChars)) {
        const DWORD err = ::GetLastError();
        HH_LOG_DEBUG(kLog, L"GetVolumeNameForVolumeMountPointW({}) failed: {} ({})", root, win32ErrorText(err), err);
        return {};
    }

    // "\\?\Volume{GUID}\" -> "GUID"
    std::wstring guid(buffer, ::wcsnlen(buffer, kVolumeGuidPathChars));
    const size_t open = guid.find(L'{');
    const size_t close = guid.rfind(L'}');
    if (open == std::wstring::npos || close == std::wstring::npos || close <= open + 1) {
        HH_LOG_DEBUG(kLog, L"unexpected volume name format for {}: {}", root, guid);
        return {};
    }
    return guid.substr(open + 1, close - open - 1);
}

/**
 * @brief Reads a REG_DWORD value from an open key.
 * @return false when missing or not a DWORD
 */
bool readDwordValue(HKEY key, const wchar_t* name, DWORD& value) {
    if (!key || !name) {
        return false;
    }
    DWORD type = 0;
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LSTATUS status = ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(data)) {
        return false;
    }
    value = data;
    return true;
}

/**
 * @brief What Explorer stores per volume about its Recycle Bin.
 */
struct BitBucketPolicy {
    bool found = false;          ///< the volume's subkey exists
    bool nukeOnDelete = false;   ///< "Don't move files to the Recycle Bin" is ticked
    bool hasMaxCapacity = false;
    DWORD maxCapacityMb = 0;     ///< bin size in MiB
};

/**
 * @brief Enumerates HKCU\...\BitBucket\Volume and reads the subkey whose
 *        name matches @p volumeGuid (case-insensitive, braces ignored).
 *
 * A missing key means Explorer has never touched this volume's settings,
 * which is the default (bin enabled, default size): the checks are skipped.
 */
BitBucketPolicy readBitBucketPolicy(const std::wstring& volumeGuid) {
    BitBucketPolicy policy;
    if (volumeGuid.empty()) {
        return policy;
    }

    HKEY volumesKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kBitBucketVolumeKey, 0, KEY_READ, &volumesKey) != ERROR_SUCCESS ||
        !volumesKey) {
        return policy;
    }

    // Walk the subkeys looking for our GUID.
    for (DWORD index = 0;; ++index) {
        wchar_t name[kMaxKeyNameChars] = {};
        DWORD nameChars = kMaxKeyNameChars;
        const LSTATUS status = ::RegEnumKeyExW(volumesKey, index, name, &nameChars, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            // ERROR_MORE_DATA (name too long) or anything else: skip it.
            if (status == ERROR_MORE_DATA) {
                continue;
            }
            break;
        }

        // Compare without braces so either spelling matches.
        std::wstring_view candidate(name, ::wcsnlen(name, kMaxKeyNameChars));
        if (!candidate.empty() && candidate.front() == L'{') {
            candidate.remove_prefix(1);
        }
        if (!candidate.empty() && candidate.back() == L'}') {
            candidate.remove_suffix(1);
        }
        if (!iequals(candidate, volumeGuid)) {
            continue;
        }

        // Found it: read the two values that matter.
        HKEY volumeKey = nullptr;
        if (::RegOpenKeyExW(volumesKey, name, 0, KEY_QUERY_VALUE, &volumeKey) == ERROR_SUCCESS && volumeKey) {
            policy.found = true;
            DWORD nuke = 0;
            if (readDwordValue(volumeKey, L"NukeOnDelete", nuke)) {
                policy.nukeOnDelete = (nuke == 1);
            }
            DWORD capacity = 0;
            if (readDwordValue(volumeKey, L"MaxCapacity", capacity)) {
                policy.hasMaxCapacity = true;
                policy.maxCapacityMb = capacity;
            }
            ::RegCloseKey(volumeKey);
        }
        break;
    }

    ::RegCloseKey(volumesKey);
    return policy;
}

/**
 * @brief True when the NoRecycleFiles policy (user or machine) is on, which
 *        makes every shell delete permanent.
 */
bool policyDisablesRecycleBin() {
    const HKEY roots[2] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    for (HKEY root : roots) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        const LSTATUS status = ::RegGetValueW(root, kExplorerPolicyKey, kNoRecycleFilesValue, RRF_RT_REG_DWORD, &type,
                                              &value, &size);
        if (status == ERROR_SUCCESS && value == 1) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Size of a file in bytes via GetFileAttributesExW.
 * @return false when the attributes cannot be read
 */
bool fileSizeOf(const std::wstring& extendedPath, uint64_t& size) {
    size = 0;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(extendedPath.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | static_cast<uint64_t>(data.nFileSizeLow);
    return true;
}

/**
 * @brief Item count of the Recycle Bin on a volume root.
 * @return false when the query fails (no bin on that volume)
 */
bool queryBinItems(const std::wstring& root, long long& items, HRESULT& hr) {
    items = 0;
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    hr = ::SHQueryRecycleBinW(root.empty() ? nullptr : root.c_str(), &info);
    if (FAILED(hr)) {
        return false;
    }
    items = static_cast<long long>(info.i64NumItems);
    return true;
}

/**
 * @brief ShellExecuteW "open" on a target; true when the shell accepted it.
 */
bool shellOpen(const std::wstring& target) {
    if (target.empty()) {
        return false;
    }
    ScopedCoInit com;
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const INT_PTR code = reinterpret_cast<INT_PTR>(result);
    if (code <= 32) {
        HH_LOG_WARN(kLog, L"ShellExecuteW(open, {}) failed with code {}", target, code);
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// recycleFile
// ---------------------------------------------------------------------------

/**
 * @brief Moves a file to the Recycle Bin (STA thread required).
 *
 * See the file header for the exact order of pre-checks. Every "Kept*"
 * outcome leaves the file untouched; Failed also leaves it untouched unless
 * the message says otherwise.
 */
RecycleResult recycleFile(const std::wstring& path) {
    if (path.empty()) {
        return makeResult(RecycleOutcome::Failed, L"No path given.");
    }

    // Shell APIs dislike the \\?\ prefix, so keep two spellings: a plain
    // absolute path for the shell and an extended one for the Win32 calls.
    std::wstring plain = fullPath(path);
    if (plain.empty()) {
        plain = path;
    }
    const std::wstring extended = toExtendedPath(plain);
    const std::wstring& win32Path = extended.empty() ? plain : extended;

    // ---- 1. does it still exist? ---------------------------------------
    const DWORD attributes = ::GetFileAttributesW(win32Path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = ::GetLastError();
        HH_LOG_INFO(kLog, L"{} not found ({}); nothing to recycle", plain, err);
        return makeResult(RecycleOutcome::KeptNotFound, L"The file no longer exists: " + plain);
    }

    // ---- 2. network volumes have no bin ---------------------------------
    if (isRemotePath(plain)) {
        HH_LOG_INFO(kLog, L"{} is on a network volume; keeping it", plain);
        return makeResult(RecycleOutcome::KeptRemote,
                          L"Network volumes have no Recycle Bin, so the file was kept: " + plain);
    }

    // ---- 3. the volume must have a bin ----------------------------------
    std::wstring root;
    if (Result<std::wstring> rootResult = volumeRoot(plain); rootResult) {
        root = rootResult.value();
    }
    if (root.empty()) {
        HH_LOG_WARN(kLog, L"could not determine the volume root of {}; keeping it", plain);
        return makeResult(RecycleOutcome::KeptNoBin, L"Could not determine the volume of " + plain + L"; file kept.");
    }
    long long itemsBefore = 0;
    HRESULT binHr = S_OK;
    if (!queryBinItems(root, itemsBefore, binHr)) {
        HH_LOG_WARN(kLog, L"SHQueryRecycleBinW({}) failed: {}; keeping {}", root, describeHr(binHr), plain);
        return makeResult(RecycleOutcome::KeptNoBin,
                          L"Volume " + root + L" has no Recycle Bin (" + describeHr(binHr) + L"); file kept.");
    }

    // ---- 4. policy: NoRecycleFiles makes every delete permanent ---------
    if (policyDisablesRecycleBin()) {
        HH_LOG_WARN(kLog, L"NoRecycleFiles policy is active; keeping {}", plain);
        return makeResult(RecycleOutcome::KeptNoBin,
                          L"A policy disables the Recycle Bin on this machine, so the file was kept: " + plain);
    }

    // ---- 5. per-volume settings: NukeOnDelete / MaxCapacity -------------
    const std::wstring volumeGuid = volumeGuidForRoot(root);
    const BitBucketPolicy policy = readBitBucketPolicy(volumeGuid);
    if (policy.found) {
        if (policy.nukeOnDelete) {
            HH_LOG_WARN(kLog, L"volume {} is set to delete permanently (NukeOnDelete); keeping {}", root, plain);
            return makeResult(RecycleOutcome::KeptNoBin,
                              L"Volume " + root + L" is configured to bypass the Recycle Bin, so the file was kept: " +
                                  plain);
        }
        if (policy.hasMaxCapacity) {
            uint64_t size = 0;
            if (fileSizeOf(win32Path, size)) {
                const uint64_t capacityBytes = static_cast<uint64_t>(policy.maxCapacityMb) * 1024ull * 1024ull;
                if (size > capacityBytes) {
                    HH_LOG_WARN(kLog, L"{} is {} bytes but the bin on {} holds at most {} MiB; keeping it", plain, size,
                                root, policy.maxCapacityMb);
                    return makeResult(RecycleOutcome::KeptTooLarge,
                                      std::format(L"The file ({} MiB) is larger than the Recycle Bin on {} ({} MiB), "
                                                  L"so it was kept: {}",
                                                  size / (1024ull * 1024ull), root, policy.maxCapacityMb, plain));
                }
            }
        }
    }

    // ---- 6. the shell operation -----------------------------------------
    ScopedCoInit com;
    if (!com.ok()) {
        HH_LOG_ERROR(kLog, L"CoInitializeEx failed: {}", describeHr(com.hr()));
        return makeResult(RecycleOutcome::Failed, L"COM could not be initialised (" + describeHr(com.hr()) + L").");
    }

    ComPtr<IFileOperation> operation;
    HRESULT hr = ::CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
    if (FAILED(hr) || !operation) {
        HH_LOG_ERROR(kLog, L"CoCreateInstance(FileOperation) failed: {}", describeHr(hr));
        return makeResult(RecycleOutcome::Failed, L"IFileOperation is unavailable (" + describeHr(hr) + L").");
    }

    // Undo-able, silent, recycle-only, stop at the first problem; and let
    // the shell warn if it would have to destroy the file instead.
    const DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE |
                        FOFX_EARLYFAILURE;
    hr = operation->SetOperationFlags(flags);
    if (FAILED(hr)) {
        HH_LOG_ERROR(kLog, L"SetOperationFlags failed: {}", describeHr(hr));
        return makeResult(RecycleOutcome::Failed, L"Could not configure the shell operation (" + describeHr(hr) + L").");
    }

    ComPtr<IShellItem> item;
    hr = ::SHCreateItemFromParsingName(plain.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item) {
        HH_LOG_ERROR(kLog, L"SHCreateItemFromParsingName({}) failed: {}", plain, describeHr(hr));
        return makeResult(RecycleOutcome::Failed, L"The shell could not open " + plain + L" (" + describeHr(hr) + L").");
    }

    hr = operation->DeleteItem(item.Get(), nullptr);
    if (FAILED(hr)) {
        HH_LOG_ERROR(kLog, L"DeleteItem({}) failed: {}", plain, describeHr(hr));
        return makeResult(RecycleOutcome::Failed, L"Could not queue the recycle of " + plain + L" (" + describeHr(hr) + L").");
    }

    hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    const HRESULT abortHr = operation->GetAnyOperationsAborted(&aborted);
    if (FAILED(abortHr)) {
        aborted = FALSE;
    }

    // ---- 7. verify what actually happened -------------------------------
    const bool gone = (::GetFileAttributesW(win32Path.c_str()) == INVALID_FILE_ATTRIBUTES);
    long long itemsAfter = 0;
    HRESULT afterHr = S_OK;
    const bool binQueried = queryBinItems(root, itemsAfter, afterHr);

    if (FAILED(hr) || aborted) {
        HH_LOG_WARN(kLog, L"recycle of {} did not complete: hr={} aborted={} gone={}", plain, describeHr(hr), aborted != 0,
                    gone);
        if (gone) {
            return makeResult(RecycleOutcome::Failed,
                              L"The shell reported a problem (" + describeHr(hr) +
                                  L") and the file is no longer at " + plain + L". Check the Recycle Bin.");
        }
        if (aborted) {
            return makeResult(RecycleOutcome::Failed, L"The recycle operation was cancelled; the file was kept: " + plain);
        }
        return makeResult(RecycleOutcome::Failed,
                          L"The shell could not recycle " + plain + L" (" + describeHr(hr) + L"); the file was kept.");
    }

    if (!gone) {
        HH_LOG_WARN(kLog, L"shell reported success but {} still exists", plain);
        return makeResult(RecycleOutcome::Failed, L"The shell reported success but the file is still present: " + plain);
    }
    if (!binQueried) {
        HH_LOG_WARN(kLog, L"{} is gone but the bin on {} could not be re-queried ({})", plain, root, describeHr(afterHr));
        return makeResult(RecycleOutcome::Failed,
                          L"The file was removed but the Recycle Bin on " + root +
                              L" could not be queried to confirm it arrived (" + describeHr(afterHr) + L").");
    }
    if (itemsAfter <= itemsBefore) {
        HH_LOG_ERROR(kLog, L"{} is gone but the bin on {} did not grow ({} -> {})", plain, root, itemsBefore, itemsAfter);
        return makeResult(RecycleOutcome::Failed,
                          std::format(L"The file was removed but the Recycle Bin on {} did not grow ({} -> {} items). "
                                      L"It may have been deleted permanently: {}",
                                      root, itemsBefore, itemsAfter, plain));
    }

    HH_LOG_INFO(kLog, L"recycled {} (bin on {}: {} -> {} items)", plain, root, itemsBefore, itemsAfter);
    return makeResult(RecycleOutcome::Recycled, L"Moved to the Recycle Bin: " + plain);
}

// ---------------------------------------------------------------------------
// Explorer helpers
// ---------------------------------------------------------------------------

/**
 * @brief Opens an Explorer window with the file selected; falls back to
 *        opening the containing folder when the item cannot be selected.
 */
bool revealInExplorer(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    std::wstring plain = fullPath(path);
    if (plain.empty()) {
        plain = path;
    }

    // Selecting an item needs a PIDL, which needs COM.
    {
        ScopedCoInit com;
        if (com.ok()) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            SFGAOF attributes = 0;
            HRESULT hr = ::SHParseDisplayName(plain.c_str(), nullptr, &pidl, 0, &attributes);
            if (SUCCEEDED(hr) && pidl) {
                hr = ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ::ILFree(pidl);
                pidl = nullptr;
                if (SUCCEEDED(hr)) {
                    return true;
                }
                HH_LOG_DEBUG(kLog, L"SHOpenFolderAndSelectItems({}) failed: {}", plain, describeHr(hr));
            } else {
                HH_LOG_DEBUG(kLog, L"SHParseDisplayName({}) failed: {}", plain, describeHr(hr));
            }
        } else {
            HH_LOG_DEBUG(kLog, L"CoInitializeEx failed before reveal: {}", describeHr(com.hr()));
        }
    }

    // Fallback: at least open the folder.
    const std::wstring folder = parentFolderOf(plain);
    if (folder.empty()) {
        return false;
    }
    return openFolder(folder);
}

/**
 * @brief Opens a folder in Explorer.
 */
bool openFolder(const std::wstring& folder) {
    if (folder.empty()) {
        return false;
    }
    return shellOpen(folder);
}

/**
 * @brief Opens a URL or document with its default handler.
 */
bool openWithShell(const std::wstring& target) {
    if (target.empty()) {
        return false;
    }
    return shellOpen(target);
}

} // namespace hh::platform
