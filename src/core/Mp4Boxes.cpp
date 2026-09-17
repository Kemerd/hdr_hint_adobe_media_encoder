// ---------------------------------------------------------------------------
// Mp4Boxes.cpp - minimal ISO-BMFF box walker (headers only).
//
// The walker never loads the media data: it reads box headers at increasing
// offsets, checks that every box fits inside its container and that the
// top-level chain lands exactly on EOF. That is enough to tell a finished
// AME export from one that is still being assembled (the trailing box is
// either missing or overshoots the current file size while AME writes it).
//
// Both the file-backed and the in-memory entry points share one walker that
// talks to a tiny ByteSource abstraction (size + read(offset, dst, len)).
// ---------------------------------------------------------------------------
#include "core/Mp4Boxes.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Handle.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace hh {

namespace {

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"Mp4";

/// Size of a plain box header (size + type).
constexpr uint64_t kCompactHeader = 8;
/// Size of a box header that carries a 64-bit largesize.
constexpr uint64_t kLargeHeader = 16;

/// Sanity cap on the number of top-level boxes we are willing to walk.
constexpr size_t kMaxTopLevelBoxes = 4096;
/// Sanity cap on the number of direct moov children we walk looking for mvhd.
constexpr size_t kMaxMoovChildren = 1024;
/// We never look further than this into a moov box (the spec cap).
constexpr uint64_t kMaxMoovScanBytes = 64ull * 1024 * 1024;

/// mvhd payload sizes (after the 8/16-byte box header) for the two versions.
constexpr uint64_t kMvhdV0PayloadBytes = 20;   ///< version+flags, ctime, mtime, timescale, duration(u32)
constexpr uint64_t kMvhdV1PayloadBytes = 32;   ///< version+flags, ctime(u64), mtime(u64), timescale, duration(u64)

/**
 * @brief Random-access byte source the walker reads from.
 *
 * read() must fill exactly @p len bytes at @p offset and return false on any
 * failure (including a short read). The walker only ever asks for a few
 * bytes at a time, so an implementation may be as simple as memcpy.
 */
struct ByteSource {
    uint64_t size = 0;
    std::function<bool(uint64_t offset, uint8_t* dst, size_t len)> read;
};

/// Outcome of parsing one box header.
enum class HeaderStatus { Ok, Truncated, Corrupt, ReadError };

/**
 * @brief Big-endian 32-bit read from a buffer that is known to hold 4 bytes.
 */
uint32_t readU32BE(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/**
 * @brief Big-endian 64-bit read from a buffer that is known to hold 8 bytes.
 */
uint64_t readU64BE(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(readU32BE(p)) << 32) | static_cast<uint64_t>(readU32BE(p + 4));
}

/**
 * @brief Renders a 4cc for log/error text, replacing non-printable bytes.
 *
 * QuickTime user-data atoms start with 0xA9 ("©"), which is fine to keep as a
 * placeholder; anything outside printable ASCII becomes '?' so the log stays
 * clean regardless of what a corrupt file contains.
 */
std::wstring displayType(const std::string& type) {
    std::wstring out;
    out.reserve(4);
    for (const char c : type) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back((u >= 0x20 && u <= 0x7E) ? static_cast<wchar_t>(u) : L'?');
    }
    return out;
}

/**
 * @brief Reads one box header at @p offset inside a container [offset, limit).
 *
 * Handles the three size encodings: a plain 32-bit size, size==1 with a
 * 64-bit largesize following the type, and size==0 meaning "to the end of
 * the container". Every arithmetic step is overflow-safe because the sizes
 * come from the file and cannot be trusted.
 *
 * @param src        where to read from
 * @param offset     absolute offset of the header
 * @param limit      end of the enclosing container (file size for top level)
 * @param boundName  what @p limit represents, for error text ("EOF", "moov")
 * @param out        receives the parsed header
 * @param error      receives a human explanation when the status is not Ok
 */
HeaderStatus readBoxHeader(const ByteSource& src, uint64_t offset, uint64_t limit, const wchar_t* boundName,
                           Mp4Box& out, std::wstring& error) {
    // A header needs at least 8 bytes inside the container.
    if (offset > limit || limit - offset < kCompactHeader) {
        error = std::format(L"truncated box header at offset {} ({} bytes before {})", offset,
                            (offset > limit) ? 0ull : (limit - offset), boundName);
        return HeaderStatus::Truncated;
    }
    if (!src.read) {
        error = L"no byte source";
        return HeaderStatus::ReadError;
    }

    // Pull the compact header: 32-bit size followed by the 4cc type.
    uint8_t header[kCompactHeader] = {};
    if (!src.read(offset, header, sizeof(header))) {
        error = std::format(L"read failed at offset {}", offset);
        return HeaderStatus::ReadError;
    }
    const uint32_t size32 = readU32BE(header);
    out.type.assign(reinterpret_cast<const char*>(header + 4), 4);
    out.offset = offset;
    out.headerSize = kCompactHeader;

    // Resolve the three size encodings into an absolute byte count.
    if (size32 == 1) {
        // largesize: another 8 bytes must fit inside the container.
        if (limit - offset < kLargeHeader) {
            error = std::format(L"truncated largesize for box '{}' at offset {}", displayType(out.type), offset);
            return HeaderStatus::Truncated;
        }
        uint8_t large[8] = {};
        if (!src.read(offset + kCompactHeader, large, sizeof(large))) {
            error = std::format(L"read failed at offset {}", offset + kCompactHeader);
            return HeaderStatus::ReadError;
        }
        out.size = readU64BE(large);
        out.headerSize = kLargeHeader;
    } else if (size32 == 0) {
        // Zero means "this box runs to the end of the container".
        out.size = limit - offset;
    } else {
        out.size = static_cast<uint64_t>(size32);
    }

    // A box can never be smaller than its own header.
    if (out.size < out.headerSize) {
        error = std::format(L"corrupt box size {} for '{}' at offset {}", out.size, displayType(out.type), offset);
        return HeaderStatus::Corrupt;
    }

    // The box must end inside the container (overflow-safe comparison).
    if (out.size > limit - offset) {
        error = std::format(L"box '{}' extends past {} (needs {} more bytes)", displayType(out.type), boundName,
                            out.size - (limit - offset));
        return HeaderStatus::Truncated;
    }
    return HeaderStatus::Ok;
}

/**
 * @brief Walks the direct children of a moov box and reads mvhd's timescale
 *        and duration into @p layout.
 *
 * Only headers are read; every child is skipped by its size. The scan stops
 * after kMaxMoovScanBytes or kMaxMoovChildren so a hostile file cannot make
 * the probe thread crawl through hundreds of megabytes.
 *
 * @return false when a read error occurred (the caller decides what to do)
 */
bool readMvhd(const ByteSource& src, const Mp4Box& moov, Mp4Layout& layout) {
    // Payload bounds of the moov box (absolute offsets).
    const uint64_t payloadStart = moov.offset + moov.headerSize;
    const uint64_t payloadEnd = moov.offset + moov.size;
    if (payloadStart > payloadEnd) {
        return true;   // cannot happen after readBoxHeader validated it, but stay safe
    }

    // Limit how far into the moov we are willing to look.
    const uint64_t scanEnd = std::min(payloadEnd, payloadStart + kMaxMoovScanBytes);

    // Walk child boxes until we find mvhd or run out of budget.
    uint64_t pos = payloadStart;
    size_t children = 0;
    while (pos < scanEnd) {
        if (++children > kMaxMoovChildren) {
            HH_LOG_DEBUG(kLog, L"moov has more than {} children; giving up on mvhd", kMaxMoovChildren);
            return true;
        }

        // Children are bounded by the real payload end, not by our scan cap.
        Mp4Box child;
        std::wstring err;
        const HeaderStatus status = readBoxHeader(src, pos, payloadEnd, L"moov", child, err);
        if (status == HeaderStatus::ReadError) {
            HH_LOG_DEBUG(kLog, L"mvhd scan: {}", err);
            return false;
        }
        if (status != HeaderStatus::Ok) {
            // A malformed child does not make the file incomplete; it only means no duration.
            HH_LOG_DEBUG(kLog, L"mvhd scan stopped: {}", err);
            return true;
        }

        if (child.type == "mvhd") {
            // The fullbox payload starts right after the child's header.
            const uint64_t bodyStart = child.offset + child.headerSize;
            const uint64_t bodyBytes = child.size - child.headerSize;
            if (bodyBytes < kMvhdV0PayloadBytes) {
                HH_LOG_DEBUG(kLog, L"mvhd payload too small ({} bytes)", bodyBytes);
                return true;
            }

            // Read the largest layout we may need; version 1 is 32 bytes.
            uint8_t body[kMvhdV1PayloadBytes] = {};
            const size_t want = static_cast<size_t>(std::min<uint64_t>(bodyBytes, kMvhdV1PayloadBytes));
            if (!src.read(bodyStart, body, want)) {
                HH_LOG_DEBUG(kLog, L"mvhd read failed at offset {}", bodyStart);
                return false;
            }

            // version(1) + flags(3) come first in every fullbox.
            const uint8_t version = body[0];
            uint32_t timescale = 0;
            uint64_t duration = 0;
            if (version == 0) {
                timescale = readU32BE(body + 12);
                duration = static_cast<uint64_t>(readU32BE(body + 16));
            } else if (version == 1) {
                if (want < kMvhdV1PayloadBytes) {
                    HH_LOG_DEBUG(kLog, L"mvhd version 1 payload too small ({} bytes)", bodyBytes);
                    return true;
                }
                timescale = readU32BE(body + 20);
                duration = readU64BE(body + 24);
            } else {
                HH_LOG_DEBUG(kLog, L"unknown mvhd version {}", static_cast<unsigned>(version));
                return true;
            }

            // Record what we found; a zero timescale leaves the duration at 0.
            layout.timescale = timescale;
            if (timescale != 0) {
                layout.durationSec = static_cast<double>(duration) / static_cast<double>(timescale);
            }
            return true;
        }

        // Skip to the next sibling (size validated to be >= headerSize > 0).
        pos += child.size;
    }
    return true;
}

/**
 * @brief Walks the top-level box chain of @p src and fills a layout.
 *
 * @param src        byte source (file or memory)
 * @param ioFailure  set to true when the walk stopped because read() failed
 */
Mp4Layout walkBoxes(const ByteSource& src, bool& ioFailure) {
    Mp4Layout layout;
    layout.fileSize = src.size;
    ioFailure = false;

    // An empty file has no boxes at all; say so instead of "chain mismatch".
    if (src.size == 0) {
        layout.error = L"empty file";
        return layout;
    }

    // Walk box by box until the chain reaches EOF or something is wrong.
    uint64_t pos = 0;
    size_t count = 0;
    while (pos < src.size) {
        if (++count > kMaxTopLevelBoxes) {
            layout.error = std::format(L"more than {} top-level boxes", kMaxTopLevelBoxes);
            return layout;
        }

        // Parse the header; any failure ends the walk with an explanation.
        Mp4Box box;
        std::wstring err;
        const HeaderStatus status = readBoxHeader(src, pos, src.size, L"EOF", box, err);
        if (status != HeaderStatus::Ok) {
            layout.error = err;
            ioFailure = (status == HeaderStatus::ReadError);
            return layout;
        }
        layout.topLevel.push_back(box);

        // Track the two boxes we care about; everything else is skipped.
        if (box.type == "moov") {
            if (!layout.hasMoov && !layout.hasMdat) {
                layout.moovFirst = true;
            }
            layout.hasMoov = true;
            if (!readMvhd(src, box, layout)) {
                layout.error = std::format(L"read failed inside moov at offset {}", box.offset);
                ioFailure = true;
                return layout;
            }
        } else if (box.type == "mdat") {
            layout.hasMdat = true;
            layout.mdatPayload += box.size - box.headerSize;
        }

        // Advance; readBoxHeader guaranteed pos + size <= fileSize.
        pos += box.size;
    }

    // The loop only exits cleanly when the chain ends exactly at EOF.
    if (pos != src.size) {
        layout.error = std::format(L"box chain ends at {} but the file is {} bytes", pos, src.size);
        return layout;
    }
    if (!layout.hasMoov) {
        layout.error = L"no moov box (file not finalized)";
        return layout;
    }
    if (!layout.hasMdat) {
        layout.error = L"no mdat box";
        return layout;
    }

    // Everything lines up: moov + mdat present and the chain lands on EOF.
    layout.complete = true;
    return layout;
}

/**
 * @brief Positions the file pointer and reads exactly @p len bytes.
 *
 * ReadFile may legitimately return fewer bytes than requested, so the read
 * loops until the request is satisfied or the file reports EOF / an error.
 *
 * @param lastError receives GetLastError() on failure (0 on a short read)
 */
bool readExact(HANDLE file, uint64_t offset, uint8_t* dst, size_t len, DWORD& lastError) {
    lastError = 0;
    if (file == nullptr || file == INVALID_HANDLE_VALUE || dst == nullptr) {
        lastError = ERROR_INVALID_HANDLE;
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (offset > static_cast<uint64_t>(INT64_MAX)) {
        lastError = ERROR_INVALID_PARAMETER;
        return false;
    }

    // Seek to the absolute offset first.
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
        lastError = ::GetLastError();
        return false;
    }

    // Read until the whole request is satisfied.
    size_t done = 0;
    while (done < len) {
        const size_t remaining = len - done;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD got = 0;
        if (!::ReadFile(file, dst + done, chunk, &got, nullptr)) {
            lastError = ::GetLastError();
            return false;
        }
        if (got == 0) {
            // EOF before we got everything: a short read.
            return false;
        }
        done += static_cast<size_t>(got);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/**
 * @brief Walks the top-level boxes of a file and reads mvhd for the duration.
 *
 * The file is opened with full sharing so a probe never interferes with AME
 * (which still holds the file while finalizing) or with mkvmerge reading a
 * previous generation of the same export.
 */
Result<Mp4Layout> inspectMp4(const std::wstring& path) {
    // Refuse obviously bad input up front.
    if (path.empty()) {
        return Error::text(L"inspectMp4: empty path");
    }

    // Open read-only with share R|W|D; the \\?\ prefix covers long paths.
    const std::wstring ext = platform::toExtendedPath(path);
    platform::UniqueHandle file(::CreateFileW(ext.c_str(), GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return Error::fromLastError(std::format(L"open '{}'", path));
    }

    // The size decides where the top-level chain must end.
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) {
        return Error::fromLastError(std::format(L"size of '{}'", path));
    }
    if (size.QuadPart < 0) {
        return Error::text(std::format(L"negative size reported for '{}'", path));
    }

    // Wire the walker to ReadFile; the last Win32 error is kept for the report.
    DWORD readError = 0;
    const HANDLE raw = file.get();
    ByteSource src;
    src.size = static_cast<uint64_t>(size.QuadPart);
    src.read = [raw, &readError](uint64_t offset, uint8_t* dst, size_t len) -> bool {
        DWORD err = 0;
        const bool ok = readExact(raw, offset, dst, len, err);
        if (!ok && err != 0) {
            readError = err;
        }
        return ok;
    };

    // Walk; a genuine I/O failure becomes an Error, a structural problem a layout.
    bool ioFailure = false;
    Mp4Layout layout = walkBoxes(src, ioFailure);
    if (ioFailure) {
        if (readError != 0) {
            return Error::fromWin32(readError, std::format(L"read '{}': {}", path, layout.error));
        }
        return Error::text(std::format(L"read '{}': {}", path, layout.error));
    }

    HH_LOG_DEBUG(kLog, L"'{}': {} bytes, {} top-level boxes, moov={} mdat={} moovFirst={} complete={} {}",
                 path, layout.fileSize, layout.topLevel.size(), layout.hasMoov, layout.hasMdat, layout.moovFirst,
                 layout.complete, layout.error);
    return layout;
}

/**
 * @brief Walks boxes from an in-memory buffer (for tests). fileSize = buffer size.
 */
Mp4Layout inspectMp4Buffer(const std::vector<uint8_t>& bytes) {
    // Memory-backed source: a bounds-checked memcpy.
    const uint8_t* data = bytes.data();
    const uint64_t total = static_cast<uint64_t>(bytes.size());
    ByteSource src;
    src.size = total;
    src.read = [data, total](uint64_t offset, uint8_t* dst, size_t len) -> bool {
        if (dst == nullptr) {
            return false;
        }
        if (len == 0) {
            return true;
        }
        // Reject reads that start or end outside the buffer (overflow-safe).
        if (data == nullptr || offset > total || static_cast<uint64_t>(len) > total - offset) {
            return false;
        }
        std::memcpy(dst, data + offset, len);
        return true;
    };

    // The memory reader cannot fail for in-range reads, so ioFailure only
    // reflects a bug; report it through the layout error text.
    bool ioFailure = false;
    Mp4Layout layout = walkBoxes(src, ioFailure);
    if (ioFailure && layout.error.empty()) {
        layout.error = L"buffer read failed";
    }
    return layout;
}

} // namespace hh
