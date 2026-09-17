// ---------------------------------------------------------------------------
// DirectoryWatch.cpp - one overlapped ReadDirectoryChangesW session.
//
// Life cycle:
//   start()  -> open the directory, create the event, arm the first read
//   drain()  -> harvest a completed read (never blocks), then re-arm
//   stop()   -> cancel the outstanding read, wait for it to land, close
//
// The subtle part is stop(): the kernel may still touch buffer_ and
// overlapped_ until the cancelled I/O has actually completed, so both must
// outlive the CancelIoEx + GetOverlappedResult(bWait = TRUE) pair. Every
// other line in this file is bookkeeping around that one rule.
// ---------------------------------------------------------------------------
#include "platform/DirectoryWatch.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"DirectoryWatch";

/// Size of the change buffer handed to ReadDirectoryChangesW. 64 KiB is the
/// largest size that still works for network directories, so it doubles as
/// the ceiling for local ones; anything bigger buys nothing.
constexpr DWORD kBufferSize = 64u * 1024u;

/// How long stop() is willing to wait for a cancelled read to complete.
/// Cancellation normally lands in microseconds; the bound only exists so a
/// misbehaving filter driver can never hang the shutdown path forever.
constexpr DWORD kCancelDrainTimeoutMs = 10000;

/// Byte offsets of the FILE_NOTIFY_INFORMATION fields. The records are read
/// field-by-field with memcpy so no alignment assumption is ever made about
/// the raw bytes the kernel handed back.
constexpr size_t kOffNextEntry = offsetof(FILE_NOTIFY_INFORMATION, NextEntryOffset);
constexpr size_t kOffAction = offsetof(FILE_NOTIFY_INFORMATION, Action);
constexpr size_t kOffNameLength = offsetof(FILE_NOTIFY_INFORMATION, FileNameLength);
constexpr size_t kRecordHeaderSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);

/**
 * @brief Reads a DWORD at a byte offset without any alignment assumption.
 * @param base   start of the buffer (never null when called)
 * @param offset byte offset, already bounds-checked by the caller
 */
DWORD readDwordAt(const uint8_t* base, size_t offset) noexcept {
    DWORD value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

/**
 * @brief Walks the FILE_NOTIFY_INFORMATION chain and appends one
 *        DirectoryChange per record.
 *
 * Every field is bounds-checked against @p byteCount before it is read; a
 * record that runs past the end, or a NextEntryOffset that would not make
 * forward progress, aborts the walk.
 *
 * @param buffer    the buffer ReadDirectoryChangesW wrote into
 * @param byteCount bytes the kernel reported as valid
 * @param out       receives the parsed records (appended)
 * @return false when the chain was malformed (caller should force a rescan)
 */
bool parseRecords(const std::vector<uint8_t>& buffer, DWORD byteCount, std::vector<DirectoryChange>& out) {
    // Never trust the reported byte count beyond what the buffer can hold.
    const size_t total = std::min<size_t>(static_cast<size_t>(byteCount), buffer.size());
    const uint8_t* base = buffer.data();
    if (!base || total < kRecordHeaderSize) {
        return false;
    }

    size_t offset = 0;
    while (offset + kRecordHeaderSize <= total) {
        // Pull the three header fields out of the raw bytes.
        const DWORD nextEntry = readDwordAt(base, offset + kOffNextEntry);
        const DWORD action = readDwordAt(base, offset + kOffAction);
        const DWORD nameBytes = readDwordAt(base, offset + kOffNameLength);

        // The file name must fit entirely inside the valid region.
        const size_t nameStart = offset + kRecordHeaderSize;
        if (static_cast<size_t>(nameBytes) > total - nameStart) {
            HH_LOG_WARN(kLog, L"change record at offset {} claims {} name bytes but only {} remain",
                        offset, nameBytes, total - nameStart);
            return false;
        }

        // Copy the UTF-16 name out (FileNameLength is in bytes, not chars).
        const size_t nameChars = static_cast<size_t>(nameBytes) / sizeof(wchar_t);
        std::wstring name(nameChars, L'\0');
        if (nameChars > 0) {
            std::memcpy(name.data(), base + nameStart, nameChars * sizeof(wchar_t));
        }
        // Some file systems pad with a terminating NUL; strip it so callers
        // never see a name that compares unequal to the same name from FindFirstFile.
        while (!name.empty() && name.back() == L'\0') {
            name.pop_back();
        }
        if (!name.empty()) {
            out.push_back(DirectoryChange{action, std::move(name)});
        }

        // Advance to the next record; zero means this was the last one.
        if (nextEntry == 0) {
            break;
        }
        if (static_cast<size_t>(nextEntry) < kRecordHeaderSize) {
            HH_LOG_WARN(kLog, L"change record at offset {} has bogus NextEntryOffset {}", offset, nextEntry);
            return false;
        }
        offset += static_cast<size_t>(nextEntry);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

/**
 * @brief Constructs an inactive watch; nothing is opened until start().
 */
DirectoryWatch::DirectoryWatch() = default;

/**
 * @brief Cancels and drains any pending read before the members go away.
 */
DirectoryWatch::~DirectoryWatch() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

/**
 * @brief Opens the directory for change notification and issues the first
 *        overlapped read.
 *
 * On any failure the object is left inactive (active() == false) and
 * lastError() carries the Win32 code, mirroring what the returned Error says.
 */
Result<void> DirectoryWatch::start(std::wstring_view directory, bool watchSubtree, DWORD filter) {
    // A second start() on a live watch would leak the pending read, so tear
    // the previous session down first.
    if (dirHandle_) {
        HH_LOG_DEBUG(kLog, L"start() called while already watching {}; restarting", directory_);
        stop();
    }
    lastError_ = 0;

    // Validate the inputs before touching the kernel.
    if (directory.empty()) {
        lastError_ = ERROR_INVALID_PARAMETER;
        HH_LOG_ERROR(kLog, L"start(): empty directory");
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"DirectoryWatch::start: empty directory");
    }
    if (filter == 0) {
        lastError_ = ERROR_INVALID_PARAMETER;
        HH_LOG_ERROR(kLog, L"start(): notify filter is zero for {}", directory);
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"DirectoryWatch::start: empty notify filter");
    }

    // Open with backup semantics (required for directories) and overlapped
    // I/O so the read can be cancelled from stop(). The \\?\ prefix keeps
    // very long render paths working.
    const std::wstring extended = toExtendedPath(directory);
    const std::wstring openPath = extended.empty() ? std::wstring(directory) : extended;
    HANDLE raw = ::CreateFileW(openPath.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        lastError_ = err;
        HH_LOG_WARN(kLog, L"CreateFileW({}) failed: {} ({})", directory, win32ErrorText(err), err);
        return Error::fromWin32(err, L"DirectoryWatch: open " + std::wstring(directory));
    }
    dirHandle_.reset(raw);

    // Manual-reset event: it stays signalled until drain() harvests the
    // completion, so a caller that wakes late never misses it.
    event_ = makeEvent(true, false);
    if (!event_) {
        const DWORD err = ::GetLastError();
        lastError_ = err;
        dirHandle_.reset();
        HH_LOG_ERROR(kLog, L"CreateEventW failed for {}: {} ({})", directory, win32ErrorText(err), err);
        return Error::fromWin32(err, L"DirectoryWatch: create event");
    }

    // Remember the configuration for re-arming and allocate the buffer once.
    buffer_.assign(kBufferSize, 0);
    directory_ = std::wstring(directory);
    watchSubtree_ = watchSubtree;
    filter_ = filter;
    pending_ = false;

    // Issue the first read. arm() records the error code on failure.
    if (!arm()) {
        const DWORD err = lastError_;
        stop();
        lastError_ = err;
        HH_LOG_WARN(kLog, L"ReadDirectoryChangesW failed for {}: {} ({})", directory_, win32ErrorText(err), err);
        return Error::fromWin32(err, L"DirectoryWatch: ReadDirectoryChangesW " + directory_);
    }

    HH_LOG_DEBUG(kLog, L"watching {} (subtree={}, filter={:#x})", directory_, watchSubtree_, filter_);
    return {};
}

/**
 * @brief Cancels the pending read, waits for the cancellation to land and
 *        closes everything.
 *
 * Safe to call repeatedly and on a never-started object. lastError_ is
 * deliberately left untouched so callers can still read why a watch died.
 */
void DirectoryWatch::stop() {
    if (dirHandle_) {
        if (pending_) {
            // Ask the kernel to abandon the read. ERROR_NOT_FOUND just means
            // it had already completed, which is fine - the drain below
            // collects that completion too.
            if (!::CancelIoEx(dirHandle_.get(), &overlapped_)) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_NOT_FOUND) {
                    HH_LOG_DEBUG(kLog, L"CancelIoEx on {} failed: {} ({})", directory_, win32ErrorText(err), err);
                }
            }

            // Wait (bounded) for the completion so the buffer is ours again,
            // then collect the status; ERROR_OPERATION_ABORTED is the
            // expected outcome and not worth a log line.
            bool drained = false;
            if (event_) {
                const DWORD wait = ::WaitForSingleObject(event_.get(), kCancelDrainTimeoutMs);
                drained = (wait == WAIT_OBJECT_0);
            }
            if (drained) {
                DWORD bytes = 0;
                if (!::GetOverlappedResult(dirHandle_.get(), &overlapped_, &bytes, TRUE)) {
                    const DWORD err = ::GetLastError();
                    if (err != ERROR_OPERATION_ABORTED) {
                        HH_LOG_DEBUG(kLog, L"drain of cancelled read on {} ended with {} ({})", directory_,
                                     win32ErrorText(err), err);
                    }
                }
            } else {
                // The read refused to complete. The kernel may still own the
                // buffer, so hand it to the heap forever rather than free
                // memory that could be written later. This path should never
                // execute; it exists purely so that it can never corrupt.
                HH_LOG_ERROR(kLog, L"cancelled read on {} did not complete within {} ms; orphaning its buffer",
                             directory_, kCancelDrainTimeoutMs);
                auto* orphan = new std::vector<uint8_t>(std::move(buffer_));
                static_cast<void>(orphan);
                buffer_.clear();
            }
            pending_ = false;
        }
        dirHandle_.reset();
        HH_LOG_DEBUG(kLog, L"stopped watching {}", directory_);
    }

    // Release the remaining resources; the directory name is kept so log
    // lines after stop() still say which folder this was.
    pending_ = false;
    event_.reset();
    buffer_.clear();
    buffer_.shrink_to_fit();
    overlapped_ = OVERLAPPED{};
}

// ---------------------------------------------------------------------------
// drain / arm
// ---------------------------------------------------------------------------

/**
 * @brief Harvests a completed read without blocking, then re-arms.
 *
 * Returns an empty vector when nothing has completed yet. When the OS
 * reports an overflow (zero bytes or ERROR_NOTIFY_ENUM_DIR) @p overflowed is
 * set so the caller rescans the folder. A fatal completion (volume gone,
 * access revoked) closes the watch and records lastError().
 */
std::vector<DirectoryChange> DirectoryWatch::drain(bool& overflowed) {
    overflowed = false;
    std::vector<DirectoryChange> changes;

    // Nothing in flight means nothing to collect.
    if (!pending_ || !dirHandle_ || !event_) {
        return changes;
    }

    // Non-blocking poll of the outstanding read.
    DWORD bytes = 0;
    if (!::GetOverlappedResult(dirHandle_.get(), &overlapped_, &bytes, FALSE)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            // Spurious wake-up: the read is still in flight.
            return changes;
        }

        // The read has completed with an error, so the buffer is ours again.
        pending_ = false;
        if (err == ERROR_NOTIFY_ENUM_DIR) {
            // Too many changes for the buffer: the OS dropped them.
            overflowed = true;
            HH_LOG_WARN(kLog, L"change buffer for {} overflowed; caller must rescan", directory_);
            ::ResetEvent(event_.get());
            if (!arm()) {
                HH_LOG_WARN(kLog, L"re-arm after overflow failed for {}: {} ({})", directory_,
                            win32ErrorText(lastError_), lastError_);
                stop();
            }
            return changes;
        }

        // Anything else is fatal for this watch (typically the directory or
        // volume disappeared). Close so active() reports false.
        lastError_ = err;
        HH_LOG_WARN(kLog, L"watch on {} failed: {} ({})", directory_, win32ErrorText(err), err);
        stop();
        return changes;
    }

    // Successful completion: the buffer holds `bytes` valid bytes.
    pending_ = false;
    if (bytes == 0) {
        // Zero bytes is the documented "buffer too small" signal.
        overflowed = true;
        HH_LOG_WARN(kLog, L"zero-length notification for {} (overflow); caller must rescan", directory_);
    } else if (!parseRecords(buffer_, bytes, changes)) {
        // A malformed chain is treated exactly like an overflow: the caller
        // rescans and nothing is silently lost.
        overflowed = true;
    }

    // Re-arm for the next batch. On failure the watch is closed and
    // lastError() explains why.
    ::ResetEvent(event_.get());
    if (!arm()) {
        HH_LOG_WARN(kLog, L"re-arm failed for {}: {} ({})", directory_, win32ErrorText(lastError_), lastError_);
        stop();
    }
    return changes;
}

/**
 * @brief Issues one overlapped ReadDirectoryChangesW into buffer_.
 * @return false on failure (lastError_ holds the code, pending_ is false)
 */
bool DirectoryWatch::arm() {
    // Guard every precondition: the handle, the event and the buffer.
    if (!dirHandle_ || !event_) {
        lastError_ = ERROR_INVALID_HANDLE;
        return false;
    }
    if (pending_) {
        // Never double-issue; the previous read still owns the buffer.
        return true;
    }
    if (buffer_.size() < kBufferSize) {
        buffer_.assign(kBufferSize, 0);
    }

    // Fresh OVERLAPPED for every read; only the event carries over.
    overlapped_ = OVERLAPPED{};
    overlapped_.hEvent = event_.get();

    // With an OVERLAPPED the call returns as soon as the read is queued.
    const BOOL ok = ::ReadDirectoryChangesW(dirHandle_.get(), buffer_.data(), static_cast<DWORD>(buffer_.size()),
                                            watchSubtree_ ? TRUE : FALSE, filter_, nullptr, &overlapped_, nullptr);
    if (!ok) {
        lastError_ = ::GetLastError();
        pending_ = false;
        return false;
    }
    pending_ = true;
    return true;
}

} // namespace hh::platform
