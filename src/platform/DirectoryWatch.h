// ---------------------------------------------------------------------------
// DirectoryWatch.h - one directory change-notification session.
//
// Usage from a worker thread:
//     DirectoryWatch w;
//     w.start(dir, false, FILE_NOTIFY_CHANGE_FILE_NAME | ...);
//     platform::waitAny({..., w.event()}, ...);
//     auto changes = w.drain();   // re-arms automatically
//
// Windows  one overlapped ReadDirectoryChangesW read, re-armed by drain().
// macOS    one FSEvents stream (file-level events) on a private dispatch
//          queue. Changes are reconciled against a snapshot of the folder
//          so the reported FILE_ACTION_* values mean exactly what they mean
//          on Windows (FSEvents' own flags are cumulative and would report
//          "created" for every later modification of a new file).
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Event.h"
#include "platform/Win.h"

#if defined(_WIN32)
#include "platform/Handle.h"
#endif

#include <memory>
#include <string>
#include <vector>

namespace hh::platform {

/**
 * @brief A single change record (FILE_ACTION_* semantics on every platform).
 */
struct DirectoryChange {
    DWORD action = 0;        ///< FILE_ACTION_ADDED / MODIFIED / REMOVED / RENAMED_OLD_NAME / RENAMED_NEW_NAME
    std::wstring name;       ///< file name relative to the watched directory
};

/**
 * @brief Watches one directory and signals a waitable handle on changes.
 *
 * Windows: the buffer stays owned by this object until the pending I/O has
 * been cancelled *and* drained, which is what makes stop() safe.
 * macOS: the FSEvents stream is invalidated and released on its own queue
 * before stop() returns, so no callback can run against a dead object.
 */
class DirectoryWatch {
public:
    DirectoryWatch();
    ~DirectoryWatch();
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;

    /// Opens the directory and starts watching. @p filter = FILE_NOTIFY_CHANGE_*.
    Result<void> start(std::wstring_view directory, bool watchSubtree, DWORD filter);
    /// Stops watching and releases every OS resource.
    void stop();

    /// True after a successful start() until stop() (or a fatal error).
    [[nodiscard]] bool active() const noexcept;
    /// Handle signalled when changes are waiting (or the watch failed). Valid after start().
    [[nodiscard]] WaitHandle event() const noexcept;
    /// The watched directory.
    [[nodiscard]] const std::wstring& directory() const noexcept { return directory_; }

    /**
     * @brief Collects the pending changes and re-arms the watch.
     *
     * Sets @p overflowed when the OS dropped notifications (the caller must
     * rescan the directory). After a fatal error active() becomes false and
     * lastError() explains why (e.g. the volume disappeared).
     */
    std::vector<DirectoryChange> drain(bool& overflowed);

    /// Last fatal error (Win32-numbered code) or 0.
    [[nodiscard]] DWORD lastError() const noexcept { return lastError_; }

private:
    std::wstring directory_;
    bool watchSubtree_ = false;
    DWORD filter_ = 0;
    DWORD lastError_ = 0;

#if defined(_WIN32)
    bool arm();

    UniqueHandle dirHandle_;
    UniqueHandle event_;
    OVERLAPPED overlapped_{};
    std::vector<uint8_t> buffer_;
    bool pending_ = false;
#else
    /// FSEvents stream, queue, snapshot and pending changes (DirectoryWatch.mm).
    struct Impl;
    std::unique_ptr<Impl> impl_;
#endif
};

} // namespace hh::platform
