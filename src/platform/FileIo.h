// ---------------------------------------------------------------------------
// FileIo.h - small, defensive file helpers.
//
// Windows: built on the wide Win32 APIs. All paths are accepted as ordinary
// Win32 paths; helpers apply the \\?\ prefix internally where an API needs
// it (see toExtendedPath).
// POSIX:   built on open/pread/stat/rename. Paths are UTF-8 on the wire;
// names read back from the file system are NFC-normalised so they compare
// equal to the same name typed by the user or written by AME.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#if defined(_WIN32)
#include "platform/Handle.h"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace hh::platform {

/**
 * @brief Identity of a file on disk, used to detect "same file?" cheaply.
 */
struct FileIdentity {
    uint64_t size = 0;
    uint64_t lastWriteUtc = 0;   ///< FILETIME as uint64 (100 ns since 1601)
    uint64_t creationUtc = 0;
    uint64_t fileIdLow = 0;
    uint64_t fileIdHigh = 0;
    uint32_t volumeSerial = 0;
    bool hasFileId = false;      ///< false on file systems without stable ids
    bool valid = false;

    /// Same volume + file id (or, without ids, same creation time + size).
    [[nodiscard]] bool sameFileAs(const FileIdentity& other) const noexcept;
};

/// Result of a write-deny probe on a file.
enum class OpenProbe {
    Ready,      ///< opened with deny-write: nobody is writing it
    Writing,    ///< ERROR_SHARING_VIOLATION: a writer holds it
    Missing,    ///< file does not exist
    Denied,     ///< access denied (permissions)
    Error,      ///< other error
};

/// Normalises to an absolute path and adds the \\?\ (or \\?\UNC\) prefix (POSIX: same as fullPath).
std::wstring toExtendedPath(std::wstring_view path);
/// Absolute, lexically normalised path without any \\?\ prefix (GetFullPathNameW
/// semantics: "." and ".." are resolved textually, symbolic links are left alone).
std::wstring fullPath(std::wstring_view path);

/// True when the path exists (file or directory).
bool exists(std::wstring_view path);
/// True when the path exists and is a directory.
bool isDirectory(std::wstring_view path);
/// True when the path exists and is a regular file.
bool isFile(std::wstring_view path);

/// Size of a file in bytes.
Result<uint64_t> fileSize(std::wstring_view path);
/// Last-write time (FILETIME as uint64) of a file.
Result<uint64_t> lastWriteUtc(std::wstring_view path);
/// Full identity (opens the file with share R/W/D, reads attributes, closes).
Result<FileIdentity> identity(std::wstring_view path);

/**
 * @brief Size, timestamps and kind of a path from one attribute query.
 */
struct FileAttributes {
    uint64_t size = 0;
    uint64_t lastWriteUtc = 0;   ///< FILETIME-style ticks
    uint64_t creationUtc = 0;    ///< FILETIME-style ticks (birth time on macOS)
    bool isDirectory = false;
};
/// Attributes without opening the file. A missing path fails with
/// ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND in Error::win32.
Result<FileAttributes> fileAttributes(std::wstring_view path);

/**
 * @brief A read-only file for positioned reads that never gets in a writer's way.
 *
 * Windows opens with FILE_SHARE_READ | WRITE | DELETE, so AME can keep
 * appending, rename or delete the file while we hold it. POSIX has no
 * sharing modes, which gives the same guarantee for free.
 */
class FileReader {
public:
    FileReader() = default;
    ~FileReader();
    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    /**
     * @brief Opens @p path for reading.
     * @param sequential  hint that reads walk forward (read-ahead), else random access
     */
    Result<void> open(std::wstring_view path, bool sequential = true);
    /// Closes the file (no-op when closed).
    void close() noexcept;
    /// True between a successful open() and close().
    [[nodiscard]] bool isOpen() const noexcept;

    /**
     * @brief Reads up to @p length bytes at @p offset.
     * @param got        receives the number of bytes read (0 at or past EOF)
     * @param lastError  receives the Win32-numbered error on failure
     * @return false on an I/O error; true otherwise (EOF included)
     */
    bool readAt(uint64_t offset, void* dst, size_t length, size_t& got, DWORD& lastError) noexcept;

    /// Current size of the open file.
    [[nodiscard]] Result<uint64_t> size() const;

private:
#if defined(_WIN32)
    UniqueHandle handle_;
#else
    int fd_ = -1;
#endif
};

/// Reads the whole file into memory (max @p maxBytes, default 64 MiB).
Result<std::vector<uint8_t>> readAll(std::wstring_view path, uint64_t maxBytes = 64ull * 1024 * 1024);
/// Reads @p length bytes starting at @p offset (fewer at EOF).
Result<std::vector<uint8_t>> readRange(std::wstring_view path, uint64_t offset, size_t length);

/// Writes bytes to a temp file next to @p path then MoveFileEx-replaces it.
Result<void> writeAllAtomic(std::wstring_view path, std::string_view bytes);
/// Appends bytes to a file (creates it if missing).
Result<void> appendAll(std::wstring_view path, std::string_view bytes);

/// mkdir -p.
Result<void> createDirectories(std::wstring_view path);
/// Deletes a file (success when it is already gone).
Result<void> deleteFile(std::wstring_view path);
/// Moves/renames, replacing the destination, write-through.
Result<void> moveReplace(std::wstring_view from, std::wstring_view to);
/// Moves/renames but fails when the destination exists.
Result<void> moveNoReplace(std::wstring_view from, std::wstring_view to);
/// Copies a file (fails when the destination exists unless @p overwrite).
Result<void> copyFile(std::wstring_view from, std::wstring_view to, bool overwrite);

/**
 * @brief "Is anyone writing this file right now?"
 *
 * Windows opens for read with share READ|DELETE (deny write) and closes again.
 * macOS has no sharing modes, so it asks the kernel which processes hold the
 * file open and whether any of those descriptors was opened for writing
 * (libproc), which answers the same question without taking any lock.
 */
OpenProbe probeDenyWrite(std::wstring_view path, DWORD* lastError = nullptr);

/// Lists entries of a directory (names only, no "." / ".."), non-recursive.
struct DirEntry {
    std::wstring name;
    bool isDirectory = false;
    uint64_t size = 0;
    uint64_t lastWriteUtc = 0;
    DWORD attributes = 0;
};
Result<std::vector<DirEntry>> listDirectory(std::wstring_view dir);

/// Volume root for a path ("C:\", "\\server\share\").
Result<std::wstring> volumeRoot(std::wstring_view path);
/// True when the path lives on a network (DRIVE_REMOTE) volume.
bool isRemotePath(std::wstring_view path);

} // namespace hh::platform
