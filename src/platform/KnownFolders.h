// ---------------------------------------------------------------------------
// KnownFolders.h - well-known directories, always without a trailing slash.
//
// Windows paths come from SHGetKnownFolderPath; macOS paths come from
// NSFileManager, so both honour folder redirection / iCloud Documents.
//
//   concept          Windows                         macOS
//   documents        Documents                       ~/Documents
//   local app data   %LOCALAPPDATA%                  ~/Library/Application Support
//   roaming data     %APPDATA%                       ~/Library/Application Support
//   program files    C:\Program Files                /Applications
//   temp             %TEMP%                          $TMPDIR (per-user)
//   logs             %LOCALAPPDATA%\HdrHint\logs     ~/Library/Logs/HdrHint
//   resources        next to HdrHint.exe             HdrHint.app/Contents/Resources
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <string>

namespace hh::platform {

/// The user's Documents folder (honours folder redirection).
std::wstring documentsFolder();
/// %LOCALAPPDATA% (macOS: ~/Library/Application Support)
std::wstring localAppDataFolder();
/// %APPDATA% (macOS: ~/Library/Application Support)
std::wstring roamingAppDataFolder();
/// C:\Program Files (macOS: /Applications)
std::wstring programFilesX64Folder();
/// C:\Program Files (x86) (macOS: ~/Applications, the per-user app folder)
std::wstring programFilesX86Folder();
/// %TEMP% (macOS: the per-user $TMPDIR)
std::wstring tempFolder();

/// Full path of the running executable.
std::wstring exePath();
/// Directory containing the running executable (no trailing slash).
std::wstring exeDirectory();

/**
 * @brief Where the shipped assets (luts, cep, GUIDE.md) live.
 *
 * Windows: next to the exe. macOS: the bundle's Contents/Resources when the
 * binary runs from inside HdrHint.app, else next to the binary (build tree,
 * the command-line tool).
 */
std::wstring resourceDirectory();

/**
 * @brief The thing to launch to start this app again.
 *
 * Windows: the exe. macOS: the enclosing .app bundle when there is one (so
 * LaunchServices, the AME startup script and Login Items open the app, not a
 * bare binary in a Terminal window), else the binary.
 */
std::wstring launchablePath();

/// %LOCALAPPDATA%\HdrHint (created on demand).
std::wstring appLocalDataFolder();
/// %APPDATA%\HdrHint (created on demand).
std::wstring appRoamingDataFolder();
/// Where the rolling log lives (created on demand).
std::wstring appLogsFolder();

} // namespace hh::platform
