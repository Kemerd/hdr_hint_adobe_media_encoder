// ---------------------------------------------------------------------------
// test_mp4_boxes.cpp - the ISO-BMFF box walker on synthetic buffers: the
// three size encodings, truncated and corrupt chains, mvhd v0/v1 duration,
// faststart detection and the file-backed entry point.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/Mp4Boxes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

namespace {

/// Appends a big-endian 32-bit value.
void putU32(Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

/// Appends a big-endian 64-bit value.
void putU64(Bytes& out, uint64_t v) {
    putU32(out, static_cast<uint32_t>(v >> 32));
    putU32(out, static_cast<uint32_t>(v & 0xFFFFFFFFull));
}

/// Appends a 4cc; anything shorter than four characters is space padded.
void putType(Bytes& out, const char* type) {
    const size_t len = type ? std::strlen(type) : 0;
    for (size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(i < len ? type[i] : ' '));
    }
}

/// Appends @p bytes to @p out.
void append(Bytes& out, const Bytes& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Appends a box whose 32-bit size field is written verbatim, so a test can
/// lie about the size (truncated, zero or corrupt boxes).
void appendBoxWithSizeField(Bytes& out, uint32_t sizeField, const char* type, const Bytes& payload) {
    putU32(out, sizeField);
    putType(out, type);
    append(out, payload);
}

/// Appends a well-formed box: 32-bit size (header + payload), 4cc, payload.
void appendBox(Bytes& out, const char* type, const Bytes& payload) {
    appendBoxWithSizeField(out, static_cast<uint32_t>(8 + payload.size()), type, payload);
}

/// Appends a box in the largesize encoding: size field 1, 4cc, 64-bit size.
void appendLargeBox(Bytes& out, const char* type, const Bytes& payload) {
    putU32(out, 1);
    putType(out, type);
    putU64(out, 16ull + payload.size());
    append(out, payload);
}

/// mvhd version 0 payload: version/flags, ctime, mtime, timescale,
/// 32-bit duration, then 80 zero bytes for the rest of the fields.
Bytes mvhdV0Payload(uint32_t timescale, uint32_t duration) {
    Bytes p;
    putU32(p, 0);            // version 0, flags 0
    putU32(p, 0);            // creation time
    putU32(p, 0);            // modification time
    putU32(p, timescale);    // +12
    putU32(p, duration);     // +16
    p.insert(p.end(), size_t{80}, static_cast<uint8_t>(0));
    return p;
}

/// mvhd version 1 payload: 64-bit times, timescale at +20, 64-bit duration
/// at +24, then 80 zero bytes.
Bytes mvhdV1Payload(uint32_t timescale, uint64_t duration) {
    Bytes p;
    putU32(p, 0x01000000u);  // version 1, flags 0
    putU64(p, 0);            // creation time
    putU64(p, 0);            // modification time
    putU32(p, timescale);    // +20
    putU64(p, duration);     // +24
    p.insert(p.end(), size_t{80}, static_cast<uint8_t>(0));
    return p;
}

/// A moov box wrapping one mvhd child with the given payload.
Bytes moovBox(const Bytes& mvhdPayload) {
    Bytes children;
    appendBox(children, "mvhd", mvhdPayload);
    Bytes out;
    appendBox(out, "moov", children);
    return out;
}

/// An 8-byte-payload ftyp box ("isom", minor version 0).
Bytes ftypBox() {
    Bytes payload = {'i', 's', 'o', 'm', 0, 0, 0, 0};
    Bytes out;
    appendBox(out, "ftyp", payload);
    return out;
}

/// ftyp + moov(mvhd v0, 600 / 6000) + mdat(1000 bytes): the canonical
/// complete faststart file used by several tests.
Bytes completeFile() {
    Bytes file;
    append(file, ftypBox());
    append(file, moovBox(mvhdV0Payload(600, 6000)));
    appendBox(file, "mdat", Bytes(size_t{1000}, static_cast<uint8_t>(0xAB)));
    return file;
}

/// Bytes -> std::string so ScratchDir::writeFile can store binary data.
std::string asString(const Bytes& bytes) {
    if (bytes.empty()) { return {}; }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

// ---- complete files --------------------------------------------------------------

/**
 * @brief ftyp + moov + mdat with matching sizes is complete, faststart, and
 *        reports the mvhd duration and the mdat payload.
 */
HH_TEST(Mp4_completeFaststartFile) {
    const Bytes file = completeFile();
    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);

    CHECK(layout.complete);
    CHECK(layout.error.empty());
    CHECK(layout.hasMoov);
    CHECK(layout.hasMdat);
    CHECK(layout.moovFirst);
    CHECK_EQ(layout.fileSize, file.size());
    CHECK_EQ(layout.mdatPayload, 1000u);
    CHECK_NEAR(layout.durationSec, 10.0, 1e-6);
    CHECK_EQ(layout.timescale, 600u);

    // Three top-level boxes at the expected offsets.
    if (CHECK_EQ(layout.topLevel.size(), 3)) {
        CHECK_EQ(layout.topLevel[0].type, "ftyp");
        CHECK_EQ(layout.topLevel[0].offset, 0u);
        CHECK_EQ(layout.topLevel[0].size, 16u);
        CHECK_EQ(layout.topLevel[0].headerSize, 8u);

        CHECK_EQ(layout.topLevel[1].type, "moov");
        CHECK_EQ(layout.topLevel[1].offset, 16u);
        CHECK_EQ(layout.topLevel[1].size, 116u);   // 8 + (8 + 100) mvhd

        CHECK_EQ(layout.topLevel[2].type, "mdat");
        CHECK_EQ(layout.topLevel[2].offset, 132u);
        CHECK_EQ(layout.topLevel[2].size, 1008u);
        CHECK_EQ(layout.topLevel[2].headerSize, 8u);
    }
}

/**
 * @brief An mdat whose size field says 0 runs to the end of the file; the
 *        payload is everything after its header.
 */
HH_TEST(Mp4_zeroSizeMdatRunsToEof) {
    Bytes file;
    append(file, ftypBox());
    append(file, moovBox(mvhdV0Payload(600, 6000)));
    appendBoxWithSizeField(file, 0, "mdat", Bytes(size_t{1234}, static_cast<uint8_t>(0x5A)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK(layout.complete);
    CHECK(layout.error.empty());
    CHECK(layout.hasMdat);
    CHECK(layout.moovFirst);
    CHECK_EQ(layout.mdatPayload, 1234u);
    if (CHECK_EQ(layout.topLevel.size(), 3)) {
        // The resolved size covers the header plus the remaining bytes.
        CHECK_EQ(layout.topLevel.back().type, "mdat");
        CHECK_EQ(layout.topLevel.back().size, 1242u);
        CHECK_EQ(layout.topLevel.back().headerSize, 8u);
    }
}

/**
 * @brief The largesize encoding (size field 1 + 64-bit size) is honoured:
 *        a 16-byte header and the right payload count.
 */
HH_TEST(Mp4_largesizeMdat) {
    Bytes file;
    append(file, ftypBox());
    append(file, moovBox(mvhdV0Payload(600, 6000)));
    appendLargeBox(file, "mdat", Bytes(size_t{777}, static_cast<uint8_t>(0x11)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK(layout.complete);
    CHECK(layout.error.empty());
    CHECK(layout.hasMdat);
    CHECK_EQ(layout.mdatPayload, 777u);
    if (CHECK_EQ(layout.topLevel.size(), 3)) {
        CHECK_EQ(layout.topLevel.back().type, "mdat");
        CHECK_EQ(layout.topLevel.back().headerSize, 16u);
        CHECK_EQ(layout.topLevel.back().size, 793u);
    }
}

/**
 * @brief mvhd version 1 keeps the timescale at +20 and a 64-bit duration at
 *        +24; a duration above 2^32 proves the high word is read.
 */
HH_TEST(Mp4_mvhdVersion1Duration) {
    const uint32_t timescale = 90000;
    const uint64_t duration = (1ull << 32) + 630000ull;

    Bytes file;
    append(file, ftypBox());
    append(file, moovBox(mvhdV1Payload(timescale, duration)));
    appendBox(file, "mdat", Bytes(size_t{16}, static_cast<uint8_t>(0)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK(layout.complete);
    CHECK_EQ(layout.timescale, timescale);
    CHECK_NEAR(layout.durationSec, static_cast<double>(duration) / static_cast<double>(timescale), 1e-6);
}

/**
 * @brief mdat ahead of moov is still complete but not a faststart layout.
 */
HH_TEST(Mp4_mdatBeforeMoovIsNotFaststart) {
    Bytes file;
    append(file, ftypBox());
    appendBox(file, "mdat", Bytes(size_t{500}, static_cast<uint8_t>(0xCD)));
    append(file, moovBox(mvhdV0Payload(600, 6000)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK(layout.complete);
    CHECK(layout.error.empty());
    CHECK(layout.hasMoov);
    CHECK(layout.hasMdat);
    CHECK_FALSE(layout.moovFirst);
    CHECK_EQ(layout.mdatPayload, 500u);
    CHECK_NEAR(layout.durationSec, 10.0, 1e-6);
    CHECK_EQ(layout.timescale, 600u);
}

/**
 * @brief Two mdat boxes add up; moov between them still counts as
 *        "not first".
 */
HH_TEST(Mp4_multipleMdatPayloadsAreSummed) {
    Bytes file;
    append(file, ftypBox());
    appendBox(file, "mdat", Bytes(size_t{300}, static_cast<uint8_t>(1)));
    append(file, moovBox(mvhdV0Payload(600, 6000)));
    appendBox(file, "mdat", Bytes(size_t{200}, static_cast<uint8_t>(2)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK(layout.complete);
    CHECK_FALSE(layout.moovFirst);
    CHECK_EQ(layout.mdatPayload, 500u);
    CHECK_EQ(layout.topLevel.size(), 4);
}

// ---- incomplete / corrupt files --------------------------------------------------

/**
 * @brief An mdat that declares more bytes than the file holds is what a
 *        still-writing export looks like: not complete, with a reason.
 */
HH_TEST(Mp4_mdatShortOfDeclaredSizeIsIncomplete) {
    Bytes file;
    append(file, ftypBox());
    append(file, moovBox(mvhdV0Payload(600, 6000)));
    // Header claims 2000 payload bytes; only 1000 follow.
    appendBoxWithSizeField(file, 2008, "mdat", Bytes(size_t{1000}, static_cast<uint8_t>(0)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK_FALSE(layout.complete);
    CHECK_FALSE(layout.error.empty());
    CHECK(layout.hasMoov);
    CHECK_EQ(layout.fileSize, file.size());
}

/**
 * @brief A size field smaller than the 8-byte header is corrupt and stops
 *        the walk.
 */
HH_TEST(Mp4_boxSmallerThanHeaderIsCorrupt) {
    Bytes file;
    append(file, ftypBox());
    append(file, moovBox(mvhdV0Payload(600, 6000)));
    appendBoxWithSizeField(file, 3, "mdat", Bytes(size_t{1000}, static_cast<uint8_t>(0)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK_FALSE(layout.complete);
    CHECK_FALSE(layout.error.empty());

    // Same for a size of 7 (one byte short of a header) and for size 1
    // without room for the 64-bit largesize.
    Bytes seven;
    append(seven, ftypBox());
    appendBoxWithSizeField(seven, 7, "mdat", {});
    CHECK_FALSE(hh::inspectMp4Buffer(seven).complete);
    CHECK_FALSE(hh::inspectMp4Buffer(seven).error.empty());

    Bytes noLarge;
    append(noLarge, ftypBox());
    appendBoxWithSizeField(noLarge, 1, "mdat", {});
    CHECK_FALSE(hh::inspectMp4Buffer(noLarge).complete);
    CHECK_FALSE(hh::inspectMp4Buffer(noLarge).error.empty());
}

/**
 * @brief A trailing partial header (fewer than 8 bytes left) means the
 *        chain does not land on EOF.
 */
HH_TEST(Mp4_trailingPartialHeaderIsIncomplete) {
    Bytes file = completeFile();
    file.push_back(0);
    file.push_back(0);
    file.push_back(0);

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK_FALSE(layout.complete);
    CHECK_FALSE(layout.error.empty());
    // The three good boxes were still walked before the stray bytes.
    CHECK(layout.hasMoov);
    CHECK(layout.hasMdat);
}

/**
 * @brief Missing moov, missing mdat and an empty buffer are all incomplete
 *        with an explanation.
 */
HH_TEST(Mp4_missingBoxesAreIncomplete) {
    // No moov: AME has not written the index yet.
    Bytes noMoov;
    append(noMoov, ftypBox());
    appendBox(noMoov, "mdat", Bytes(size_t{100}, static_cast<uint8_t>(0)));
    const hh::Mp4Layout a = hh::inspectMp4Buffer(noMoov);
    CHECK_FALSE(a.complete);
    CHECK_FALSE(a.hasMoov);
    CHECK(a.hasMdat);
    CHECK_FALSE(a.error.empty());

    // No mdat: an index without media.
    Bytes noMdat;
    append(noMdat, ftypBox());
    append(noMdat, moovBox(mvhdV0Payload(600, 6000)));
    const hh::Mp4Layout b = hh::inspectMp4Buffer(noMdat);
    CHECK_FALSE(b.complete);
    CHECK(b.hasMoov);
    CHECK_FALSE(b.hasMdat);
    CHECK_FALSE(b.error.empty());

    // Empty buffer.
    const hh::Mp4Layout c = hh::inspectMp4Buffer(Bytes{});
    CHECK_FALSE(c.complete);
    CHECK_FALSE(c.error.empty());
    CHECK(c.topLevel.empty());
    CHECK_EQ(c.fileSize, 0u);
}

/**
 * @brief A moov without mvhd is still structurally complete; it just has no
 *        duration.
 */
HH_TEST(Mp4_moovWithoutMvhdHasNoDuration) {
    Bytes children;
    appendBox(children, "udta", Bytes(size_t{12}, static_cast<uint8_t>(0)));
    Bytes file;
    append(file, ftypBox());
    appendBox(file, "moov", children);
    appendBox(file, "mdat", Bytes(size_t{10}, static_cast<uint8_t>(0)));

    const hh::Mp4Layout layout = hh::inspectMp4Buffer(file);
    CHECK(layout.complete);
    CHECK_EQ(layout.timescale, 0u);
    CHECK_NEAR(layout.durationSec, 0.0, 1e-12);
}

// ---- file-backed entry point ----------------------------------------------------

/**
 * @brief inspectMp4(path) on a file holding the complete buffer reports the
 *        same layout as the in-memory walker.
 */
HH_TEST(Mp4_inspectFileMatchesBuffer) {
    hh::test::ScratchDir dir(L"mp4");
    if (!CHECK(dir.valid())) { return; }

    const Bytes bytes = completeFile();
    if (!CHECK(dir.writeFile(L"clip.mp4", asString(bytes)))) { return; }

    const hh::Mp4Layout fromBuffer = hh::inspectMp4Buffer(bytes);
    const auto r = hh::inspectMp4(dir.file(L"clip.mp4"));
    if (!CHECK(r.ok())) { return; }
    const hh::Mp4Layout& f = r.value();

    CHECK_EQ(f.complete, fromBuffer.complete);
    CHECK(f.complete);
    CHECK_EQ(f.hasMoov, fromBuffer.hasMoov);
    CHECK_EQ(f.hasMdat, fromBuffer.hasMdat);
    CHECK_EQ(f.moovFirst, fromBuffer.moovFirst);
    CHECK_EQ(f.fileSize, fromBuffer.fileSize);
    CHECK_EQ(f.mdatPayload, fromBuffer.mdatPayload);
    CHECK_NEAR(f.durationSec, fromBuffer.durationSec, 1e-9);
    CHECK_EQ(f.timescale, fromBuffer.timescale);
    CHECK_WEQ(f.error, fromBuffer.error);
    if (CHECK_EQ(f.topLevel.size(), fromBuffer.topLevel.size())) {
        for (size_t i = 0; i < f.topLevel.size(); ++i) {
            CHECK_EQ(f.topLevel[i].type, fromBuffer.topLevel[i].type);
            CHECK_EQ(f.topLevel[i].offset, fromBuffer.topLevel[i].offset);
            CHECK_EQ(f.topLevel[i].size, fromBuffer.topLevel[i].size);
            CHECK_EQ(f.topLevel[i].headerSize, fromBuffer.topLevel[i].headerSize);
        }
    }

    // A truncated copy on disk is reported the same way as in memory.
    Bytes cut(bytes.begin(), bytes.end() - 100);
    if (CHECK(dir.writeFile(L"partial.mp4", asString(cut)))) {
        const auto p = hh::inspectMp4(dir.file(L"partial.mp4"));
        if (CHECK(p.ok())) {
            CHECK_FALSE(p.value().complete);
            CHECK_FALSE(p.value().error.empty());
            CHECK_EQ(p.value().fileSize, cut.size());
        }
    }
}

/**
 * @brief A missing file (and an empty path) is an Error, not a layout.
 */
HH_TEST(Mp4_inspectMissingFileIsAnError) {
    hh::test::ScratchDir dir(L"mp4_missing");
    if (!CHECK(dir.valid())) { return; }

    const auto r = hh::inspectMp4(dir.file(L"does_not_exist.mp4"));
    CHECK_FALSE(r.ok());
    if (!r.ok()) {
        CHECK_FALSE(r.error().message.empty());
        CHECK_EQ(r.error().win32, static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
    }

    const auto e = hh::inspectMp4(L"");
    CHECK_FALSE(e.ok());
    if (!e.ok()) {
        CHECK_FALSE(e.error().message.empty());
    }
}
