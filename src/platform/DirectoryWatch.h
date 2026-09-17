// ---------------------------------------------------------------------------
// DirectoryWatch.h - one overlapped ReadDirectoryChangesW session.
//
// Usage from a worker thread:
//     DirectoryWatch w;
//     w.start(dir, false, FILE_NOTIFY_CHANGE_FILE_NAME | ...);
//     WaitForMultipleObjects({..., w.event()}, ...);
//     auto changes = w.drain();   // re-arms automatically
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Handle.h"
#include "platform/Win.h"

#include <string>
#include <vector>

namespace hh::platform {

/**
 * @brief A single change record from ReadDirectoryChangesW.
 */
struct DirectoryChange {
    DWORD action = 0;        ///< FILE_ACTION_ADDED / MODIFIED / REMOVED / RENAMED_OLD_NAME / RENAMED_NEW_NAME
    std::wstring name;       ///< file name relative to the watched directory
};

/**
 * @brief Watches one directory with overlapped I/O and a manual-reset event.
 *
 * The buffer stays owned by this object until the pending I/O has been
 * cancelled *and* drained, which is what makes stop() safe.
 */
class DirectoryWatch {
public:
    DirectoryWatch();
    ~DirectoryWatch();
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;

    /// Opens the directory and issues the first read. @p filter = FILE_NOTIFY_CHANGE_*.
    Result<void> start(std::wstring_view directory, bool watchSubtree, DWORD filter);
    /// Cancels the pending I/O, waits for it to drain and closes the handle.
    void stop();

    /// True after a successful start() until stop().
    [[nodiscard]] bool active() const noexcept { return dirHandle_.valid(); }
    /// Event signalled when a read completes (or fails). Never null after start().
    [[nodiscard]] HANDLE event() const noexcept { return event_.get(); }
    /// The watched directory.
    [[nodiscard]] const std::wstring& directory() const noexcept { return directory_; }

    /**
     * @brief Collects the completed changes and re-arms the watch.
     *
     * Sets @p overflowed when the OS dropped notifications (the caller must
     * rescan the directory). After a fatal error active() becomes false and
     * lastError() explains why (e.g. the volume disappeared).
     */
    std::vector<DirectoryChange> drain(bool& overflowed);

    /// Last fatal error (win32 code) or 0.
    [[nodiscard]] DWORD lastError() const noexcept { return lastError_; }

private:
    bool arm();

    std::wstring directory_;
    UniqueHandle dirHandle_;
    UniqueHandle event_;
    OVERLAPPED overlapped_{};
    std::vector<uint8_t> buffer_;
    bool watchSubtree_ = false;
    DWORD filter_ = 0;
    bool pending_ = false;
    DWORD lastError_ = 0;
};

} // namespace hh::platform
