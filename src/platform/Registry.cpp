// ---------------------------------------------------------------------------
// Registry.cpp - tiny HKCU/HKLM value helpers.
//
// Everything goes through the RegGetValueW / RegSetKeyValueW family, which
// open and close the key for us and never leave handles behind. Reads of
// REG_EXPAND_SZ values are expanded transparently (RRF_RT_REG_SZ without
// RRF_NOEXPAND), which is what every caller in this app wants for paths.
// ---------------------------------------------------------------------------
#include "platform/Registry.h"

#include "core/Logger.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace hh::platform {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"Registry";

/// Upper bound on a string value we are willing to read (1 MiB of UTF-16).
constexpr DWORD kMaxStringBytes = 1024u * 1024u * 2u;

/**
 * @brief Null-terminated copy of a view, or nullptr for an empty view (the
 *        registry APIs treat NULL and "" the same, but NULL is clearer).
 */
struct KeyText {
    std::wstring storage;
    const wchar_t* ptr = nullptr;

    explicit KeyText(std::wstring_view text) : storage(text)
    {
        ptr = storage.empty() ? nullptr : storage.c_str();
    }
};

/**
 * @brief Builds the "regOp(HKxx\sub\name)" context used in error messages.
 */
std::wstring context(std::wstring_view op, HKEY root, std::wstring_view subKey, std::wstring_view name)
{
    // Only the predefined roots have a friendly name; anything else is a handle.
    std::wstring rootName;
    if (root == HKEY_CURRENT_USER) {
        rootName = L"HKCU";
    } else if (root == HKEY_LOCAL_MACHINE) {
        rootName = L"HKLM";
    } else if (root == HKEY_CLASSES_ROOT) {
        rootName = L"HKCR";
    } else if (root == HKEY_USERS) {
        rootName = L"HKU";
    } else {
        rootName = L"HKEY";
    }
    if (name.empty()) {
        return std::format(L"{}({}\\{})", op, rootName, subKey);
    }
    return std::format(L"{}({}\\{}\\{})", op, rootName, subKey, name);
}

} // namespace

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

/**
 * @brief Reads a REG_SZ / REG_EXPAND_SZ value (expanded).
 *
 * The size reported for a REG_EXPAND_SZ value can be smaller than the
 * expanded text, so ERROR_MORE_DATA is handled by growing the buffer to the
 * size the second call reports and trying again.
 */
Result<std::wstring> regReadString(HKEY root, std::wstring_view subKey, std::wstring_view name)
{
    if (root == nullptr) {
        return Error::text(L"regReadString: null root key");
    }
    const KeyText key(subKey);
    const KeyText value(name);
    const DWORD flags = RRF_RT_REG_SZ;

    // Size query first (pvData == nullptr).
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = ::RegGetValueW(root, key.ptr, value.ptr, flags, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS) {
        return Error::fromWin32(static_cast<DWORD>(status), context(L"regReadString", root, subKey, name));
    }
    if (bytes == 0) {
        return std::wstring();
    }

    // Fetch loop: grows on ERROR_MORE_DATA, bounded to avoid spinning.
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (bytes > kMaxStringBytes) {
            return Error::text(context(L"regReadString", root, subKey, name) + L": value too large");
        }
        // One extra code unit guarantees a terminator even for odd sizes.
        std::wstring buffer(static_cast<size_t>(bytes) / sizeof(wchar_t) + 1, L'\0');
        DWORD capacity = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        status = ::RegGetValueW(root, key.ptr, value.ptr, flags, &type, buffer.data(), &capacity);
        if (status == ERROR_MORE_DATA) {
            bytes = std::max<DWORD>(capacity, bytes * 2u);
            continue;
        }
        if (status != ERROR_SUCCESS) {
            return Error::fromWin32(static_cast<DWORD>(status), context(L"regReadString", root, subKey, name));
        }

        // Trim to the returned byte count, then to the first NUL.
        const size_t units = std::min(buffer.size(), static_cast<size_t>(capacity) / sizeof(wchar_t));
        buffer.resize(units);
        const size_t nul = buffer.find(L'\0');
        if (nul != std::wstring::npos) {
            buffer.resize(nul);
        }
        return buffer;
    }
    return Error::fromWin32(ERROR_MORE_DATA, context(L"regReadString", root, subKey, name));
}

/**
 * @brief Reads a REG_DWORD value (a 4-byte REG_BINARY is accepted too).
 */
Result<DWORD> regReadDword(HKEY root, std::wstring_view subKey, std::wstring_view name)
{
    if (root == nullptr) {
        return Error::text(L"regReadDword: null root key");
    }
    const KeyText key(subKey);
    const KeyText value(name);

    // RRF_RT_DWORD restricts to 32-bit data of either type.
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const LSTATUS status = ::RegGetValueW(root, key.ptr, value.ptr, RRF_RT_DWORD, &type, &data, &bytes);
    if (status != ERROR_SUCCESS) {
        return Error::fromWin32(static_cast<DWORD>(status), context(L"regReadDword", root, subKey, name));
    }
    if (bytes != sizeof(data)) {
        return Error::text(context(L"regReadDword", root, subKey, name) + L": unexpected value size");
    }
    return data;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

/**
 * @brief Writes a REG_SZ value, creating the key path as needed.
 */
Result<void> regWriteString(HKEY root, std::wstring_view subKey, std::wstring_view name, std::wstring_view value)
{
    if (root == nullptr) {
        return Error::text(L"regWriteString: null root key");
    }
    const KeyText key(subKey);
    const KeyText valueName(name);

    // The data must be null-terminated and the byte count must include it.
    const std::wstring text(value);
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (bytes > kMaxStringBytes) {
        return Error::text(context(L"regWriteString", root, subKey, name) + L": value too large");
    }
    const LSTATUS status = ::RegSetKeyValueW(root, key.ptr, valueName.ptr, REG_SZ, text.c_str(), static_cast<DWORD>(bytes));
    if (status != ERROR_SUCCESS) {
        Error e = Error::fromWin32(static_cast<DWORD>(status), context(L"regWriteString", root, subKey, name));
        HH_LOG_WARN(kLog, L"{}", e.toString());
        return e;
    }
    return Result<void>::success();
}

/**
 * @brief Writes a REG_DWORD value, creating the key path as needed.
 */
Result<void> regWriteDword(HKEY root, std::wstring_view subKey, std::wstring_view name, DWORD value)
{
    if (root == nullptr) {
        return Error::text(L"regWriteDword: null root key");
    }
    const KeyText key(subKey);
    const KeyText valueName(name);

    const LSTATUS status = ::RegSetKeyValueW(root, key.ptr, valueName.ptr, REG_DWORD, &value, sizeof(value));
    if (status != ERROR_SUCCESS) {
        Error e = Error::fromWin32(static_cast<DWORD>(status), context(L"regWriteDword", root, subKey, name));
        HH_LOG_WARN(kLog, L"{}", e.toString());
        return e;
    }
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

/**
 * @brief Deletes a key and everything below it.
 *
 * An empty sub key is refused outright: RegDeleteTree on a bare root would
 * wipe the whole hive. A key that is already gone counts as success.
 */
Result<void> regDeleteKey(HKEY root, std::wstring_view subKey)
{
    if (root == nullptr) {
        return Error::text(L"regDeleteKey: null root key");
    }
    if (subKey.empty()) {
        return Error::text(L"regDeleteKey: refusing to delete a root hive");
    }
    const KeyText key(subKey);

    const LSTATUS status = ::RegDeleteTreeW(root, key.ptr);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return Result<void>::success();
    }
    Error e = Error::fromWin32(static_cast<DWORD>(status), context(L"regDeleteKey", root, subKey, L""));
    HH_LOG_WARN(kLog, L"{}", e.toString());
    return e;
}

/**
 * @brief Deletes a single value under a key.
 *
 * A key or value that is already gone counts as success, so callers can
 * "turn off" a setting without checking first.
 */
Result<void> regDeleteValue(HKEY root, std::wstring_view subKey, std::wstring_view name)
{
    if (root == nullptr) {
        return Error::text(L"regDeleteValue: null root key");
    }
    if (subKey.empty() || name.empty()) {
        return Error::text(L"regDeleteValue: empty key or value name");
    }
    const KeyText key(subKey);
    const KeyText valueName(name);

    // Open with KEY_SET_VALUE only; a missing key means nothing to delete.
    HKEY handle = nullptr;
    LSTATUS status = ::RegOpenKeyExW(root, key.ptr, 0, KEY_SET_VALUE, &handle);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return Result<void>::success();
    }
    if (status != ERROR_SUCCESS || handle == nullptr) {
        Error e = Error::fromWin32(static_cast<DWORD>(status), context(L"regDeleteValue", root, subKey, name));
        HH_LOG_WARN(kLog, L"{}", e.toString());
        return e;
    }

    status = ::RegDeleteValueW(handle, valueName.ptr);
    ::RegCloseKey(handle);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) {
        return Result<void>::success();
    }
    Error e = Error::fromWin32(static_cast<DWORD>(status), context(L"regDeleteValue", root, subKey, name));
    HH_LOG_WARN(kLog, L"{}", e.toString());
    return e;
}

/**
 * @brief True when the key exists.
 *
 * A key we are not allowed to open still exists, so ERROR_ACCESS_DENIED
 * counts as present.
 */
bool regKeyExists(HKEY root, std::wstring_view subKey)
{
    if (root == nullptr) {
        return false;
    }
    const KeyText key(subKey);

    HKEY handle = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(root, key.ptr, 0, KEY_READ, &handle);
    if (status == ERROR_SUCCESS) {
        if (handle != nullptr) {
            ::RegCloseKey(handle);
        }
        return true;
    }
    return status == ERROR_ACCESS_DENIED;
}

} // namespace hh::platform
