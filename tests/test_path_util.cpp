// ---------------------------------------------------------------------------
// test_path_util.cpp - the HdrHint path rules: job keys, hint/partial names,
// "(2)" conflict names, AME sidecar and scratch-file recognition.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"

#include <string>
#include <vector>

namespace path = hh::path;
namespace platform = hh::platform;

// ---- keys -------------------------------------------------------------------------

HH_TEST(PathUtil_normalizeKeyUpperCasesAndCleans) {
    // Upper-case, backslashes, no \\?\ prefix, "." / ".." resolved.
    CHECK_WEQ(path::normalizeKey(L"c:\\Out\\clip.mp4"), L"C:\\OUT\\CLIP.MP4");
    CHECK_WEQ(path::normalizeKey(L"\\\\?\\C:\\out\\clip.mp4"), L"C:\\OUT\\CLIP.MP4");
    CHECK_WEQ(path::normalizeKey(L"C:/out/clip.mp4"), L"C:\\OUT\\CLIP.MP4");
    CHECK_WEQ(path::normalizeKey(L"C:\\out\\..\\clip.mp4"), L"C:\\CLIP.MP4");
    CHECK_WEQ(path::normalizeKey(L"C:\\out\\.\\clip.mp4"), L"C:\\OUT\\CLIP.MP4");
    // Same file, different spellings: one key.
    CHECK_WEQ(path::normalizeKey(L"B:\\YouTube Renders\\clip.mp4"), path::normalizeKey(L"b:/youtube renders/CLIP.MP4"));
    // Empty stays empty rather than turning into the current directory.
    CHECK(path::normalizeKey(L"").empty());
}

HH_TEST(PathUtil_normalizeKeyResolvesRelativePaths) {
    // A relative path is anchored on the current directory, exactly like
    // GetFullPathNameW does it, then upper-cased.
    const std::wstring key = path::normalizeKey(L"rel\\clip.mp4");
    const std::wstring expected = platform::toUpperInvariant(platform::fullPath(L"rel\\clip.mp4"));
    CHECK_WEQ(key, expected);
    CHECK(key.size() > 3);
    // Absolute: drive letter or UNC.
    const bool driveStyle = key.size() > 2 && key[1] == L':' && key[2] == L'\\';
    const bool uncStyle = key.rfind(L"\\\\", 0) == 0;
    CHECK(driveStyle || uncStyle);
}

// ---- name parts -----------------------------------------------------------------

HH_TEST(PathUtil_fileNameStemExtension) {
    CHECK_WEQ(path::fileName(L"C:\\a\\b\\clip.mp4"), L"clip.mp4");
    CHECK_WEQ(path::fileName(L"C:/a/b/clip.mp4"), L"clip.mp4");
    CHECK_WEQ(path::fileName(L"clip.mp4"), L"clip.mp4");
    CHECK(path::fileName(L"").empty());

    CHECK_WEQ(path::stem(L"clip.mp4"), L"clip");
    CHECK_WEQ(path::stem(L"clip.tar.gz"), L"clip.tar");
    CHECK_WEQ(path::stem(L"C:\\a\\clip.mp4"), L"clip");
    CHECK_WEQ(path::stem(L"noext"), L"noext");

    CHECK_WEQ(path::extension(L"clip.mp4"), L".mp4");
    CHECK_WEQ(path::extension(L"CLIP.MP4"), L".mp4");
    CHECK_WEQ(path::extension(L"C:\\a\\clip.tar.gz"), L".gz");
    CHECK(path::extension(L"noext").empty());
    // A dot inside a folder name is not an extension of the file.
    CHECK(path::extension(L"C:\\dir.v2\\noext").empty());
}

HH_TEST(PathUtil_parentAndJoin) {
    CHECK_WEQ(path::parent(L"C:\\a\\b\\clip.mp4"), L"C:\\a\\b");
    CHECK_WEQ(path::parent(L"C:\\clip.mp4"), L"C:\\");
    CHECK_WEQ(path::parent(L"C:\\"), L"C:\\");
    CHECK(path::parent(L"clip.mp4").empty());

    CHECK_WEQ(path::join(L"C:\\a", L"b.mp4"), L"C:\\a\\b.mp4");
    CHECK_WEQ(path::join(L"C:\\a\\", L"b.mp4"), L"C:\\a\\b.mp4");
    CHECK_WEQ(path::join(L"C:\\", L"b.mp4"), L"C:\\b.mp4");
    CHECK_WEQ(path::join(L"C:\\a", L"\\b.mp4"), L"C:\\a\\b.mp4");
}

HH_TEST(PathUtil_normalizeSeparators) {
    CHECK_WEQ(path::normalizeSeparators(L"C:/a//b\\\\c"), L"C:\\a\\b\\c");
    CHECK_WEQ(path::normalizeSeparators(L"C:\\a\\b"), L"C:\\a\\b");
    // UNC keeps its double leading backslash.
    CHECK_WEQ(path::normalizeSeparators(L"\\\\server\\share\\x"), L"\\\\server\\share\\x");
    CHECK_WEQ(path::normalizeSeparators(L"//server/share//x"), L"\\\\server\\share\\x");
    CHECK(path::normalizeSeparators(L"").empty());
}

HH_TEST(PathUtil_hasExtension) {
    const std::vector<std::wstring> list = {L".mp4", L".mov", L".m4v"};
    CHECK(path::hasExtension(L"C:\\x\\clip.mp4", list));
    CHECK(path::hasExtension(L"clip.MP4", list));
    CHECK(path::hasExtension(L"clip.Mov", list));
    CHECK_FALSE(path::hasExtension(L"clip.mkv", list));
    CHECK_FALSE(path::hasExtension(L"clip", list));
    CHECK_FALSE(path::hasExtension(L"clip.mp4", {}));
}

// ---- hint / partial names -----------------------------------------------------------

HH_TEST(PathUtil_hintPathFor) {
    // Next to the source by default.
    CHECK_WEQ(path::hintPathFor(L"C:\\out\\clip.mp4", L"_REC709_HINT", L""), L"C:\\out\\clip_REC709_HINT.mkv");
    // A stem that already carries the suffix is not doubled.
    CHECK_WEQ(path::hintPathFor(L"C:\\out\\clip_REC709_HINT.mp4", L"_REC709_HINT", L""), L"C:\\out\\clip_REC709_HINT.mkv");
    // Custom output folder (with and without a trailing separator).
    CHECK_WEQ(path::hintPathFor(L"C:\\out\\clip.mp4", L"_X", L"D:\\hints"), L"D:\\hints\\clip_X.mkv");
    CHECK_WEQ(path::hintPathFor(L"C:\\out\\clip.mp4", L"_X", L"D:\\hints\\"), L"D:\\hints\\clip_X.mkv");
    // .mov sources become .mkv too; an empty suffix is just "<stem>.mkv".
    CHECK_WEQ(path::hintPathFor(L"C:\\out\\master.mov", L"_X", L""), L"C:\\out\\master_X.mkv");
    CHECK_WEQ(path::hintPathFor(L"C:\\out\\clip.mp4", L"", L""), L"C:\\out\\clip.mkv");
}

HH_TEST(PathUtil_firstFreePath) {
    hh::test::ScratchDir dir(L"firstfree");
    if (!CHECK(dir.valid())) { return; }
    const std::wstring base = dir.file(L"clip_REC709_HINT.mkv");

    // Nothing there yet: the name is free as-is.
    CHECK_WEQ(path::firstFreePath(base), base);

    // Occupied: " (2)" goes before the extension.
    CHECK(dir.writeFile(L"clip_REC709_HINT.mkv", "x"));
    CHECK_WEQ(path::firstFreePath(base), dir.file(L"clip_REC709_HINT (2).mkv"));

    // " (2)" occupied as well: " (3)".
    CHECK(dir.writeFile(L"clip_REC709_HINT (2).mkv", "x"));
    CHECK_WEQ(path::firstFreePath(base), dir.file(L"clip_REC709_HINT (3).mkv"));
}

HH_TEST(PathUtil_isOurOutput) {
    CHECK(path::isOurOutput(L"clip_REC709_HINT.mkv", L"_REC709_HINT"));
    CHECK(path::isOurOutput(L"clip_rec709_hint.MKV", L"_REC709_HINT"));
    CHECK(path::isOurOutput(L"C:\\out\\clip_REC709_HINT.mkv", L"_REC709_HINT"));
    CHECK_FALSE(path::isOurOutput(L"clip.mkv", L"_REC709_HINT"));
    CHECK_FALSE(path::isOurOutput(L"clip_REC709_HINT.mp4", L"_REC709_HINT"));
    CHECK_FALSE(path::isOurOutput(L"clip_REC709_HINTx.mkv", L"_REC709_HINT"));
    CHECK_FALSE(path::isOurOutput(L"", L"_REC709_HINT"));
}

HH_TEST(PathUtil_partialOutputs) {
    const std::wstring hint = L"C:\\x\\clip_REC709_HINT.mkv";
    const std::wstring partial = path::partialPathFor(hint);
    // The partial swaps the final ".mkv" for ".hdrhint-partial.mkv" so mkvmerge still writes Matroska.
    CHECK_WEQ(partial, L"C:\\x\\clip_REC709_HINT.hdrhint-partial.mkv");
    CHECK(partial.size() >= 4 && partial.compare(partial.size() - 4, 4, L".mkv") == 0);

    CHECK(path::isPartialOutput(path::fileName(partial)));
    CHECK(path::isPartialOutput(L"CLIP.MKV.HDRHINT-PARTIAL.MKV"));
    CHECK_FALSE(path::isPartialOutput(L"clip_REC709_HINT.mkv"));
    CHECK_FALSE(path::isPartialOutput(L"clip.hdrhint-partial.mp4"));
    CHECK_FALSE(path::isPartialOutput(L""));
}

// ---- AME temp files ----------------------------------------------------------------

HH_TEST(PathUtil_parseSidecarValid) {
    const auto s = path::parseSidecar(L"clip.12345.6789.m4v");
    if (CHECK(s.has_value())) {
        CHECK_WEQ(s->stem, L"clip");
        CHECK_EQ(s->pid, 12345u);
        CHECK_EQ(s->tid, 6789u);
        CHECK_WEQ(s->ext, L"m4v");
    }

    const auto a = path::parseSidecar(L"My Episode 04.30112.51004.aac");
    if (CHECK(a.has_value())) {
        CHECK_WEQ(a->stem, L"My Episode 04");
        CHECK_EQ(a->pid, 30112u);
        CHECK_EQ(a->tid, 51004u);
        CHECK_WEQ(a->ext, L"aac");
    }

    // Dots in the stem: only the last three components matter.
    const auto d = path::parseSidecar(L"a.b.c.100.200.m4v");
    if (CHECK(d.has_value())) {
        CHECK_WEQ(d->stem, L"a.b.c");
        CHECK_EQ(d->pid, 100u);
        CHECK_EQ(d->tid, 200u);
    }
}

HH_TEST(PathUtil_parseSidecarFalsePositiveIsParserNotWatcher) {
    // "Show.2024.06.m4v" is a plain media name that happens to fit the shape.
    // The parser is purely syntactic: it must return pid 2024 / tid 6, and the
    // watcher is the layer that rejects it (no live process with that pid).
    const auto s = path::parseSidecar(L"Show.2024.06.m4v");
    if (CHECK(s.has_value())) {
        CHECK_WEQ(s->stem, L"Show");
        CHECK_EQ(s->pid, 2024u);
        CHECK_EQ(s->tid, 6u);
        CHECK_WEQ(s->ext, L"m4v");
    }
}

HH_TEST(PathUtil_parseSidecarInvalid) {
    CHECK_FALSE(path::parseSidecar(L"clip.mp4").has_value());
    CHECK_FALSE(path::parseSidecar(L"clip.12345.m4v").has_value());
    CHECK_FALSE(path::parseSidecar(L"clip.abc.6789.m4v").has_value());
    CHECK_FALSE(path::parseSidecar(L"clip.12345.abc.m4v").has_value());
    CHECK_FALSE(path::parseSidecar(L"clip.12345.6789.txt").has_value());
    CHECK_FALSE(path::parseSidecar(L"clip.12345.6789.").has_value());
    CHECK_FALSE(path::parseSidecar(L".12345.6789.m4v").has_value());
    CHECK_FALSE(path::parseSidecar(L"12345.6789.m4v").has_value());
    CHECK_FALSE(path::parseSidecar(L"").has_value());
}

HH_TEST(PathUtil_isTemporaryName) {
    // AME's "<8hex>-<4hex>-<4hex>-<4hex>.tmp" scratch files, either case.
    CHECK(path::isTemporaryName(L"1a2b3c4d-1a2b-3c4d-5e6f.tmp"));
    CHECK(path::isTemporaryName(L"1A2B3C4D-1A2B-3C4D-5E6F.TMP"));
    CHECK(path::isTemporaryName(L"deadbeef-0000-ffff-1234.tmp"));
    // Other temp names.
    CHECK(path::isTemporaryName(L"render.tmp"));
    // Real media names are not temporary.
    CHECK_FALSE(path::isTemporaryName(L"clip.mp4"));
    CHECK_FALSE(path::isTemporaryName(L"clip_REC709_HINT.mkv"));
    CHECK_FALSE(path::isTemporaryName(L"1a2b3c4d-1a2b-3c4d-5e6f.mp4"));
    CHECK_FALSE(path::isTemporaryName(L""));
}

// ---- names for display / validation ----------------------------------------------

HH_TEST(PathUtil_hasInvalidFileNameChars) {
    CHECK_FALSE(path::hasInvalidFileNameChars(L"clip.mp4"));
    CHECK_FALSE(path::hasInvalidFileNameChars(L"My Episode (04) [final].mp4"));
    CHECK_FALSE(path::hasInvalidFileNameChars(L""));
    for (const wchar_t bad : {L'\\', L'/', L':', L'*', L'?', L'"', L'<', L'>', L'|'}) {
        const std::wstring name = std::wstring(L"a") + bad + L"b";
        CHECK(path::hasInvalidFileNameChars(name));
    }
}

HH_TEST(PathUtil_ellipsizeMiddle) {
    // Short enough: untouched.
    CHECK_WEQ(path::ellipsizeMiddle(L"clip.mp4", 20), L"clip.mp4");
    CHECK_WEQ(path::ellipsizeMiddle(L"clip.mp4", 8), L"clip.mp4");
    CHECK(path::ellipsizeMiddle(L"", 8).empty());

    // The documented example: 23 chars into 15 with a single U+2026.
    const std::wstring shortened = path::ellipsizeMiddle(L"very_long_file_name.mp4", 15);
    CHECK_WEQ(shortened, L"very_lo\u2026ame.mp4");

    // Never longer than the limit and always keeps head + tail.
    for (const size_t limit : {5u, 8u, 10u, 12u, 18u}) {
        const std::wstring s = path::ellipsizeMiddle(L"very_long_file_name.mp4", limit);
        CHECK(s.size() <= limit);
        CHECK(!s.empty() && s.find(L'\u2026') != std::wstring::npos);
        CHECK(!s.empty() && s.back() == L'4');
        CHECK(!s.empty() && s.front() == L'v');
    }
}
