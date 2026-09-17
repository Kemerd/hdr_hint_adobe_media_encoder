// ---------------------------------------------------------------------------
// SingleInstance.h - one HdrHint per user session, argv forwarding.
//
// The primary instance owns a named mutex and a message-only window with a
// fixed class name. A second launch finds that window and forwards its
// arguments via WM_COPYDATA, then exits.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Handle.h"
#include "platform/Win.h"

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

    /// Tries to become the primary. Returns true when this process owns the mutex.
    bool acquire();
    /// True after a successful acquire().
    [[nodiscard]] bool isPrimary() const noexcept { return primary_; }

    /**
     * @brief Creates the message-only window that receives forwarded argv.
     * @param onArgs called on this thread with the forwarded argument list.
     */
    bool registerReceiver(std::function<void(const std::vector<std::wstring>&)> onArgs);

    /// Sends argv to the primary (used by the secondary before exiting).
    static bool forwardToPrimary(const std::vector<std::wstring>& args);

    /// The message window (nullptr in secondaries / before registerReceiver).
    [[nodiscard]] HWND receiverWindow() const noexcept { return receiver_; }

private:
    static LRESULT CALLBACK receiverProc(HWND, UINT, WPARAM, LPARAM);

    UniqueHandle mutex_;
    HWND receiver_ = nullptr;
    bool primary_ = false;
    std::function<void(const std::vector<std::wstring>&)> onArgs_;
};

} // namespace hh::platform
