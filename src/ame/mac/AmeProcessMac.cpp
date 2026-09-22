// ---------------------------------------------------------------------------
// AmeProcessMac.cpp - AmeProcess.h on macOS.
//
// AME installs as "/Applications/Adobe Media Encoder <year>/Adobe Media
// Encoder <year>.app", so the main executable is
//
//     .../Adobe Media Encoder 2026.app/Contents/MacOS/Adobe Media Encoder 2026
//
// The year changes every release, hence a prefix match on the image name.
// Helper executables that share the prefix live deeper in the bundle
// (Contents/Frameworks/..., Contents/Resources/...), so a candidate only
// counts when it is the bundle's own main executable.
// ---------------------------------------------------------------------------
#include "ame/AmeProcess.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/Process.h"
#include "platform/Utf.h"

#include <string>
#include <string_view>

namespace hh::ame {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"AmeProcess";

/// Every AME release's executable starts with this.
constexpr std::wstring_view kAmeImagePrefix = L"Adobe Media Encoder";

/**
 * @brief True when @p imagePath is "<X>.app/Contents/MacOS/<X>", i.e. the
 *        main executable of an app bundle rather than a nested helper.
 */
bool isBundleMainExecutable(const std::wstring& imagePath)
{
    if (imagePath.empty()) {
        return false;
    }
    // Walk up: <X> -> MacOS -> Contents -> <X>.app
    const std::wstring name = path::fileName(imagePath);
    const std::wstring macosDir = path::parent(imagePath);
    const std::wstring contentsDir = path::parent(macosDir);
    const std::wstring bundleDir = path::parent(contentsDir);
    if (name.empty() || macosDir.empty() || contentsDir.empty() || bundleDir.empty()) {
        return false;
    }
    return platform::iequals(path::fileName(macosDir), L"MacOS") &&
           platform::iequals(path::fileName(contentsDir), L"Contents") &&
           platform::iequals(path::fileName(bundleDir), name + L".app");
}

} // namespace

/**
 * @brief True while any Adobe Media Encoder release is running for any user
 *        visible to us (the check only needs the image path, which libproc
 *        reports for other users' processes too).
 */
bool isAmeRunning()
{
    for (const DWORD pid : platform::findProcessesByImagePrefix(kAmeImagePrefix)) {
        const std::wstring imagePath = platform::processImagePath(pid);
        if (isBundleMainExecutable(imagePath)) {
            return true;
        }
        HH_LOG_DEBUG(kLog, L"pid {} shares the AME prefix but is not the app ({})", pid, imagePath);
    }
    return false;
}

} // namespace hh::ame
