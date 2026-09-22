// ---------------------------------------------------------------------------
// Terms.h - the OS nouns that appear in user-facing text.
//
// "Move it to the Recycle Bin" reads wrong on a Mac, and "Show in Finder"
// reads wrong on Windows. Every string the user sees that names an OS
// concept builds it from these, so each platform speaks its own language
// while the sentences around the nouns stay shared.
// ---------------------------------------------------------------------------
#pragma once

namespace hh::platform::terms {

#if defined(_WIN32)
/// Where deleted files go ("... to the Recycle Bin").
inline constexpr const wchar_t* kTrash = L"Recycle Bin";
/// The file browser ("Show in Explorer").
inline constexpr const wchar_t* kFileBrowser = L"Explorer";
/// Where the app lives while its window is closed ("waits in the tray").
inline constexpr const wchar_t* kTray = L"tray";
/// mkvmerge's executable file name.
inline constexpr const wchar_t* kMkvmergeExe = L"mkvmerge.exe";
#else
inline constexpr const wchar_t* kTrash = L"Trash";
inline constexpr const wchar_t* kFileBrowser = L"Finder";
inline constexpr const wchar_t* kTray = L"menu bar";
inline constexpr const wchar_t* kMkvmergeExe = L"mkvmerge";
#endif

} // namespace hh::platform::terms
