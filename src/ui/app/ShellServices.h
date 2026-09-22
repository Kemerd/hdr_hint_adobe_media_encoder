// ---------------------------------------------------------------------------
// ShellServices.h - the few desktop-shell services the view models call.
//
// File/folder pickers, the clipboard and the app version are the only OS
// services the view models need beyond the engine. Keeping them behind these
// functions lets AppViewModels.cpp stay platform-neutral:
//
//   Windows  ShellServicesWin.cpp  IFileOpenDialog, CF_UNICODETEXT, VERSIONINFO
//   macOS    ui/mac/ShellServicesMac.mm  NSOpenPanel, NSPasteboard, Info.plist
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <string>
#include <string_view>

namespace hh::ui {

#if defined(_WIN32)
/// The window dialogs are modal to (and that owns the clipboard).
using NativeWindowHandle = HWND;
#else
/// The window dialogs attach to as sheets (an NSWindow*, or nullptr for app-modal).
using NativeWindowHandle = void*;
#endif

/**
 * @brief Puts text on the clipboard as plain Unicode text.
 * @param owner  window that becomes the clipboard owner (may be null)
 * @param text   the text; an empty string clears the clipboard
 * @return true when the clipboard now holds @p text
 */
bool copyTextToClipboard(NativeWindowHandle owner, std::wstring_view text);

/**
 * @brief "1.0.0" read from the executable's version resource (Windows
 *        VERSIONINFO, macOS CFBundleShortVersionString), falling back to the
 *        compiled-in version when the resource is missing.
 */
std::wstring appVersionString();

/**
 * @brief Shows a modal file or folder picker and returns the chosen path.
 *
 * @param owner          dialog owner (may be null)
 * @param pickFolder     true = choose a folder, false = choose a file
 * @param title          dialog title / message
 * @param startFolder    folder the dialog opens in (ignored when it does not exist)
 * @param filterLabel    human label of the file filter (nullptr = no filter)
 * @param filterPattern  "*.cube" style pattern (macOS: "*.ext" restricts the
 *                       extension; any other pattern allows every file)
 * @return the chosen path, or empty when the user cancelled or the shell refused
 */
std::wstring showPathPicker(NativeWindowHandle owner, bool pickFolder, const wchar_t* title,
                            const std::wstring& startFolder, const wchar_t* filterLabel,
                            const wchar_t* filterPattern);

} // namespace hh::ui
