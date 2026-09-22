// ---------------------------------------------------------------------------
// ShellServicesWin.cpp - ShellServices.h on Windows.
//
// The clipboard (CF_UNICODETEXT), the VERSIONINFO reader and the
// IFileOpenDialog picker, moved here verbatim from AppViewModels.cpp so the
// view models stay platform-neutral.
// ---------------------------------------------------------------------------
#include "ui/app/ShellServices.h"

#include "core/Expected.h"
#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Handle.h"
#include "platform/KnownFolders.h"

#include <shobjidl_core.h>
#include <winver.h>

#include <cstring>
#include <format>
#include <vector>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"ViewModel";

/// Version shown when the executable carries no VERSIONINFO (tests, stripped builds).
constexpr const wchar_t* kFallbackVersion = L"1.0.0";

} // namespace

bool copyTextToClipboard(NativeWindowHandle owner, std::wstring_view text) {
    // Another process may hold the clipboard for a moment; a few short retries
    // cover the common case (a clipboard manager peeking) without stalling the UI.
    bool opened = false;
    for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
        opened = ::OpenClipboard(owner) != FALSE;
        if (!opened) {
            ::Sleep(10);
        }
    }
    if (!opened) {
        HH_LOG_WARN(kLog, L"OpenClipboard failed ({})", ::GetLastError());
        return false;
    }

    bool ok = false;
    if (!::EmptyClipboard()) {
        HH_LOG_WARN(kLog, L"EmptyClipboard failed ({})", ::GetLastError());
    } else {
        // CF_UNICODETEXT wants a movable global block with a terminating NUL.
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL global = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!global) {
            HH_LOG_WARN(kLog, L"GlobalAlloc({}) failed ({})", bytes, ::GetLastError());
        } else {
            void* memory = ::GlobalLock(global);
            if (!memory) {
                HH_LOG_WARN(kLog, L"GlobalLock failed ({})", ::GetLastError());
            } else {
                if (!text.empty()) {
                    std::memcpy(memory, text.data(), text.size() * sizeof(wchar_t));
                }
                static_cast<wchar_t*>(memory)[text.size()] = L'\0';
                ::GlobalUnlock(global);
                // On success the clipboard owns the block; on failure we free it.
                if (::SetClipboardData(CF_UNICODETEXT, global)) {
                    ok = true;
                    global = nullptr;
                } else {
                    HH_LOG_WARN(kLog, L"SetClipboardData failed ({})", ::GetLastError());
                }
            }
            if (global) {
                ::GlobalFree(global);
            }
        }
    }
    ::CloseClipboard();
    return ok;
}

std::wstring appVersionString() {
    // The exe never changes while running; read the version block once.
    static const std::wstring cached = []() -> std::wstring {
        const std::wstring exe = platform::exePath();
        if (exe.empty()) {
            return kFallbackVersion;
        }
        DWORD ignored = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(exe.c_str(), &ignored);
        if (size == 0) {
            HH_LOG_DEBUG(kLog, L"no VERSIONINFO in {} ({})", exe, ::GetLastError());
            return kFallbackVersion;
        }
        std::vector<uint8_t> block(size);
        if (!::GetFileVersionInfoW(exe.c_str(), 0, size, block.data())) {
            HH_LOG_DEBUG(kLog, L"GetFileVersionInfoW failed ({})", ::GetLastError());
            return kFallbackVersion;
        }
        // The root block is the fixed info: product version as two DWORDs.
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (!::VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed ||
            length < sizeof(VS_FIXEDFILEINFO)) {
            return kFallbackVersion;
        }
        const unsigned major = HIWORD(fixed->dwProductVersionMS);
        const unsigned minor = LOWORD(fixed->dwProductVersionMS);
        const unsigned patch = HIWORD(fixed->dwProductVersionLS);
        return std::format(L"{}.{}.{}", major, minor, patch);
    }();
    return cached;
}

/**
 * @brief Shows an IFileOpenDialog and returns the chosen path.
 *
 * Shared by both view models: the queue needs the same .cube picker the
 * settings screen uses, so the dialog code lives here rather than on one of
 * them. Returns an empty string when the user cancels or the shell refuses.
 */
std::wstring showPathPicker(NativeWindowHandle owner, bool pickFolder, const wchar_t* title, const std::wstring& startFolder,
                            const wchar_t* filterLabel, const wchar_t* filterPattern) {
    // COM is initialised by the app on the UI thread; a failure here is logged, not fatal.
    platform::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = ::CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        HH_LOG_ERROR(kLog, L"CoCreateInstance(FileOpenDialog) failed: {}", hresultText(hr));
        return {};
    }

    // File-system items only; folders when asked; never change the process cwd.
    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (SUCCEEDED(hr)) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
        options |= pickFolder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
        hr = dialog->SetOptions(options);
    }
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"IFileOpenDialog options failed: {}", hresultText(hr));
    }
    if (title && *title) {
        if (FAILED(dialog->SetTitle(title))) {
            HH_LOG_DEBUG(kLog, L"IFileOpenDialog::SetTitle failed");
        }
    }

    // A file filter narrows the list to what we can actually use.
    if (!pickFolder && filterLabel && filterPattern) {
        const COMDLG_FILTERSPEC specs[] = {
            {filterLabel, filterPattern},
            {L"All files", L"*.*"},
        };
        if (FAILED(dialog->SetFileTypes(static_cast<UINT>(std::size(specs)), specs))) {
            HH_LOG_DEBUG(kLog, L"IFileOpenDialog::SetFileTypes failed");
        } else if (FAILED(dialog->SetFileTypeIndex(1))) {
            HH_LOG_DEBUG(kLog, L"IFileOpenDialog::SetFileTypeIndex failed");
        }
    }

    // Start where the current value points, unless the shell remembers a better place.
    if (!startFolder.empty() && platform::isDirectory(startFolder)) {
        platform::ComPtr<IShellItem> folder;
        hr = ::SHCreateItemFromParsingName(startFolder.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr) && folder) {
            if (FAILED(dialog->SetDefaultFolder(folder.Get()))) {
                HH_LOG_DEBUG(kLog, L"IFileOpenDialog::SetDefaultFolder failed");
            }
        }
    }

    // Modal on the owner; cancel is the normal exit, not an error.
    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"IFileOpenDialog::Show failed: {}", hresultText(hr));
        return {};
    }

    platform::ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        HH_LOG_WARN(kLog, L"IFileOpenDialog::GetResult failed: {}", hresultText(hr));
        return {};
    }
    PWSTR raw = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    if (FAILED(hr) || !raw) {
        HH_LOG_WARN(kLog, L"IShellItem::GetDisplayName failed: {}", hresultText(hr));
        return {};
    }
    // The shell allocated the string; free it once copied.
    const platform::CoTaskMemPtr<wchar_t> holder(raw);
    return std::wstring(raw);
}

} // namespace hh::ui
