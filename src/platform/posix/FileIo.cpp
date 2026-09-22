// ---------------------------------------------------------------------------
// posix/FileIo.cpp - the FileIo.h contract on POSIX (macOS first).
//
// Semantics mirror the Windows implementation line for line:
//   * fullPath() is lexical, like GetFullPathNameW (no symlink resolution);
//   * writeAllAtomic() writes a sibling temp file, flushes it to the platter
//     (F_FULLFSYNC on macOS, the equivalent of FILE_FLAG_WRITE_THROUGH) and
//     renames it over the target;
//   * moves fall back to copy + delete across volumes (MOVEFILE_COPY_ALLOWED);
//   * probeDenyWrite() answers "is a writer holding this file?" by asking
//     the kernel for every descriptor open on the file (libproc) instead of
//     relying on Win32 share modes, which POSIX does not have.
// ---------------------------------------------------------------------------
#include "platform/FileIo.h"

#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <copyfile.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <stdio.h>          // renamex_np / RENAME_EXCL
#endif

namespace hh::platform {

using posix::closeQuietly;
using posix::creationTicks;
using posix::errnoError;
using posix::fromNative;
using posix::lastWriteTicks;
using posix::toNative;

namespace {

/// Largest single read/write we issue (keeps every size comfortably in range).
constexpr size_t kChunk = 1u << 20;

/// The "opened for writing" bit in proc_fileinfo::fi_openflags (FWRITE).
#if defined(FWRITE)
constexpr uint32_t kOpenForWrite = FWRITE;
#else
constexpr uint32_t kOpenForWrite = 0x0002u;
#endif

/**
 * @brief stat() that retries on EINTR. Follows symlinks like the Win32 calls do for targets.
 */
bool statPath(const std::string& native, struct stat& st) noexcept {
    if (native.empty()) {
        errno = EINVAL;
        return false;
    }
    int r = 0;
    do {
        r = ::stat(native.c_str(), &st);
    } while (r != 0 && errno == EINTR);
    return r == 0;
}

/**
 * @brief open() with O_CLOEXEC that retries on EINTR.
 */
int openRetry(const std::string& native, int flags, mode_t mode = 0) noexcept {
    if (native.empty()) {
        errno = EINVAL;
        return -1;
    }
    int fd = -1;
    do {
        fd = ::open(native.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

/**
 * @brief Writes every byte of @p bytes, riding out short writes and EINTR.
 */
Result<void> writeFully(int fd, std::string_view bytes, std::wstring_view what) {
    size_t done = 0;
    while (done < bytes.size()) {
        const size_t want = std::min(kChunk, bytes.size() - done);
        const ssize_t n = ::write(fd, bytes.data() + done, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errnoError(L"write " + std::wstring(what));
        }
        if (n == 0) {
            return Error::fromWin32(ERROR_DISK_FULL, L"write " + std::wstring(what));
        }
        done += static_cast<size_t>(n);
    }
    return Result<void>::success();
}

/**
 * @brief Pushes a file's data all the way to stable storage.
 *
 * fsync() on macOS only reaches the drive's cache; F_FULLFSYNC asks the
 * drive to flush as well, which is what FILE_FLAG_WRITE_THROUGH promises.
 */
void flushToDisk(int fd) noexcept {
#if defined(F_FULLFSYNC)
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return;
    }
#endif
    ::fsync(fd);
}

/**
 * @brief Copies @p from to @p to (data + metadata on macOS).
 * @param exclusive fail with EEXIST when the destination exists
 */
bool copyNative(const std::string& from, const std::string& to, bool exclusive) noexcept {
#if defined(__APPLE__)
    copyfile_flags_t flags = COPYFILE_ALL;
    if (exclusive) {
        flags |= COPYFILE_EXCL;
    }
    return ::copyfile(from.c_str(), to.c_str(), nullptr, flags) == 0;
#else
    // Portable fallback: byte copy with the source permissions.
    struct stat st {};
    if (!statPath(from, st)) {
        return false;
    }
    int in = openRetry(from, O_RDONLY);
    if (in < 0) {
        return false;
    }
    int out = openRetry(to, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) {
        closeQuietly(in);
        return false;
    }
    std::vector<char> buffer(kChunk);
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = (n == 0);
            break;
        }
        if (!writeFully(out, std::string_view(buffer.data(), static_cast<size_t>(n)), L"copy")) {
            ok = false;
            break;
        }
    }
    closeQuietly(in);
    closeQuietly(out);
    return ok;
#endif
}

} // namespace

// ---- FileIdentity ---------------------------------------------------------------

bool FileIdentity::sameFileAs(const FileIdentity& other) const noexcept {
    // Without both identities being valid there is nothing to compare.
    if (!valid || !other.valid) { return false; }

    // Stable file ids (device + inode) are authoritative when both sides have them.
    if (hasFileId && other.hasFileId) {
        return volumeSerial == other.volumeSerial && fileIdLow == other.fileIdLow && fileIdHigh == other.fileIdHigh;
    }

    // Fallback for file systems without ids: creation time + size.
    return creationUtc == other.creationUtc && size == other.size;
}

// ---- path normalisation ---------------------------------------------------------

/**
 * @brief Absolute + lexically normalised: "." and ".." folded, duplicate
 *        slashes collapsed, a trailing slash kept only when it was given.
 */
std::wstring fullPath(std::wstring_view path) {
    if (path.empty()) { return {}; }

    // Relative paths hang off the current directory, like GetFullPathNameW.
    std::wstring input(path);
    if (input.front() != L'/') {
        char cwd[PATH_MAX] = {};
        if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
            return input;
        }
        std::wstring base = fromNative(cwd);
        if (base.empty() || base.back() != L'/') { base += L'/'; }
        input = base + input;
    }
    const bool trailingSlash = input.size() > 1 && input.back() == L'/';

    // Walk the components, folding "." and "..".
    std::vector<std::wstring_view> parts;
    const std::wstring_view view(input);
    size_t i = 0;
    while (i < view.size()) {
        while (i < view.size() && view[i] == L'/') { ++i; }
        const size_t start = i;
        while (i < view.size() && view[i] != L'/') { ++i; }
        const std::wstring_view part = view.substr(start, i - start);
        if (part.empty() || part == L".") { continue; }
        if (part == L"..") {
            if (!parts.empty()) { parts.pop_back(); }   // ".." at the root stays at the root
            continue;
        }
        parts.push_back(part);
    }

    std::wstring out;
    out.reserve(input.size());
    for (const std::wstring_view part : parts) {
        out += L'/';
        out.append(part);
    }
    if (out.empty()) { return L"/"; }
    if (trailingSlash) { out += L'/'; }
    return out;
}

std::wstring toExtendedPath(std::wstring_view path) {
    // No length limit to work around on POSIX: the extended form is the full form.
    return fullPath(path);
}

// ---- existence ------------------------------------------------------------------

bool exists(std::wstring_view path) {
    if (path.empty()) { return false; }
    struct stat st {};
    return statPath(toNative(path), st);
}

bool isDirectory(std::wstring_view path) {
    if (path.empty()) { return false; }
    struct stat st {};
    return statPath(toNative(path), st) && S_ISDIR(st.st_mode);
}

bool isFile(std::wstring_view path) {
    if (path.empty()) { return false; }
    struct stat st {};
    return statPath(toNative(path), st) && !S_ISDIR(st.st_mode);
}

// ---- attributes -----------------------------------------------------------------

Result<uint64_t> fileSize(std::wstring_view path) {
    struct stat st {};
    if (!statPath(toNative(path), st)) { return errnoError(L"stat " + std::wstring(path)); }
    return static_cast<uint64_t>(st.st_size < 0 ? 0 : st.st_size);
}

Result<uint64_t> lastWriteUtc(std::wstring_view path) {
    struct stat st {};
    if (!statPath(toNative(path), st)) { return errnoError(L"stat " + std::wstring(path)); }
    return lastWriteTicks(st);
}

Result<FileIdentity> identity(std::wstring_view path) {
    // stat never conflicts with a writer; there is nothing to open.
    struct stat st {};
    if (!statPath(toNative(path), st)) { return errnoError(L"stat " + std::wstring(path)); }

    FileIdentity id;
    id.size = static_cast<uint64_t>(st.st_size < 0 ? 0 : st.st_size);
    id.lastWriteUtc = lastWriteTicks(st);
    id.creationUtc = creationTicks(st);
    // Device + inode are the POSIX file id; both are stable for a file's lifetime.
    id.volumeSerial = static_cast<uint32_t>(static_cast<uint64_t>(st.st_dev) & 0xFFFFFFFFull);
    id.fileIdLow = static_cast<uint64_t>(st.st_ino);
    id.fileIdHigh = static_cast<uint64_t>(static_cast<uint64_t>(st.st_dev) >> 32);
    id.hasFileId = st.st_ino != 0;
    id.valid = true;
    return id;
}

Result<FileAttributes> fileAttributes(std::wstring_view path) {
    struct stat st {};
    if (!statPath(toNative(path), st)) { return errnoError(L"stat " + std::wstring(path)); }
    FileAttributes a;
    a.size = static_cast<uint64_t>(st.st_size < 0 ? 0 : st.st_size);
    a.lastWriteUtc = lastWriteTicks(st);
    a.creationUtc = creationTicks(st);
    a.isDirectory = S_ISDIR(st.st_mode);
    return a;
}

// ---- FileReader --------------------------------------------------------------------

FileReader::~FileReader() {
    close();
}

FileReader::FileReader(FileReader&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Result<void> FileReader::open(std::wstring_view path, bool sequential) {
    close();
    if (path.empty()) {
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"FileReader::open: empty path");
    }
    fd_ = openRetry(toNative(path), O_RDONLY);
    if (fd_ < 0) {
        return errnoError(L"open(read) " + std::wstring(path));
    }
#if defined(F_RDAHEAD)
    // Read-ahead helps the log tailer's forward walk and hurts the box walker's jumps.
    ::fcntl(fd_, F_RDAHEAD, sequential ? 1 : 0);
#else
    static_cast<void>(sequential);
#endif
    return Result<void>::success();
}

void FileReader::close() noexcept {
    closeQuietly(fd_);
}

bool FileReader::isOpen() const noexcept {
    return fd_ >= 0;
}

bool FileReader::readAt(uint64_t offset, void* dst, size_t length, size_t& got, DWORD& lastError) noexcept {
    got = 0;
    lastError = 0;
    if (fd_ < 0 || dst == nullptr) {
        lastError = ERROR_INVALID_HANDLE;
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (offset > static_cast<uint64_t>(INT64_MAX)) {
        lastError = ERROR_INVALID_PARAMETER;
        return false;
    }
    // pread never moves a shared file position, so this is safe from any thread.
    const size_t want = std::min(length, kChunk);
    ssize_t n = -1;
    do {
        n = ::pread(fd_, dst, want, static_cast<off_t>(offset));
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        lastError = win32FromErrno(errno);
        return false;
    }
    got = static_cast<size_t>(n);
    return true;
}

Result<uint64_t> FileReader::size() const {
    if (fd_ < 0) {
        return Error::fromWin32(ERROR_INVALID_HANDLE, L"FileReader::size: not open");
    }
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        return errnoError(L"fstat");
    }
    return static_cast<uint64_t>(st.st_size < 0 ? 0 : st.st_size);
}

// ---- reading ---------------------------------------------------------------------

Result<std::vector<uint8_t>> readAll(std::wstring_view path, uint64_t maxBytes) {
    FileReader reader;
    if (auto opened = reader.open(path, true); !opened) { return opened.error(); }
    const auto size = reader.size();
    if (!size) { return size.error(); }
    if (size.value() > maxBytes) {
        return Error::text(L"file too large to read: " + std::wstring(path));
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size.value()));
    size_t done = 0;
    // Read in bounded chunks; a growing file may hand back fewer bytes than announced.
    while (done < bytes.size()) {
        size_t got = 0;
        DWORD err = 0;
        if (!reader.readAt(done, bytes.data() + done, bytes.size() - done, got, err)) {
            return Error::fromWin32(err, L"read " + std::wstring(path));
        }
        if (got == 0) { break; }
        done += got;
    }
    bytes.resize(done);
    return bytes;
}

Result<std::vector<uint8_t>> readRange(std::wstring_view path, uint64_t offset, size_t length) {
    FileReader reader;
    if (auto opened = reader.open(path, false); !opened) { return opened.error(); }

    std::vector<uint8_t> bytes(length);
    size_t done = 0;
    while (done < bytes.size()) {
        size_t got = 0;
        DWORD err = 0;
        if (!reader.readAt(offset + done, bytes.data() + done, bytes.size() - done, got, err)) {
            return Error::fromWin32(err, L"read " + std::wstring(path));
        }
        if (got == 0) { break; }   // EOF: fewer bytes than asked
        done += got;
    }
    bytes.resize(done);
    return bytes;
}

// ---- writing ---------------------------------------------------------------------

Result<void> writeAllAtomic(std::wstring_view path, std::string_view bytes) {
    if (path.empty()) { return Error::text(L"writeAllAtomic: empty path"); }

    // Make sure the parent exists (settings/state folders are created lazily).
    const std::wstring full = fullPath(path);
    const size_t slash = full.find_last_of(L'/');
    if (slash != std::wstring::npos && slash > 0) {
        if (auto r = createDirectories(full.substr(0, slash)); !r) { return r; }
    }

    // Write to a sibling temp file and push it to stable storage.
    const std::wstring temp = full + L".tmp";
    const std::string nativeTemp = toNative(temp);
    {
        int fd = openRetry(nativeTemp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { return errnoError(L"open(temp) " + temp); }
        if (auto r = writeFully(fd, bytes, temp); !r) {
            closeQuietly(fd);
            ::unlink(nativeTemp.c_str());
            return r;
        }
        flushToDisk(fd);
        closeQuietly(fd);
    }

    // rename() is atomic: readers see the old file or the new one, never a torn one.
    if (::rename(nativeTemp.c_str(), toNative(full).c_str()) != 0) {
        const Error e = errnoError(L"rename " + temp);
        ::unlink(nativeTemp.c_str());
        return e;
    }
    return Result<void>::success();
}

Result<void> appendAll(std::wstring_view path, std::string_view bytes) {
    int fd = openRetry(toNative(path), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { return errnoError(L"open(append) " + std::wstring(path)); }
    Result<void> r = writeFully(fd, bytes, path);
    closeQuietly(fd);
    return r;
}

// ---- directories / moves --------------------------------------------------------

Result<void> createDirectories(std::wstring_view path) {
    if (path.empty()) { return Error::text(L"createDirectories: empty path"); }
    const std::wstring full = fullPath(path);
    if (isDirectory(full)) { return Result<void>::success(); }

    // Walk the path from the root creating each missing segment.
    for (size_t i = 1; i <= full.size(); ++i) {
        if (i != full.size() && full[i] != L'/') { continue; }
        const std::wstring segment = full.substr(0, i);
        if (segment.empty() || segment == L"/") { continue; }
        const std::string native = toNative(segment);
        if (::mkdir(native.c_str(), 0755) != 0) {
            const int err = errno;
            // EEXIST is fine when the thing that exists is a directory.
            if (err == EEXIST && isDirectory(segment)) { continue; }
            if (err == EEXIST) {
                return Error::fromWin32(ERROR_DIRECTORY, L"mkdir " + segment + L" (a file is in the way)");
            }
            return errnoError(L"mkdir " + segment, err);
        }
    }
    return Result<void>::success();
}

Result<void> deleteFile(std::wstring_view path) {
    if (path.empty()) { return Result<void>::success(); }
    if (::unlink(toNative(path).c_str()) != 0) {
        const int err = errno;
        // Already gone counts as success.
        if (err == ENOENT) { return Result<void>::success(); }
        return errnoError(L"unlink " + std::wstring(path), err);
    }
    return Result<void>::success();
}

Result<void> moveReplace(std::wstring_view from, std::wstring_view to) {
    const std::string src = toNative(from);
    const std::string dst = toNative(to);
    if (::rename(src.c_str(), dst.c_str()) == 0) {
        return Result<void>::success();
    }
    const int err = errno;
    // Different volume: copy + delete, like MOVEFILE_COPY_ALLOWED.
    if (err == EXDEV) {
        if (!copyNative(src, dst, false)) {
            return errnoError(L"copy " + std::wstring(from) + L" -> " + std::wstring(to));
        }
        ::unlink(src.c_str());
        return Result<void>::success();
    }
    return errnoError(L"rename " + std::wstring(from) + L" -> " + std::wstring(to), err);
}

Result<void> moveNoReplace(std::wstring_view from, std::wstring_view to) {
    if (exists(to)) { return Error::text(L"destination exists: " + std::wstring(to)); }
    const std::string src = toNative(from);
    const std::string dst = toNative(to);
#if defined(__APPLE__) && defined(RENAME_EXCL)
    // Atomic "rename unless the target exists" - closes the exists() race above.
    if (::renamex_np(src.c_str(), dst.c_str(), RENAME_EXCL) == 0) {
        return Result<void>::success();
    }
#else
    if (::link(src.c_str(), dst.c_str()) == 0) {
        ::unlink(src.c_str());
        return Result<void>::success();
    }
#endif
    const int err = errno;
    if (err == EXDEV) {
        if (!copyNative(src, dst, true)) {
            return errnoError(L"copy " + std::wstring(from) + L" -> " + std::wstring(to));
        }
        ::unlink(src.c_str());
        return Result<void>::success();
    }
    if (err == EEXIST) {
        return Error::text(L"destination exists: " + std::wstring(to));
    }
    return errnoError(L"rename " + std::wstring(from) + L" -> " + std::wstring(to), err);
}

Result<void> copyFile(std::wstring_view from, std::wstring_view to, bool overwrite) {
    if (!copyNative(toNative(from), toNative(to), !overwrite)) {
        return errnoError(L"copy " + std::wstring(from) + L" -> " + std::wstring(to));
    }
    return Result<void>::success();
}

// ---- probes ----------------------------------------------------------------------

OpenProbe probeDenyWrite(std::wstring_view path, DWORD* lastError) {
    if (lastError) { *lastError = 0; }
    const std::string native = toNative(path);

    // The file must exist and be readable by us first.
    struct stat st {};
    if (!statPath(native, st)) {
        const int err = errno;
        if (lastError) { *lastError = win32FromErrno(err); }
        if (err == ENOENT || err == ENOTDIR || err == EINVAL) { return OpenProbe::Missing; }
        if (err == EACCES || err == EPERM) { return OpenProbe::Denied; }
        return OpenProbe::Error;
    }
    if (::access(native.c_str(), R_OK) != 0) {
        if (lastError) { *lastError = ERROR_ACCESS_DENIED; }
        return OpenProbe::Denied;
    }

#if defined(__APPLE__)
    // Which processes hold this file open? proc_listpidspath scans every
    // process's descriptor table in the kernel; typically a millisecond.
    std::vector<int> pids(2048);
    const int bytes = ::proc_listpidspath(PROC_ALL_PIDS, 0, native.c_str(), 0, pids.data(),
                                          static_cast<int>(pids.size() * sizeof(int)));
    if (bytes <= 0) {
        return OpenProbe::Ready;   // nobody (or the query is unavailable): nothing blocks us
    }
    const int count = std::min<int>(bytes / static_cast<int>(sizeof(int)), static_cast<int>(pids.size()));
    const int self = static_cast<int>(::getpid());
    for (int i = 0; i < count; ++i) {
        const int pid = pids[static_cast<size_t>(i)];
        if (pid <= 0 || pid == self) { continue; }

        // That process's descriptor table.
        const int need = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (need <= 0) { continue; }
        std::vector<proc_fdinfo> fds(static_cast<size_t>(need) / sizeof(proc_fdinfo) + 16);
        const int got = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
        if (got <= 0) { continue; }
        const size_t n = static_cast<size_t>(got) / sizeof(proc_fdinfo);

        // A vnode descriptor on our (device, inode) opened with FWRITE is a writer.
        for (size_t k = 0; k < n && k < fds.size(); ++k) {
            if (fds[k].proc_fdtype != PROX_FDTYPE_VNODE) { continue; }
            vnode_fdinfowithpath info {};
            const int r = ::proc_pidfdinfo(pid, fds[k].proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info));
            if (r != static_cast<int>(sizeof(info))) { continue; }
            const auto& vst = info.pvip.vip_vi.vi_stat;
            if (static_cast<uint64_t>(vst.vst_ino) != static_cast<uint64_t>(st.st_ino) ||
                static_cast<uint32_t>(vst.vst_dev) != static_cast<uint32_t>(st.st_dev)) {
                continue;
            }
            if ((info.pfi.fi_openflags & kOpenForWrite) != 0) {
                if (lastError) { *lastError = ERROR_SHARING_VIOLATION; }
                return OpenProbe::Writing;
            }
        }
    }
#endif
    return OpenProbe::Ready;
}

// ---- listing ---------------------------------------------------------------------

Result<std::vector<DirEntry>> listDirectory(std::wstring_view dir) {
    if (dir.empty()) { return Error::text(L"listDirectory: empty path"); }
    const std::string native = toNative(dir);
    DIR* d = native.empty() ? nullptr : ::opendir(native.c_str());
    if (d == nullptr) {
        return errnoError(L"opendir " + std::wstring(dir));
    }
    const int dirFd = ::dirfd(d);

    std::vector<DirEntry> entries;
    for (;;) {
        errno = 0;
        const dirent* e = ::readdir(d);
        if (e == nullptr) {
            if (errno != 0) {
                const Error err = errnoError(L"readdir " + std::wstring(dir));
                ::closedir(d);
                return err;
            }
            break;
        }
        // Skip the pseudo entries.
        const char* name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }

        DirEntry entry;
        entry.name = fromNative(name);

        // The link itself first (REPARSE_POINT), then what it points at (size / kind).
        struct stat lst {};
        const bool haveLink = ::fstatat(dirFd, name, &lst, AT_SYMLINK_NOFOLLOW) == 0;
        struct stat st {};
        const bool haveTarget = ::fstatat(dirFd, name, &st, 0) == 0;
        const struct stat& info = haveTarget ? st : lst;
        if (haveTarget || haveLink) {
            entry.isDirectory = S_ISDIR(info.st_mode);
            entry.size = entry.isDirectory ? 0 : static_cast<uint64_t>(info.st_size < 0 ? 0 : info.st_size);
            entry.lastWriteUtc = lastWriteTicks(info);
        }
        DWORD attrs = 0;
        if (entry.isDirectory) { attrs |= FILE_ATTRIBUTE_DIRECTORY; }
        if (name[0] == '.') { attrs |= FILE_ATTRIBUTE_HIDDEN; }
        if (haveLink && S_ISLNK(lst.st_mode)) { attrs |= FILE_ATTRIBUTE_REPARSE_POINT; }
        if ((haveTarget || haveLink) && (info.st_mode & S_IWUSR) == 0) { attrs |= FILE_ATTRIBUTE_READONLY; }
        entry.attributes = attrs == 0 ? FILE_ATTRIBUTE_NORMAL : attrs;
        entries.push_back(std::move(entry));
    }
    ::closedir(d);
    return entries;
}

// ---- volumes ---------------------------------------------------------------------

Result<std::wstring> volumeRoot(std::wstring_view path) {
    const std::wstring full = fullPath(path);
    struct statfs fs {};
    if (::statfs(toNative(full).c_str(), &fs) != 0) {
        return errnoError(L"statfs " + full);
    }
    return fromNative(fs.f_mntonname);
}

bool isRemotePath(std::wstring_view path) {
    const std::wstring full = fullPath(path);
    struct statfs fs {};
    if (::statfs(toNative(full).c_str(), &fs) != 0) {
        return false;
    }
    // MNT_LOCAL is set for every volume backed by local storage.
    return (fs.f_flags & MNT_LOCAL) == 0;
}

} // namespace hh::platform
