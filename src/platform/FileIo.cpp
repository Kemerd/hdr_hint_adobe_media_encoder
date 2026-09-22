// ---------------------------------------------------------------------------
// FileIo.cpp - defensive file helpers on the wide Win32 APIs.
// ---------------------------------------------------------------------------
#include "platform/FileIo.h"

#include "platform/Handle.h"
#include "platform/Utf.h"

#include <algorithm>
#include <string>
#include <vector>

namespace hh::platform {

namespace {

/// Largest single ReadFile/WriteFile chunk we issue (keeps DWORD sizes sane).
constexpr DWORD kChunk = 1u << 20;

/// True when the path already carries a "\\?\" or "\\.\" prefix.
bool hasDevicePrefix(std::wstring_view p) {
    return p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'?' || p[2] == L'.') && p[3] == L'\\';
}

/// True for "\\server\share" style paths.
bool isUncPath(std::wstring_view p) {
    return p.size() >= 3 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.';
}

/// Opens a file for reading with the most permissive sharing (never blocks writers).
UniqueHandle openShared(std::wstring_view path, DWORD access, DWORD flags) {
    const std::wstring ext = toExtendedPath(path);
    return UniqueHandle(::CreateFileW(ext.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, flags, nullptr));
}

/// FILETIME -> uint64 (100 ns ticks).
uint64_t ftToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

} // namespace

// ---- FileIdentity ---------------------------------------------------------------

bool FileIdentity::sameFileAs(const FileIdentity& other) const noexcept {
    // Without both identities being valid there is nothing to compare.
    if (!valid || !other.valid) { return false; }

    // Stable file ids are authoritative when both sides have them.
    if (hasFileId && other.hasFileId) {
        return volumeSerial == other.volumeSerial && fileIdLow == other.fileIdLow && fileIdHigh == other.fileIdHigh;
    }

    // Fallback for file systems without ids: creation time + size.
    return creationUtc == other.creationUtc && size == other.size;
}

// ---- path normalisation ---------------------------------------------------------

std::wstring fullPath(std::wstring_view path) {
    if (path.empty()) { return {}; }

    // A \\?\ path is already absolute; only strip the prefix so callers get a plain path.
    std::wstring input(path);
    if (hasDevicePrefix(input)) {
        if (input.rfind(L"\\\\?\\UNC\\", 0) == 0) {
            return L"\\\\" + input.substr(8);
        }
        return input.substr(4);
    }

    // GetFullPathNameW resolves relative segments and converts forward slashes.
    std::wstring out(MAX_PATH, L'\0');
    DWORD needed = ::GetFullPathNameW(input.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (needed == 0) { return input; }
    if (needed > out.size()) {
        out.assign(needed, L'\0');
        needed = ::GetFullPathNameW(input.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
        if (needed == 0 || needed > out.size()) { return input; }
    }
    out.resize(needed);
    return out;
}

std::wstring toExtendedPath(std::wstring_view path) {
    if (path.empty()) { return {}; }
    if (hasDevicePrefix(path)) { return std::wstring(path); }

    // Normalise first: the prefix disables all path parsing (including '/').
    const std::wstring full = fullPath(path);
    if (isUncPath(full)) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

// ---- existence ------------------------------------------------------------------

bool exists(std::wstring_view path) {
    if (path.empty()) { return false; }
    const DWORD attrs = ::GetFileAttributesW(toExtendedPath(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory(std::wstring_view path) {
    if (path.empty()) { return false; }
    const DWORD attrs = ::GetFileAttributesW(toExtendedPath(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isFile(std::wstring_view path) {
    if (path.empty()) { return false; }
    const DWORD attrs = ::GetFileAttributesW(toExtendedPath(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ---- attributes -----------------------------------------------------------------

Result<uint64_t> fileSize(std::wstring_view path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(toExtendedPath(path).c_str(), GetFileExInfoStandard, &data)) {
        return Error::fromLastError(L"GetFileAttributesEx " + std::wstring(path));
    }
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

Result<uint64_t> lastWriteUtc(std::wstring_view path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(toExtendedPath(path).c_str(), GetFileExInfoStandard, &data)) {
        return Error::fromLastError(L"GetFileAttributesEx " + std::wstring(path));
    }
    return ftToU64(data.ftLastWriteTime);
}

Result<FileIdentity> identity(std::wstring_view path) {
    // FILE_READ_ATTRIBUTES never conflicts with a writer's exclusive data access.
    UniqueHandle h(::CreateFileW(toExtendedPath(path).c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h) { return Error::fromLastError(L"CreateFile(attributes) " + std::wstring(path)); }

    FileIdentity id;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h.get(), &info)) {
        return Error::fromLastError(L"GetFileInformationByHandle " + std::wstring(path));
    }
    id.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    id.lastWriteUtc = ftToU64(info.ftLastWriteTime);
    id.creationUtc = ftToU64(info.ftCreationTime);
    id.volumeSerial = info.dwVolumeSerialNumber;

    // Prefer the 128-bit id (ReFS / newer NTFS); fall back to the classic 64-bit index.
    FILE_ID_INFO idInfo{};
    if (::GetFileInformationByHandleEx(h.get(), FileIdInfo, &idInfo, sizeof(idInfo))) {
        uint64_t low = 0, high = 0;
        std::memcpy(&low, idInfo.FileId.Identifier, sizeof(low));
        std::memcpy(&high, idInfo.FileId.Identifier + sizeof(low), sizeof(high));
        id.fileIdLow = low;
        id.fileIdHigh = high;
        id.volumeSerial = static_cast<uint32_t>(idInfo.VolumeSerialNumber & 0xFFFFFFFFull);
        id.hasFileId = true;
    } else if (info.nFileIndexHigh != 0 || info.nFileIndexLow != 0) {
        id.fileIdLow = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        id.fileIdHigh = 0;
        id.hasFileId = true;
    }
    id.valid = true;
    return id;
}

Result<FileAttributes> fileAttributes(std::wstring_view path) {
    // One metadata query; never opens the file, so AME's exclusive lock is irrelevant.
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(toExtendedPath(path).c_str(), GetFileExInfoStandard, &data)) {
        return Error::fromLastError(L"GetFileAttributesEx " + std::wstring(path));
    }
    FileAttributes a;
    a.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    a.lastWriteUtc = ftToU64(data.ftLastWriteTime);
    a.creationUtc = ftToU64(data.ftCreationTime);
    a.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return a;
}

// ---- FileReader --------------------------------------------------------------------

FileReader::~FileReader() {
    close();
}

FileReader::FileReader(FileReader&& other) noexcept : handle_(std::move(other.handle_)) {}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
    if (this != &other) {
        handle_ = std::move(other.handle_);
    }
    return *this;
}

Result<void> FileReader::open(std::wstring_view path, bool sequential) {
    close();
    if (path.empty()) {
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"FileReader::open: empty path");
    }
    // Share everything: a log writer or AME finalising a file must never see a sharing violation from us.
    handle_ = openShared(path, GENERIC_READ, sequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_FLAG_RANDOM_ACCESS);
    if (!handle_) {
        return Error::fromLastError(L"CreateFile(read) " + std::wstring(path));
    }
    return Result<void>::success();
}

void FileReader::close() noexcept {
    handle_.reset();
}

bool FileReader::isOpen() const noexcept {
    return handle_.valid();
}

bool FileReader::readAt(uint64_t offset, void* dst, size_t length, size_t& got, DWORD& lastError) noexcept {
    got = 0;
    lastError = 0;
    if (!handle_ || dst == nullptr) {
        lastError = ERROR_INVALID_HANDLE;
        return false;
    }
    if (length == 0) {
        return true;
    }
    // Position, then one bounded read; callers loop for more.
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(handle_.get(), pos, nullptr, FILE_BEGIN)) {
        lastError = ::GetLastError();
        return false;
    }
    const DWORD want = static_cast<DWORD>(std::min<size_t>(length, kChunk));
    DWORD read = 0;
    if (!::ReadFile(handle_.get(), dst, want, &read, nullptr)) {
        const DWORD err = ::GetLastError();
        // Reading past the end of a file that shrank is EOF, not a failure.
        if (err == ERROR_HANDLE_EOF) {
            return true;
        }
        lastError = err;
        return false;
    }
    got = static_cast<size_t>(read);
    return true;
}

Result<uint64_t> FileReader::size() const {
    if (!handle_) {
        return Error::fromWin32(ERROR_INVALID_HANDLE, L"FileReader::size: not open");
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle_.get(), &size) || size.QuadPart < 0) {
        return Error::fromLastError(L"GetFileSizeEx");
    }
    return static_cast<uint64_t>(size.QuadPart);
}

// ---- reading ---------------------------------------------------------------------

Result<std::vector<uint8_t>> readAll(std::wstring_view path, uint64_t maxBytes) {
    UniqueHandle h = openShared(path, GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN);
    if (!h) { return Error::fromLastError(L"CreateFile(read) " + std::wstring(path)); }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h.get(), &size)) { return Error::fromLastError(L"GetFileSizeEx " + std::wstring(path)); }
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > maxBytes) {
        return Error::text(L"file too large to read: " + std::wstring(path));
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    // Read in bounded chunks; a growing file may hand back fewer bytes than announced.
    while (done < bytes.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(kChunk, bytes.size() - done));
        DWORD got = 0;
        if (!::ReadFile(h.get(), bytes.data() + done, want, &got, nullptr)) {
            return Error::fromLastError(L"ReadFile " + std::wstring(path));
        }
        if (got == 0) { break; }
        done += got;
    }
    bytes.resize(done);
    return bytes;
}

Result<std::vector<uint8_t>> readRange(std::wstring_view path, uint64_t offset, size_t length) {
    UniqueHandle h = openShared(path, GENERIC_READ, FILE_FLAG_RANDOM_ACCESS);
    if (!h) { return Error::fromLastError(L"CreateFile(read) " + std::wstring(path)); }

    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h.get(), pos, nullptr, FILE_BEGIN)) {
        return Error::fromLastError(L"SetFilePointerEx " + std::wstring(path));
    }

    std::vector<uint8_t> bytes(length);
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(kChunk, bytes.size() - done));
        DWORD got = 0;
        if (!::ReadFile(h.get(), bytes.data() + done, want, &got, nullptr)) {
            return Error::fromLastError(L"ReadFile " + std::wstring(path));
        }
        if (got == 0) { break; }   // EOF: fewer bytes than asked
        done += got;
    }
    bytes.resize(done);
    return bytes;
}

// ---- writing ---------------------------------------------------------------------

namespace {

/// Writes every byte of @p bytes to an open handle.
Result<void> writeFully(HANDLE h, std::string_view bytes, std::wstring_view what) {
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(kChunk, bytes.size() - done));
        DWORD wrote = 0;
        if (!::WriteFile(h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
            return Error::fromLastError(L"WriteFile " + std::wstring(what));
        }
        done += wrote;
    }
    return Result<void>::success();
}

} // namespace

Result<void> writeAllAtomic(std::wstring_view path, std::string_view bytes) {
    if (path.empty()) { return Error::text(L"writeAllAtomic: empty path"); }

    // Make sure the parent exists (settings/state folders are created lazily).
    const std::wstring full = fullPath(path);
    const size_t slash = full.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 0) {
        if (auto r = createDirectories(full.substr(0, slash)); !r) { return r; }
    }

    // Write to a sibling temp file with write-through so the bytes hit the disk.
    const std::wstring temp = full + L".tmp";
    {
        UniqueHandle h(::CreateFileW(toExtendedPath(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!h) { return Error::fromLastError(L"CreateFile(temp) " + temp); }
        if (auto r = writeFully(h.get(), bytes, temp); !r) { return r; }
        ::FlushFileBuffers(h.get());
    }

    // Swap it into place; MOVEFILE_REPLACE_EXISTING keeps readers from ever seeing a torn file.
    if (!::MoveFileExW(toExtendedPath(temp).c_str(), toExtendedPath(full).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const Error e = Error::fromLastError(L"MoveFileEx " + temp);
        ::DeleteFileW(toExtendedPath(temp).c_str());
        return e;
    }
    return Result<void>::success();
}

Result<void> appendAll(std::wstring_view path, std::string_view bytes) {
    UniqueHandle h(::CreateFileW(toExtendedPath(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) { return Error::fromLastError(L"CreateFile(append) " + std::wstring(path)); }
    return writeFully(h.get(), bytes, path);
}

// ---- directories / moves --------------------------------------------------------

Result<void> createDirectories(std::wstring_view path) {
    if (path.empty()) { return Error::text(L"createDirectories: empty path"); }
    const std::wstring full = fullPath(path);
    if (isDirectory(full)) { return Result<void>::success(); }

    // Walk the path from the root creating each missing segment.
    size_t start = 0;
    if (isUncPath(full)) {
        // Skip "\\server\share".
        size_t p = full.find(L'\\', 2);
        if (p != std::wstring::npos) { p = full.find(L'\\', p + 1); }
        start = (p == std::wstring::npos) ? full.size() : p;
    } else if (full.size() >= 2 && full[1] == L':') {
        start = 2;
    }
    for (size_t i = start; i <= full.size(); ++i) {
        if (i == full.size() || full[i] == L'\\') {
            const std::wstring segment = full.substr(0, i);
            if (segment.empty() || segment.back() == L':') { continue; }
            if (!::CreateDirectoryW(toExtendedPath(segment).c_str(), nullptr)) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    return Error::fromWin32(err, L"CreateDirectory " + segment);
                }
            }
        }
    }
    return Result<void>::success();
}

Result<void> deleteFile(std::wstring_view path) {
    if (path.empty()) { return Result<void>::success(); }
    if (!::DeleteFileW(toExtendedPath(path).c_str())) {
        const DWORD err = ::GetLastError();
        // Already gone counts as success.
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) { return Result<void>::success(); }
        return Error::fromWin32(err, L"DeleteFile " + std::wstring(path));
    }
    return Result<void>::success();
}

Result<void> moveReplace(std::wstring_view from, std::wstring_view to) {
    if (!::MoveFileExW(toExtendedPath(from).c_str(), toExtendedPath(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
        return Error::fromLastError(L"MoveFileEx " + std::wstring(from) + L" -> " + std::wstring(to));
    }
    return Result<void>::success();
}

Result<void> moveNoReplace(std::wstring_view from, std::wstring_view to) {
    if (exists(to)) { return Error::text(L"destination exists: " + std::wstring(to)); }
    if (!::MoveFileExW(toExtendedPath(from).c_str(), toExtendedPath(to).c_str(),
                       MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
        return Error::fromLastError(L"MoveFileEx " + std::wstring(from) + L" -> " + std::wstring(to));
    }
    return Result<void>::success();
}

Result<void> copyFile(std::wstring_view from, std::wstring_view to, bool overwrite) {
    if (!::CopyFileW(toExtendedPath(from).c_str(), toExtendedPath(to).c_str(), overwrite ? FALSE : TRUE)) {
        return Error::fromLastError(L"CopyFile " + std::wstring(from) + L" -> " + std::wstring(to));
    }
    return Result<void>::success();
}

// ---- probes ----------------------------------------------------------------------

OpenProbe probeDenyWrite(std::wstring_view path, DWORD* lastError) {
    // Deny-write sharing: succeeds with any number of readers, fails while a writer holds the file.
    UniqueHandle h(::CreateFileW(toExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h) {
        if (lastError) { *lastError = 0; }
        return OpenProbe::Ready;
    }
    const DWORD err = ::GetLastError();
    if (lastError) { *lastError = err; }
    switch (err) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return OpenProbe::Writing;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return OpenProbe::Missing;
    case ERROR_ACCESS_DENIED:
        return OpenProbe::Denied;
    default:
        return OpenProbe::Error;
    }
}

// ---- listing ---------------------------------------------------------------------

Result<std::vector<DirEntry>> listDirectory(std::wstring_view dir) {
    if (dir.empty()) { return Error::text(L"listDirectory: empty path"); }
    std::wstring pattern = toExtendedPath(dir);
    if (pattern.back() != L'\\') { pattern += L'\\'; }
    pattern += L'*';

    WIN32_FIND_DATAW data{};
    UniqueFindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) { return std::vector<DirEntry>{}; }   // empty directory
        return Error::fromWin32(err, L"FindFirstFileEx " + std::wstring(dir));
    }

    std::vector<DirEntry> entries;
    do {
        // Skip the pseudo entries.
        if (data.cFileName[0] == L'.' && (data.cFileName[1] == L'\0' || (data.cFileName[1] == L'.' && data.cFileName[2] == L'\0'))) {
            continue;
        }
        DirEntry e;
        e.name = data.cFileName;
        e.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        e.lastWriteUtc = ftToU64(data.ftLastWriteTime);
        e.attributes = data.dwFileAttributes;
        entries.push_back(std::move(e));
    } while (::FindNextFileW(find.get(), &data));

    const DWORD err = ::GetLastError();
    if (err != ERROR_NO_MORE_FILES && err != ERROR_SUCCESS) {
        return Error::fromWin32(err, L"FindNextFile " + std::wstring(dir));
    }
    return entries;
}

// ---- volumes ---------------------------------------------------------------------

Result<std::wstring> volumeRoot(std::wstring_view path) {
    const std::wstring full = fullPath(path);
    std::wstring root(MAX_PATH, L'\0');
    if (!::GetVolumePathNameW(full.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
        return Error::fromLastError(L"GetVolumePathName " + full);
    }
    root.resize(::wcsnlen(root.c_str(), root.size()));
    return root;
}

bool isRemotePath(std::wstring_view path) {
    const std::wstring full = fullPath(path);
    if (isUncPath(full)) { return true; }
    auto root = volumeRoot(full);
    if (!root) { return false; }
    return ::GetDriveTypeW(root.value().c_str()) == DRIVE_REMOTE;
}

} // namespace hh::platform
