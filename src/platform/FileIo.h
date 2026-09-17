// ---------------------------------------------------------------------------
// FileIo.h - small, defensive file helpers built on the wide Win32 APIs.
//
// All paths are accepted as ordinary Win32 paths; helpers apply the \\?\
// prefix internally where an API needs it (see toExtendedPath).
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

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

/// Normalises to an absolute path and adds the \\?\ (or \\?\UNC\) prefix.
std::wstring toExtendedPath(std::wstring_view path);
/// Absolute path without any \\?\ prefix (GetFullPathNameW).
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

/// Opens for read with share READ|DELETE (deny write) and closes again.
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
