// ---------------------------------------------------------------------------
// Mp4Boxes.h - minimal ISO-BMFF box walker (headers only, no sample parsing).
//
// Used to decide whether an .mp4/.mov is structurally complete: the top-level
// box chain must end exactly at EOF and contain both moov and mdat.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hh {

struct Mp4Box {
    std::string type;        ///< 4cc
    uint64_t offset = 0;     ///< absolute offset of the header
    uint64_t size = 0;       ///< total size incl. header (0 = to EOF, already resolved)
    uint64_t headerSize = 8; ///< 8 or 16 (largesize)
};

struct Mp4Layout {
    bool complete = false;         ///< chain ends exactly at EOF, moov + mdat present
    bool hasMoov = false;
    bool hasMdat = false;
    bool moovFirst = false;        ///< faststart layout (moov before mdat)
    uint64_t fileSize = 0;
    uint64_t mdatPayload = 0;      ///< sum of mdat payload bytes
    double durationSec = 0.0;      ///< from mvhd (0 when not found)
    uint32_t timescale = 0;
    std::vector<Mp4Box> topLevel;
    std::wstring error;            ///< why it is not complete
};

/// Walks the top-level boxes of a file and reads mvhd for the duration.
Result<Mp4Layout> inspectMp4(const std::wstring& path);

/// Walks boxes from an in-memory buffer (for tests). @p fileSize = buffer size.
Mp4Layout inspectMp4Buffer(const std::vector<uint8_t>& bytes);

} // namespace hh
