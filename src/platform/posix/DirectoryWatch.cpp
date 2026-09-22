// ---------------------------------------------------------------------------
// posix/DirectoryWatch.cpp - DirectoryWatch on macOS via FSEvents.
//
// Why a snapshot
//   FSEvents (file-level mode) reports *cumulative* flags per path: a file
//   created and then appended to keeps saying "created" in later batches,
//   renames report both halves as "renamed", and bursts are coalesced. The
//   engine's watchers were written against ReadDirectoryChangesW, where
//   ADDED / MODIFIED / REMOVED are precise. So FSEvents is used only as a
//   "something changed at these paths" signal; each reported name is then
//   reconciled against the last known state of the directory:
//
//     not known, exists now         -> FILE_ACTION_ADDED
//     known, different inode        -> FILE_ACTION_ADDED   (replaced)
//     known, size or mtime changed  -> FILE_ACTION_MODIFIED
//     known, gone now               -> FILE_ACTION_REMOVED
//
//   Dropped events (MustScanSubDirs / UserDropped / KernelDropped) trigger a
//   full reconcile *and* report an overflow, so the caller rescans exactly
//   as it would after ERROR_NOTIFY_ENUM_DIR on Windows.
//
// Threading
//   The stream delivers on a private serial dispatch queue. That callback
//   and drain() (worker thread) share one mutex; the manual-reset Event is
//   set while changes are waiting and reset by drain(). stop() stops and
//   invalidates the stream and then flushes the queue, so no callback can
//   ever run against a destroyed Impl.
// ---------------------------------------------------------------------------
#include "platform/DirectoryWatch.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"DirectoryWatch";

/// FSEvents coalescing window. Short: the engine's watchers coalesce too.
constexpr CFTimeInterval kLatencySeconds = 0.05;

/// Upper bound on the snapshot so a huge tree can never exhaust memory.
constexpr size_t kMaxSnapshotEntries = 200'000;

/**
 * @brief The canonical spelling of a directory (real case, symlinks resolved).
 *
 * FSEvents reports canonical paths (/private/var/..., the on-disk case), so
 * the prefix match below has to start from the same spelling. F_GETPATH on
 * an open descriptor is the kernel's own answer; realpath() is the fallback.
 */
std::string canonicalDirectory(const std::string& native) {
    int fd = ::open(native.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd >= 0) {
        char buffer[MAXPATHLEN] = {};
        const int r = ::fcntl(fd, F_GETPATH, buffer);
        ::close(fd);
        if (r != -1 && buffer[0] != '\0') {
            return buffer;
        }
    }
    char resolved[PATH_MAX] = {};
    if (::realpath(native.c_str(), resolved) != nullptr) {
        return resolved;
    }
    return native;
}

} // namespace

// ===========================================================================
// Impl
// ===========================================================================

/**
 * @brief Everything the FSEvents callback and drain() share.
 */
struct DirectoryWatch::Impl {
    /// Last known state of one entry, for reconciliation.
    struct Entry {
        uint64_t size = 0;
        int64_t mtimeNs = 0;
        uint64_t inode = 0;
        bool isDirectory = false;
    };

    std::mutex mutex;                               ///< guards everything below except the stream/queue
    std::string root;                               ///< canonical directory, no trailing slash
    bool subtree = false;                           ///< watch nested folders too
    DWORD filter = 0;                               ///< FILE_NOTIFY_CHANGE_* the caller asked for
    Event signal;                                   ///< manual-reset: set while there is something to drain
    std::unordered_map<std::string, Entry> snapshot;///< relative path -> last known state
    std::vector<DirectoryChange> pending;           ///< reconciled changes not yet drained
    bool overflow = false;                          ///< events were dropped since the last drain
    std::atomic<bool> fatal{false};                 ///< the directory went away
    DWORD fatalError = 0;                           ///< why (Win32-numbered)

    FSEventStreamRef stream = nullptr;              ///< owned; released in stop()
    dispatch_queue_t queue = nullptr;               ///< owned; released in stop()

    // ---- reconciliation (mutex held) ----------------------------------------

    /// Stats one entry without following a symlink (the link is the entry).
    static bool statEntry(const std::string& full, Entry& out) {
        struct stat st {};
        if (::lstat(full.c_str(), &st) != 0) {
            return false;
        }
        out.size = static_cast<uint64_t>(st.st_size < 0 ? 0 : st.st_size);
        out.mtimeNs = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000ll + st.st_mtimespec.tv_nsec;
        out.inode = static_cast<uint64_t>(st.st_ino);
        out.isDirectory = S_ISDIR(st.st_mode);
        return true;
    }

    /// True when the caller's filter wants add/remove records for this kind of entry.
    bool wantsNameChanges(bool isDirectory) const noexcept {
        return isDirectory ? (filter & FILE_NOTIFY_CHANGE_DIR_NAME) != 0 : (filter & FILE_NOTIFY_CHANGE_FILE_NAME) != 0;
    }

    /// True when the caller's filter wants content-change records.
    bool wantsModifications() const noexcept {
        return (filter & (FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES)) != 0;
    }

    void push(DWORD action, const std::string& relative) {
        pending.push_back(DirectoryChange{action, posix::fromNative(relative)});
    }

    /**
     * @brief Compares one name against the snapshot and records the difference.
     */
    void reconcile(const std::string& relative) {
        if (relative.empty()) {
            return;
        }
        Entry now;
        const bool present = statEntry(root + "/" + relative, now);
        const auto it = snapshot.find(relative);
        if (present) {
            if (it == snapshot.end()) {
                if (snapshot.size() < kMaxSnapshotEntries) {
                    snapshot.emplace(relative, now);
                }
                if (wantsNameChanges(now.isDirectory)) {
                    push(FILE_ACTION_ADDED, relative);
                }
                return;
            }
            Entry& known = it->second;
            if (known.inode != now.inode) {
                // Same name, different file: something was renamed or copied over it.
                known = now;
                if (wantsNameChanges(now.isDirectory)) {
                    push(FILE_ACTION_ADDED, relative);
                }
                return;
            }
            if (known.size != now.size || known.mtimeNs != now.mtimeNs) {
                known = now;
                if (!now.isDirectory && wantsModifications()) {
                    push(FILE_ACTION_MODIFIED, relative);
                }
            }
            return;
        }
        if (it != snapshot.end()) {
            const bool wasDirectory = it->second.isDirectory;
            snapshot.erase(it);
            if (wantsNameChanges(wasDirectory)) {
                push(FILE_ACTION_REMOVED, relative);
            }
        }
        // Created and deleted between two batches: nothing observable happened.
    }

    /**
     * @brief Lists the directory (recursively when watching a subtree).
     * @param into  receives relative names
     * @return false when the root itself cannot be read
     */
    bool listNames(std::vector<std::string>& into) const {
        std::vector<std::string> stack{std::string()};
        bool rootOk = false;
        while (!stack.empty() && into.size() < kMaxSnapshotEntries) {
            const std::string rel = std::move(stack.back());
            stack.pop_back();
            const std::string dirPath = rel.empty() ? root : root + "/" + rel;
            DIR* d = ::opendir(dirPath.c_str());
            if (d == nullptr) {
                if (rel.empty()) {
                    return false;
                }
                continue;   // a nested folder vanished mid-walk
            }
            if (rel.empty()) {
                rootOk = true;
            }
            for (const dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
                const char* name = e->d_name;
                if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                    continue;
                }
                std::string child = rel.empty() ? std::string(name) : rel + "/" + name;
                if (subtree && e->d_type == DT_DIR) {
                    stack.push_back(child);
                }
                into.push_back(std::move(child));
                if (into.size() >= kMaxSnapshotEntries) {
                    break;
                }
            }
            ::closedir(d);
        }
        return rootOk;
    }

    /// Fills the snapshot from a fresh listing without reporting anything.
    bool seed() {
        std::vector<std::string> names;
        if (!listNames(names)) {
            return false;
        }
        snapshot.clear();
        snapshot.reserve(names.size());
        for (const std::string& rel : names) {
            Entry e;
            if (statEntry(root + "/" + rel, e)) {
                snapshot.emplace(rel, e);
            }
        }
        return true;
    }

    /// Reconciles every name on disk and every name in the snapshot.
    void reconcileAll() {
        std::vector<std::string> names;
        if (!listNames(names)) {
            return;   // the root is gone; RootChanged reports that separately
        }
        std::unordered_set<std::string> seen(names.begin(), names.end());
        for (const std::string& rel : names) {
            reconcile(rel);
        }
        std::vector<std::string> vanished;
        for (const auto& [rel, entry] : snapshot) {
            static_cast<void>(entry);
            if (seen.find(rel) == seen.end()) {
                vanished.push_back(rel);
            }
        }
        for (const std::string& rel : vanished) {
            reconcile(rel);
        }
    }

    /**
     * @brief Maps an absolute event path to a name relative to the root.
     * @return false for paths outside the root (or too deep without subtree)
     */
    bool relativeName(const char* path, std::string& out, bool& isRoot) const {
        isRoot = false;
        if (path == nullptr) {
            return false;
        }
        const size_t len = std::strlen(path);
        // Case-insensitive prefix match: APFS is case-insensitive by default.
        if (len < root.size() || ::strncasecmp(path, root.c_str(), root.size()) != 0) {
            return false;
        }
        if (len == root.size() || (len == root.size() + 1 && path[root.size()] == '/')) {
            isRoot = true;
            return true;
        }
        if (path[root.size()] != '/') {
            return false;   // "/renders2" is not inside "/renders"
        }
        std::string rel(path + root.size() + 1);
        while (!rel.empty() && rel.back() == '/') {
            rel.pop_back();
        }
        if (rel.empty()) {
            isRoot = true;
            return true;
        }
        if (!subtree && rel.find('/') != std::string::npos) {
            return false;   // deeper than we watch
        }
        out = std::move(rel);
        return true;
    }

    // ---- the FSEvents callback -----------------------------------------------

    static void callback(ConstFSEventStreamRef, void* info, size_t count, void* eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<Impl*>(info);
        if (self == nullptr || eventPaths == nullptr || flags == nullptr) {
            return;
        }
        char** paths = static_cast<char**>(eventPaths);

        std::lock_guard<std::mutex> lock(self->mutex);
        const size_t before = self->pending.size();
        bool rescan = false;
        std::unordered_set<std::string> dirty;

        for (size_t i = 0; i < count; ++i) {
            const FSEventStreamEventFlags f = flags[i];

            // Dropped history: reconcile everything and tell the caller.
            if ((f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                      kFSEventStreamEventFlagKernelDropped)) != 0) {
                rescan = true;
                self->overflow = true;
            }
            // The watched folder itself was moved / deleted, or its volume went away.
            if ((f & kFSEventStreamEventFlagRootChanged) != 0) {
                struct stat st {};
                if (::stat(self->root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                    self->fatalError = ERROR_PATH_NOT_FOUND;
                    self->fatal.store(true);
                }
                continue;
            }
            if ((f & kFSEventStreamEventFlagUnmount) != 0) {
                self->fatalError = ERROR_NOT_READY;
                self->fatal.store(true);
                continue;
            }
            if ((f & kFSEventStreamEventFlagHistoryDone) != 0) {
                continue;
            }

            std::string rel;
            bool isRoot = false;
            if (!self->relativeName(paths[i], rel, isRoot)) {
                continue;
            }
            if (isRoot) {
                rescan = true;   // directory-level event: look at everything
            } else {
                dirty.insert(std::move(rel));
            }
        }

        if (rescan) {
            self->reconcileAll();
        } else {
            for (const std::string& rel : dirty) {
                self->reconcile(rel);
            }
        }

        // Wake the worker when there is anything to report.
        if (self->pending.size() != before || self->overflow || self->fatal.load()) {
            self->signal.set();
        }
    }
};

// ===========================================================================
// DirectoryWatch
// ===========================================================================

DirectoryWatch::DirectoryWatch() = default;

DirectoryWatch::~DirectoryWatch() {
    stop();
}

/**
 * @brief Snapshots the directory, then starts an FSEvents stream on it.
 */
Result<void> DirectoryWatch::start(std::wstring_view directory, bool watchSubtree, DWORD filter) {
    if (impl_) {
        HH_LOG_DEBUG(kLog, L"start() called while already watching {}; restarting", directory_);
        stop();
    }
    lastError_ = 0;

    // Validate the inputs before touching the OS.
    if (directory.empty()) {
        lastError_ = ERROR_INVALID_PARAMETER;
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"DirectoryWatch::start: empty directory");
    }
    if (filter == 0) {
        lastError_ = ERROR_INVALID_PARAMETER;
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"DirectoryWatch::start: empty notify filter");
    }
    const std::string native = posix::toNative(fullPath(directory));
    struct stat st {};
    if (native.empty() || ::stat(native.c_str(), &st) != 0) {
        const int err = native.empty() ? EINVAL : errno;
        lastError_ = win32FromErrno(err);
        return Error::fromErrno(err, L"DirectoryWatch: open " + std::wstring(directory));
    }
    if (!S_ISDIR(st.st_mode)) {
        lastError_ = ERROR_DIRECTORY;
        return Error::fromWin32(ERROR_DIRECTORY, L"DirectoryWatch: not a directory: " + std::wstring(directory));
    }

    auto impl = std::make_unique<Impl>();
    impl->root = canonicalDirectory(native);
    while (impl->root.size() > 1 && impl->root.back() == '/') {
        impl->root.pop_back();
    }
    impl->subtree = watchSubtree;
    impl->filter = filter;
    if (!impl->signal.create(true, false)) {
        lastError_ = win32FromErrno(errno);
        return Error::fromErrno(errno, L"DirectoryWatch: create event");
    }

    // Know what is there before the first event can arrive.
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->seed()) {
            const int err = errno;
            lastError_ = win32FromErrno(err);
            return Error::fromErrno(err, L"DirectoryWatch: list " + std::wstring(directory));
        }
    }

    // One stream on the canonical path, file-level events, no deferral.
    CFStringRef cfPath = ::CFStringCreateWithFileSystemRepresentation(kCFAllocatorDefault, impl->root.c_str());
    if (cfPath == nullptr) {
        lastError_ = ERROR_INVALID_NAME;
        return Error::fromWin32(ERROR_INVALID_NAME, L"DirectoryWatch: bad path " + std::wstring(directory));
    }
    const void* values[1] = {cfPath};
    CFArrayRef paths = ::CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
    ::CFRelease(cfPath);
    if (paths == nullptr) {
        lastError_ = ERROR_NOT_ENOUGH_MEMORY;
        return Error::fromWin32(ERROR_NOT_ENOUGH_MEMORY, L"DirectoryWatch: CFArrayCreate");
    }

    FSEventStreamContext context{};
    context.info = impl.get();
    impl->stream = ::FSEventStreamCreate(kCFAllocatorDefault, &Impl::callback, &context, paths,
                                         kFSEventStreamEventIdSinceNow, kLatencySeconds,
                                         kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                             kFSEventStreamCreateFlagWatchRoot);
    ::CFRelease(paths);
    if (impl->stream == nullptr) {
        lastError_ = ERROR_NOT_SUPPORTED;
        return Error::fromWin32(ERROR_NOT_SUPPORTED, L"DirectoryWatch: FSEventStreamCreate " + std::wstring(directory));
    }

    impl->queue = ::dispatch_queue_create("com.everett.hdrhint.directorywatch", DISPATCH_QUEUE_SERIAL);
    if (impl->queue == nullptr) {
        ::FSEventStreamRelease(impl->stream);
        impl->stream = nullptr;
        lastError_ = ERROR_NOT_ENOUGH_MEMORY;
        return Error::fromWin32(ERROR_NOT_ENOUGH_MEMORY, L"DirectoryWatch: dispatch_queue_create");
    }
    ::FSEventStreamSetDispatchQueue(impl->stream, impl->queue);
    if (!::FSEventStreamStart(impl->stream)) {
        ::FSEventStreamInvalidate(impl->stream);
        ::FSEventStreamRelease(impl->stream);
        impl->stream = nullptr;
        ::dispatch_release(impl->queue);
        impl->queue = nullptr;
        lastError_ = ERROR_NOT_SUPPORTED;
        return Error::fromWin32(ERROR_NOT_SUPPORTED, L"DirectoryWatch: FSEventStreamStart " + std::wstring(directory));
    }

    directory_ = std::wstring(directory);
    watchSubtree_ = watchSubtree;
    filter_ = filter;
    impl_ = std::move(impl);
    HH_LOG_DEBUG(kLog, L"watching {} (subtree={}, filter={:#x})", directory_, watchSubtree_, filter_);
    return {};
}

/**
 * @brief Stops the stream and drains its queue before freeing the Impl.
 */
void DirectoryWatch::stop() {
    if (!impl_) {
        return;
    }
    if (impl_->stream != nullptr) {
        ::FSEventStreamStop(impl_->stream);
        ::FSEventStreamInvalidate(impl_->stream);
        ::FSEventStreamRelease(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->queue != nullptr) {
        // A callback may already be running; wait for the queue to go idle.
        ::dispatch_sync_f(impl_->queue, nullptr, [](void*) {});
        ::dispatch_release(impl_->queue);
        impl_->queue = nullptr;
    }
    impl_.reset();
    HH_LOG_DEBUG(kLog, L"stopped watching {}", directory_);
}

bool DirectoryWatch::active() const noexcept {
    return impl_ != nullptr && !impl_->fatal.load();
}

WaitHandle DirectoryWatch::event() const noexcept {
    return impl_ ? impl_->signal.handle() : kInvalidWaitHandle;
}

/**
 * @brief Hands out the reconciled changes and clears the signal.
 */
std::vector<DirectoryChange> DirectoryWatch::drain(bool& overflowed) {
    overflowed = false;
    std::vector<DirectoryChange> changes;
    if (!impl_) {
        return changes;
    }
    bool fatal = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        changes.swap(impl_->pending);
        overflowed = impl_->overflow;
        impl_->overflow = false;
        fatal = impl_->fatal.load();
        if (fatal) {
            lastError_ = impl_->fatalError != 0 ? impl_->fatalError : ERROR_PATH_NOT_FOUND;
        }
        impl_->signal.reset();
    }
    if (overflowed) {
        HH_LOG_WARN(kLog, L"events for {} were dropped; caller must rescan", directory_);
    }
    if (fatal) {
        HH_LOG_WARN(kLog, L"watch on {} failed: {} ({})", directory_, win32ErrorText(lastError_), lastError_);
        stop();
    }
    return changes;
}

} // namespace hh::platform
