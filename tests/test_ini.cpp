// ---------------------------------------------------------------------------
// test_ini.cpp - IniFile: parse/serialize round trips that keep comments and
// order, quoted values, lists, booleans (including Python's "True") and the
// fallback behaviour for missing keys.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/IniFile.h"
#include "platform/Utf.h"

#include <string>
#include <vector>

using hh::IniFile;

namespace {

/// A representative settings file: preamble comment, two sections, an
/// in-section comment, a quoted value, a Python-style boolean and a list.
const wchar_t* const kSample =
    L"; HdrHint settings\n"
    L"\n"
    L"[app]\n"
    L"theme = dark\n"
    L"; the accent colour\n"
    L"accent = \"  blue  \"\n"
    L"first_run = True\n"
    L"\n"
    L"[watch]\n"
    L"extensions = .mp4|.mov|.m4v\n"
    L"debounce_ms = 500\n"
    L"ratio = 1.5\n";

/// CRLF -> LF and no trailing newlines, so a writer that prefers CRLF still
/// counts as a faithful round trip.
std::wstring normalizedText(std::wstring_view text) {
    std::wstring out = hh::platform::replaceAll(text, L"\r\n", L"\n");
    while (!out.empty() && out.back() == L'\n') { out.pop_back(); }
    return out;
}

} // namespace

// ---- parsing / lookup ----------------------------------------------------------------

HH_TEST(IniFile_parseAndGet) {
    IniFile ini;
    ini.parse(kSample);

    CHECK_WEQ(ini.get(L"app", L"theme"), L"dark");
    // Quoted values keep their inner whitespace verbatim.
    CHECK_WEQ(ini.get(L"app", L"accent"), L"  blue  ");
    CHECK_WEQ(ini.get(L"watch", L"extensions"), L".mp4|.mov|.m4v");

    // Keys and sections are case-insensitive.
    CHECK_WEQ(ini.get(L"APP", L"THEME"), L"dark");
    CHECK(ini.has(L"App", L"Theme"));

    // Missing key / section -> fallback, and has() is false.
    CHECK_WEQ(ini.get(L"app", L"missing", L"fallback"), L"fallback");
    CHECK_WEQ(ini.get(L"nope", L"theme", L"fb"), L"fb");
    CHECK(ini.get(L"app", L"missing").empty());
    CHECK_FALSE(ini.has(L"app", L"missing"));
    CHECK_FALSE(ini.has(L"nope", L"theme"));
}

HH_TEST(IniFile_sectionsAndKeysKeepOrder) {
    IniFile ini;
    ini.parse(kSample);

    const std::vector<std::wstring> sections = ini.sections();
    if (CHECK_EQ(sections.size(), 2)) {
        CHECK_WEQ(sections[0], L"app");
        CHECK_WEQ(sections[1], L"watch");
    }
    const std::vector<std::wstring> keys = ini.keys(L"app");
    if (CHECK_EQ(keys.size(), 3)) {
        CHECK_WEQ(keys[0], L"theme");
        CHECK_WEQ(keys[1], L"accent");
        CHECK_WEQ(keys[2], L"first_run");
    }
    CHECK(ini.keys(L"nope").empty());
}

HH_TEST(IniFile_roundTripPreservesCommentsAndOrder) {
    IniFile ini;
    ini.parse(kSample);
    const std::wstring out = ini.serialize();

    // Byte-for-byte apart from line-ending style.
    CHECK_WEQ(normalizedText(out), normalizedText(kSample));

    // And parsing the output again yields the same values (idempotent).
    IniFile again;
    again.parse(out);
    CHECK_WEQ(again.get(L"app", L"accent"), L"  blue  ");
    CHECK_WEQ(again.get(L"app", L"first_run"), L"True");
    CHECK_WEQ(normalizedText(again.serialize()), normalizedText(out));
}

HH_TEST(IniFile_booleans) {
    IniFile ini;
    ini.parse(L"[b]\n"
              L"py = True\n"
              L"pyf = False\n"
              L"yes = yes\n"
              L"no = no\n"
              L"one = 1\n"
              L"zero = 0\n"
              L"on = on\n"
              L"off = off\n"
              L"junk = maybe\n");

    CHECK(ini.getBool(L"b", L"py", false));
    CHECK_FALSE(ini.getBool(L"b", L"pyf", true));
    CHECK(ini.getBool(L"b", L"yes", false));
    CHECK_FALSE(ini.getBool(L"b", L"no", true));
    CHECK(ini.getBool(L"b", L"one", false));
    CHECK_FALSE(ini.getBool(L"b", L"zero", true));
    CHECK(ini.getBool(L"b", L"on", false));
    CHECK_FALSE(ini.getBool(L"b", L"off", true));

    // Junk and missing keys return the fallback, whichever it is.
    CHECK(ini.getBool(L"b", L"junk", true));
    CHECK_FALSE(ini.getBool(L"b", L"junk", false));
    CHECK(ini.getBool(L"b", L"missing", true));
    CHECK_FALSE(ini.getBool(L"b", L"missing", false));
}

HH_TEST(IniFile_numbers) {
    IniFile ini;
    ini.parse(kSample);
    ini.set(L"n", L"neg", L"-42");
    ini.set(L"n", L"junk", L"12abc");

    CHECK_EQ(ini.getInt(L"watch", L"debounce_ms", 0), 500);
    CHECK_EQ(ini.getInt(L"n", L"neg", 0), -42);
    CHECK_EQ(ini.getInt(L"n", L"junk", 7), 7);
    CHECK_EQ(ini.getInt(L"watch", L"missing", 9), 9);

    CHECK_NEAR(ini.getDouble(L"watch", L"ratio", 0.0), 1.5, 1e-9);
    CHECK_NEAR(ini.getDouble(L"watch", L"debounce_ms", 0.0), 500.0, 1e-9);
    CHECK_NEAR(ini.getDouble(L"n", L"junk", 2.5), 2.5, 1e-9);
    CHECK_NEAR(ini.getDouble(L"watch", L"missing", 3.25), 3.25, 1e-9);
}

HH_TEST(IniFile_lists) {
    IniFile ini;
    ini.parse(kSample);
    ini.set(L"l", L"spaced", L" a | b c | d ");

    const std::vector<std::wstring> ext = ini.getList(L"watch", L"extensions");
    if (CHECK_EQ(ext.size(), 3)) {
        CHECK_WEQ(ext[0], L".mp4");
        CHECK_WEQ(ext[1], L".mov");
        CHECK_WEQ(ext[2], L".m4v");
    }

    // Items are trimmed; inner spaces survive.
    const std::vector<std::wstring> spaced = ini.getList(L"l", L"spaced");
    if (CHECK_EQ(spaced.size(), 3)) {
        CHECK_WEQ(spaced[0], L"a");
        CHECK_WEQ(spaced[1], L"b c");
        CHECK_WEQ(spaced[2], L"d");
    }

    // Missing -> empty; a single value is a one-item list.
    CHECK(ini.getList(L"watch", L"missing").empty());
    CHECK_EQ(ini.getList(L"app", L"theme").size(), 1);

    // setList / getList round trip.
    ini.setList(L"l", L"folders", {L"C:\\a", L"D:\\b c", L"E:\\d"});
    const std::vector<std::wstring> folders = ini.getList(L"l", L"folders");
    if (CHECK_EQ(folders.size(), 3)) {
        CHECK_WEQ(folders[0], L"C:\\a");
        CHECK_WEQ(folders[1], L"D:\\b c");
        CHECK_WEQ(folders[2], L"E:\\d");
    }
    ini.setList(L"l", L"empty", {});
    CHECK(ini.getList(L"l", L"empty").empty());
}

// ---- mutation ----------------------------------------------------------------------------

HH_TEST(IniFile_setUpdatesInPlaceAndAppends) {
    IniFile ini;
    ini.parse(kSample);

    // Existing key: value changes, position does not.
    ini.set(L"app", L"theme", L"light");
    CHECK_WEQ(ini.get(L"app", L"theme"), L"light");
    std::vector<std::wstring> keys = ini.keys(L"app");
    if (CHECK_EQ(keys.size(), 3)) { CHECK_WEQ(keys[0], L"theme"); }

    // Case-insensitive match must not create a duplicate.
    ini.set(L"APP", L"THEME", L"system");
    CHECK_WEQ(ini.get(L"app", L"theme"), L"system");
    CHECK_EQ(ini.keys(L"app").size(), 3);

    // New key: appended to its section.
    ini.set(L"app", L"dock_mode", L"docked");
    keys = ini.keys(L"app");
    if (CHECK_EQ(keys.size(), 4)) { CHECK_WEQ(keys[3], L"dock_mode"); }

    // New section: appended after the existing ones.
    ini.set(L"ipc", L"pipe_name", L"HdrHint");
    const std::vector<std::wstring> sections = ini.sections();
    if (CHECK_EQ(sections.size(), 3)) { CHECK_WEQ(sections[2], L"ipc"); }
    CHECK_WEQ(ini.get(L"ipc", L"pipe_name"), L"HdrHint");

    // The comment between theme and accent survived all of that.
    const std::wstring out = normalizedText(ini.serialize());
    const size_t commentAt = out.find(L"; the accent colour");
    const size_t accentAt = out.find(L"accent =");
    CHECK(commentAt != std::wstring::npos);
    CHECK(accentAt != std::wstring::npos);
    CHECK(commentAt < accentAt);
}

HH_TEST(IniFile_setBoolSetIntRoundTrip) {
    IniFile ini;
    ini.setBool(L"s", L"flag_true", true);
    ini.setBool(L"s", L"flag_false", false);
    ini.setInt(L"s", L"count", 1234567890123LL);
    ini.setInt(L"s", L"negative", -5);

    CHECK(ini.getBool(L"s", L"flag_true", false));
    CHECK_FALSE(ini.getBool(L"s", L"flag_false", true));
    CHECK_EQ(ini.getInt(L"s", L"count", 0), 1234567890123LL);
    CHECK_EQ(ini.getInt(L"s", L"negative", 0), -5);

    // Survives a serialize/parse cycle.
    IniFile again;
    again.parse(ini.serialize());
    CHECK(again.getBool(L"s", L"flag_true", false));
    CHECK_FALSE(again.getBool(L"s", L"flag_false", true));
    CHECK_EQ(again.getInt(L"s", L"count", 0), 1234567890123LL);
}

HH_TEST(IniFile_valuesNeedingQuotesSurviveRoundTrip) {
    IniFile ini;
    ini.set(L"q", L"padded", L"  spaced out  ");
    ini.set(L"q", L"plain", L"C:\\path with spaces\\file.cube");
    ini.set(L"q", L"empty", L"");

    IniFile again;
    again.parse(ini.serialize());
    CHECK_WEQ(again.get(L"q", L"padded"), L"  spaced out  ");
    CHECK_WEQ(again.get(L"q", L"plain"), L"C:\\path with spaces\\file.cube");
    CHECK(again.has(L"q", L"empty"));
    CHECK(again.get(L"q", L"empty", L"fb").empty());
}

HH_TEST(IniFile_setCommentAppearsBeforeKey) {
    IniFile ini;
    ini.parse(kSample);
    ini.setComment(L"watch", L"debounce_ms", L"milliseconds to wait after the last change");

    const std::wstring out = normalizedText(ini.serialize());
    const size_t commentAt = out.find(L"milliseconds to wait after the last change");
    const size_t keyAt = out.find(L"debounce_ms =");
    const size_t sectionAt = out.find(L"[watch]");
    CHECK(commentAt != std::wstring::npos);
    CHECK(keyAt != std::wstring::npos);
    CHECK(sectionAt != std::wstring::npos);
    // Inside the [watch] section and directly ahead of the key.
    CHECK(sectionAt < commentAt);
    CHECK(commentAt < keyAt);

    // The value is untouched and the comment is not parsed as a key.
    IniFile again;
    again.parse(out);
    CHECK_EQ(again.getInt(L"watch", L"debounce_ms", 0), 500);
    CHECK_EQ(again.keys(L"watch").size(), 3);
}

HH_TEST(IniFile_clear) {
    IniFile ini;
    ini.parse(kSample);
    CHECK_FALSE(ini.sections().empty());
    ini.clear();
    CHECK(ini.sections().empty());
    CHECK_FALSE(ini.has(L"app", L"theme"));
    CHECK(normalizedText(ini.serialize()).empty());
}

// ---- odd input --------------------------------------------------------------------------

HH_TEST(IniFile_crlfTabsAndBomTolerated) {
    IniFile ini;
    // BOM as a wide char, CRLF endings, tabs around '='.
    ini.parse(L"\uFEFF[a]\r\nk\t=\tv\r\n\r\n[b]\r\nx = 1\r\n");
    CHECK_WEQ(ini.get(L"a", L"k"), L"v");
    CHECK_EQ(ini.getInt(L"b", L"x", 0), 1);
    const std::vector<std::wstring> sections = ini.sections();
    if (CHECK_EQ(sections.size(), 2)) { CHECK_WEQ(sections[0], L"a"); }
}

HH_TEST(IniFile_garbageLinesDoNotCrash) {
    IniFile ini;
    ini.parse(L"no section key = 1\n"
              L"[unterminated\n"
              L"=\n"
              L"= value without key\n"
              L"[ok]\n"
              L"key = value\n"
              L"just text\n");
    CHECK_WEQ(ini.get(L"ok", L"key"), L"value");
    // Serialising whatever was kept must not throw or crash either.
    const std::wstring out = ini.serialize();
    CHECK(out.find(L"[ok]") != std::wstring::npos);
}

// ---- disk ---------------------------------------------------------------------------------

HH_TEST(IniFile_loadSaveRoundTripOnDisk) {
    hh::test::ScratchDir dir(L"ini");
    if (!CHECK(dir.valid())) { return; }

    // Write the sample with a UTF-8 BOM: load must tolerate it.
    const std::string bom = "\xEF\xBB\xBF";
    CHECK(dir.writeFile(L"in.ini", bom + hh::platform::toUtf8(kSample)));

    IniFile ini;
    const auto loaded = ini.load(dir.file(L"in.ini"));
    if (!CHECK(loaded.ok())) { return; }
    CHECK_WEQ(ini.get(L"app", L"theme"), L"dark");
    CHECK_WEQ(ini.get(L"app", L"accent"), L"  blue  ");

    // Save elsewhere, read the bytes back: no BOM, content preserved.
    const auto saved = ini.save(dir.file(L"out.ini"));
    if (!CHECK(saved.ok())) { return; }
    const std::string bytes = dir.readFile(L"out.ini");
    CHECK(bytes.size() > 3);
    CHECK(bytes.rfind(bom, 0) != 0);
    CHECK_WEQ(normalizedText(hh::platform::toWide(bytes)), normalizedText(kSample));

    // And load that again.
    IniFile again;
    CHECK(again.load(dir.file(L"out.ini")).ok());
    CHECK_WEQ(again.get(L"watch", L"extensions"), L".mp4|.mov|.m4v");
    CHECK(again.getBool(L"app", L"first_run", false));
}

HH_TEST(IniFile_loadMissingFileIsEmptySuccess) {
    hh::test::ScratchDir dir(L"ini_missing");
    if (!CHECK(dir.valid())) { return; }

    IniFile ini;
    ini.set(L"pre", L"k", L"v");
    const auto r = ini.load(dir.file(L"does_not_exist.ini"));
    CHECK(r.ok());
    // The object is empty after loading a missing file.
    CHECK(ini.sections().empty());
    CHECK_FALSE(ini.has(L"pre", L"k"));
}

HH_TEST(IniFile_unicodeValuesRoundTripOnDisk) {
    hh::test::ScratchDir dir(L"ini_unicode");
    if (!CHECK(dir.valid())) { return; }

    IniFile ini;
    ini.set(L"u", L"name", L"S\u00E9rie \u2013 \u65E5\u672C\u8A9E.mp4");
    CHECK(ini.save(dir.file(L"u.ini")).ok());

    IniFile again;
    CHECK(again.load(dir.file(L"u.ini")).ok());
    CHECK_WEQ(again.get(L"u", L"name"), L"S\u00E9rie \u2013 \u65E5\u672C\u8A9E.mp4");
}
