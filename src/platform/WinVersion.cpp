// ---------------------------------------------------------------------------
// WinVersion.cpp - OS build detection without the deprecated GetVersionEx.
//
// GetVersionExW lies once an executable is not manifested for the running
// OS; RtlGetVersion in ntdll always reports the truth. It is resolved with
// GetProcAddress so no DDK import library is needed. Should it ever be
// missing, the file version of kernel32.dll is used instead.
// ---------------------------------------------------------------------------
#include "platform/WinVersion.h"

#include "core/Logger.h"

#include <winver.h>

#include <string>
#include <vector>

namespace hh::platform {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"WinVersion";

/// Signature of ntdll!RtlGetVersion (NTSTATUS is a LONG).
using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

/// First Windows 11 build.
constexpr DWORD kWindows11Build = 22000;

/// Windows 11 22H2: DWMWA_SYSTEMBACKDROP_TYPE became public.
constexpr DWORD kSystemBackdropBuild = 22621;

/**
 * @brief Asks ntdll!RtlGetVersion for the build number.
 * @return Build number, or 0 when the export cannot be resolved.
 */
DWORD buildFromRtlGetVersion()
{
    // ntdll is always mapped into every process; no LoadLibrary needed.
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        HH_LOG_WARN(kLog, L"GetModuleHandle(ntdll) failed: {}", Error::fromLastError(L"").toString());
        return 0;
    }
    const FARPROC proc = ::GetProcAddress(ntdll, "RtlGetVersion");
    if (proc == nullptr) {
        HH_LOG_WARN(kLog, L"RtlGetVersion export not found");
        return 0;
    }

    // NTSTATUS >= 0 means success.
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(proc);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const LONG status = rtlGetVersion(&info);
    if (status < 0) {
        HH_LOG_WARN(kLog, L"RtlGetVersion failed with NTSTATUS 0x{:08X}", static_cast<unsigned long>(status));
        return 0;
    }
    return info.dwBuildNumber;
}

/**
 * @brief Fallback: the product version stamped on kernel32.dll, whose third
 *        component is the OS build number.
 * @return Build number, or 0 on any failure.
 */
DWORD buildFromKernel32Version()
{
    const wchar_t* module = L"kernel32.dll";
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(module, &handle);
    if (size == 0) {
        HH_LOG_WARN(kLog, L"GetFileVersionInfoSize(kernel32) failed: {}", Error::fromLastError(L"").toString());
        return 0;
    }

    // Pull the whole version block, then query the fixed info root.
    std::vector<unsigned char> block(static_cast<size_t>(size));
    if (!::GetFileVersionInfoW(module, 0, size, block.data())) {
        HH_LOG_WARN(kLog, L"GetFileVersionInfo(kernel32) failed: {}", Error::fromLastError(L"").toString());
        return 0;
    }
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLength = 0;
    if (!::VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedLength)
        || fixed == nullptr || fixedLength < sizeof(VS_FIXEDFILEINFO)) {
        HH_LOG_WARN(kLog, L"VerQueryValue(kernel32) failed");
        return 0;
    }
    // Product version is major.minor.build.revision packed in two DWORDs.
    return static_cast<DWORD>(HIWORD(fixed->dwProductVersionLS));
}

} // namespace

/**
 * @brief Windows build number, resolved once and cached for the process.
 */
DWORD windowsBuildNumber()
{
    // Function-local static: thread-safe one-time initialisation.
    static const DWORD cached = [] {
        DWORD build = buildFromRtlGetVersion();
        if (build == 0) {
            build = buildFromKernel32Version();
        }
        HH_LOG_DEBUG(kLog, L"Windows build {}", build);
        return build;
    }();
    return cached;
}

/**
 * @brief True on Windows 11 (build 22000 and later).
 */
bool isWindows11()
{
    return windowsBuildNumber() >= kWindows11Build;
}

/**
 * @brief True when DWMWA_SYSTEMBACKDROP_TYPE (Mica / Acrylic) is available.
 */
bool supportsSystemBackdrop()
{
    return windowsBuildNumber() >= kSystemBackdropBuild;
}

/**
 * @brief True inside a Remote Desktop session (animations should be toned
 *        down and DirectComposition may be software-rendered).
 */
bool isRemoteSession()
{
    return ::GetSystemMetrics(SM_REMOTESESSION) != 0;
}

} // namespace hh::platform
