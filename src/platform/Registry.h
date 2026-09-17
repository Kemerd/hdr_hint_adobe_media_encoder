// ---------------------------------------------------------------------------
// Registry.h - tiny HKCU/HKLM value helpers.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#include <string>

namespace hh::platform {

/// Reads a REG_SZ / REG_EXPAND_SZ value.
Result<std::wstring> regReadString(HKEY root, std::wstring_view subKey, std::wstring_view name);
/// Reads a REG_DWORD value.
Result<DWORD> regReadDword(HKEY root, std::wstring_view subKey, std::wstring_view name);
/// Writes a REG_SZ value (creates the key).
Result<void> regWriteString(HKEY root, std::wstring_view subKey, std::wstring_view name, std::wstring_view value);
/// Writes a REG_DWORD value (creates the key).
Result<void> regWriteDword(HKEY root, std::wstring_view subKey, std::wstring_view name, DWORD value);
/// Deletes a whole key (recursively).
Result<void> regDeleteKey(HKEY root, std::wstring_view subKey);
/// Deletes one value (success when it does not exist).
Result<void> regDeleteValue(HKEY root, std::wstring_view subKey, std::wstring_view name);
/// True when the key exists.
bool regKeyExists(HKEY root, std::wstring_view subKey);

} // namespace hh::platform
