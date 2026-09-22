// ---------------------------------------------------------------------------
// SingleInstance.h - one HdrHint per user session, argv forwarding.
//
// Windows: the primary instance owns a named mutex and a message-only window
// with a fixed class name. A second launch finds that window and forwards
// its arguments via WM_COPYDATA, then exits.
//
// macOS: the primary holds an exclusive flock() on a lock file in the app's
// support folder and listens on a Unix socket next to it. A second launch
// connects, writes the same {"argv": [...]} document, waits for a one-byte
// acknowledgement and exits. The socket is serviced by a dispatch source on
// the main queue, so onArgs runs on the UI thread exactly as on Windows.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#if defined(_WIN32)
#include "platform/Handle.h"
#endif

#include <functional>
#include <string>
#include <vector>

namespace hh::platform {

class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /// Tries to become the primary. Returns true when this process owns the instance lock.
    bool acquire();
    /// True after a successful acquire().
    [[nodiscard]] bool isPrimary() const noexcept { return primary_; }

    /**
     * @brief Starts receiving forwarded argv.
     * @param onArgs called on the UI thread with the forwarded argument list.
     */
    bool registerReceiver(std::function<void(const std::vector<std::wstring>&)> onArgs);

    /// Sends argv to the primary (used by the secondary before exiting).
    static bool forwardToPrimary(const std::vector<std::wstring>& args);

#if defined(_WIN32)
    /// The message window (nullptr in secondaries / before registerReceiver).
    [[nodiscard]] HWND receiverWindow() const noexcept { return receiver_; }
#endif

private:
#if defined(_WIN32)
    static LRESULT CALLBACK receiverProc(HWND, UINT, WPARAM, LPARAM);

    UniqueHandle mutex_;
    HWND receiver_ = nullptr;
#else
    /// Accepts and services one forwarding client (runs on the main queue).
    static void onReadable(void* context);

    int lockFd_ = -1;          ///< flock()ed lock file (primary only)
    int listenFd_ = -1;        ///< listening Unix socket (primary only)
    void* source_ = nullptr;   ///< dispatch_source_t reading listenFd_
    std::string socketPath_;   ///< unlinked again on destruction
#endif
    bool primary_ = false;
    std::function<void(const std::vector<std::wstring>&)> onArgs_;
};

} // namespace hh::platform
