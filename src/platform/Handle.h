// ---------------------------------------------------------------------------
// Handle.h - RAII wrappers for kernel handles, COM pointers and COM init.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <combaseapi.h>
#include <wrl/client.h>

#include <memory>
#include <utility>

namespace hh::platform {

/// COM smart pointer (ships with the Windows SDK, no third-party dependency).
using Microsoft::WRL::ComPtr;

/**
 * @brief Owns a kernel HANDLE and closes it with CloseHandle.
 *
 * Both nullptr and INVALID_HANDLE_VALUE are normalised to "empty" so callers
 * can write `UniqueHandle h(CreateFileW(...)); if (!h) {...}` regardless of
 * which failure sentinel the API uses.
 */
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(normalize(h)) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) { reset(other.release()); }
        return *this;
    }

    /// Raw handle (nullptr when empty).
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    /// True when a real handle is owned.
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// Gives up ownership without closing.
    [[nodiscard]] HANDLE release() noexcept { HANDLE h = handle_; handle_ = nullptr; return h; }

    /// Closes the current handle (if any) and takes ownership of @p h.
    void reset(HANDLE h = nullptr) noexcept {
        HANDLE old = handle_;
        handle_ = normalize(h);
        if (old != nullptr) { ::CloseHandle(old); }
    }

private:
    static HANDLE normalize(HANDLE h) noexcept { return (h == INVALID_HANDLE_VALUE) ? nullptr : h; }
    HANDLE handle_ = nullptr;
};

/**
 * @brief Owns a FindFirstFile handle and closes it with FindClose.
 */
class UniqueFindHandle {
public:
    UniqueFindHandle() = default;
    explicit UniqueFindHandle(HANDLE h) noexcept : handle_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~UniqueFindHandle() { reset(); }
    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
    UniqueFindHandle(UniqueFindHandle&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
    UniqueFindHandle& operator=(UniqueFindHandle&& o) noexcept {
        if (this != &o) { reset(); handle_ = std::exchange(o.handle_, nullptr); }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    void reset() noexcept { if (handle_) { ::FindClose(handle_); handle_ = nullptr; } }
private:
    HANDLE handle_ = nullptr;
};

/**
 * @brief Creates a manual- or auto-reset event and owns it.
 */
inline UniqueHandle makeEvent(bool manualReset, bool initialState = false) {
    return UniqueHandle(::CreateEventW(nullptr, manualReset ? TRUE : FALSE, initialState ? TRUE : FALSE, nullptr));
}

/**
 * @brief Scoped CoInitializeEx / CoUninitialize for the calling thread.
 */
class ScopedCoInit {
public:
    explicit ScopedCoInit(DWORD flags = COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE) noexcept
        : hr_(::CoInitializeEx(nullptr, flags)) {}
    ~ScopedCoInit() { if (SUCCEEDED(hr_)) { ::CoUninitialize(); } }
    ScopedCoInit(const ScopedCoInit&) = delete;
    ScopedCoInit& operator=(const ScopedCoInit&) = delete;
    /// True when COM is usable on this thread (S_OK, S_FALSE or RPC_E_CHANGED_MODE).
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
    [[nodiscard]] HRESULT hr() const noexcept { return hr_; }
private:
    HRESULT hr_;
};

/// Deleter for memory returned via CoTaskMemAlloc (e.g. SHGetKnownFolderPath).
struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { if (p) { ::CoTaskMemFree(p); } }
};
template <class T>
using CoTaskMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

} // namespace hh::platform
