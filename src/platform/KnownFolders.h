// ---------------------------------------------------------------------------
// KnownFolders.h - well-known directories, always without a trailing slash.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <string>

namespace hh::platform {

/// The user's Documents folder (honours folder redirection).
std::wstring documentsFolder();
/// %LOCALAPPDATA%
std::wstring localAppDataFolder();
/// %APPDATA%
std::wstring roamingAppDataFolder();
/// C:\Program Files
std::wstring programFilesX64Folder();
/// C:\Program Files (x86)
std::wstring programFilesX86Folder();
/// %TEMP%
std::wstring tempFolder();

/// Full path of the running executable.
std::wstring exePath();
/// Directory containing the running executable (no trailing slash).
std::wstring exeDirectory();

/// %LOCALAPPDATA%\HdrHint (created on demand).
std::wstring appLocalDataFolder();
/// %APPDATA%\HdrHint (created on demand).
std::wstring appRoamingDataFolder();

} // namespace hh::platform
