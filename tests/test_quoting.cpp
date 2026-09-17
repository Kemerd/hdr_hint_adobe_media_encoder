// ---------------------------------------------------------------------------
// test_quoting.cpp - command-line quoting for CreateProcessW: quoteArgument
// and buildCommandLine, checked against the expected text and against the
// one parser that matters, CommandLineToArgvW.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "platform/Process.h"
#include "platform/Win.h"

#include <shellapi.h>

#include <cstddef>
#include <string>
#include <vector>

namespace platform = hh::platform;

namespace {

/**
 * @brief Splits a command line exactly the way a child process would see
 *        its argv (CommandLineToArgvW implements the MSVC CRT rules).
 */
std::vector<std::wstring> splitLikeChild(const std::wstring& commandLine) {
    std::vector<std::wstring> out;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(commandLine.c_str(), &argc);
    if (!argv) { return out; }
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i] ? argv[i] : L"");
    }
    ::LocalFree(argv);
    return out;
}

/**
 * @brief Builds a line from exe + args, splits it again and checks that
 *        every piece came back verbatim.
 */
void checkRoundTrip(const std::wstring& exe, const std::vector<std::wstring>& args) {
    const std::wstring line = platform::buildCommandLine(exe, args);
    const std::vector<std::wstring> parsed = splitLikeChild(line);
    if (!CHECK_EQ(parsed.size(), args.size() + 1)) { return; }
    CHECK_WEQ(parsed[0], exe);
    for (size_t i = 0; i < args.size(); ++i) {
        CHECK_WEQ(parsed[i + 1], args[i]);
    }
}

} // namespace

// ---- quoteArgument ---------------------------------------------------------------

/**
 * @brief Arguments without whitespace or quotes are passed through untouched,
 *        trailing backslashes included.
 */
HH_TEST(Quoting_plainArgumentsAreUnchanged) {
    CHECK_WEQ(platform::quoteArgument(L"a"), L"a");
    CHECK_WEQ(platform::quoteArgument(L"-J"), L"-J");
    CHECK_WEQ(platform::quoteArgument(L"--color-range"), L"--color-range");
    CHECK_WEQ(platform::quoteArgument(L"0:0.708,0.292,0.170,0.797,0.131,0.046"), L"0:0.708,0.292,0.170,0.797,0.131,0.046");
    CHECK_WEQ(platform::quoteArgument(L"C:\\dir\\"), L"C:\\dir\\");
    CHECK_WEQ(platform::quoteArgument(L"C:\\a\\b.mp4"), L"C:\\a\\b.mp4");
    CHECK_WEQ(platform::quoteArgument(L"\\\\server\\share\\clip.mp4"), L"\\\\server\\share\\clip.mp4");
}

/**
 * @brief Whitespace forces quotes; the empty argument becomes a quoted
 *        empty string so it still counts as an argument.
 */
HH_TEST(Quoting_whitespaceGetsQuoted) {
    CHECK_WEQ(platform::quoteArgument(L"a b"), L"\"a b\"");
    CHECK_WEQ(platform::quoteArgument(L""), L"\"\"");
    CHECK_WEQ(platform::quoteArgument(L"tab\there"), L"\"tab\there\"");
    CHECK_WEQ(platform::quoteArgument(L" "), L"\" \"");
    CHECK_WEQ(platform::quoteArgument(L"C:\\a b.mp4"), L"\"C:\\a b.mp4\"");
}

/**
 * @brief A backslash run right before the closing quote is doubled so the
 *        parser does not treat it as escaping that quote.
 */
HH_TEST(Quoting_trailingBackslashesBeforeClosingQuoteAreDoubled) {
    CHECK_WEQ(platform::quoteArgument(L"C:\\my dir\\"), L"\"C:\\my dir\\\\\"");
    CHECK_WEQ(platform::quoteArgument(L"C:\\my dir\\\\"), L"\"C:\\my dir\\\\\\\\\"");
    // Backslashes in the middle of a quoted argument stay single.
    CHECK_WEQ(platform::quoteArgument(L"C:\\my dir\\clip.mp4"), L"\"C:\\my dir\\clip.mp4\"");
}

/**
 * @brief Embedded quotes are escaped, and a backslash run in front of one
 *        is doubled before the escaping backslash is added.
 */
HH_TEST(Quoting_embeddedQuotesAreEscaped) {
    CHECK_WEQ(platform::quoteArgument(L"a\"b"), L"\"a\\\"b\"");
    // Two backslashes then a quote: four backslashes, one escape, the quote.
    CHECK_WEQ(platform::quoteArgument(L"x\\\\\"y"), L"\"x\\\\\\\\\\\"y\"");
    // One backslash then a quote: two backslashes, one escape, the quote.
    CHECK_WEQ(platform::quoteArgument(L"x\\\"y"), L"\"x\\\\\\\"y\"");
    // A quote alone.
    CHECK_WEQ(platform::quoteArgument(L"\""), L"\"\\\"\"");
}

// ---- buildCommandLine ------------------------------------------------------------

/**
 * @brief The executable and every argument are quoted individually and
 *        joined with single spaces.
 */
HH_TEST(Quoting_buildCommandLineJoinsQuotedPieces) {
    const std::wstring line = platform::buildCommandLine(
        L"C:\\Program Files\\MKVToolNix\\mkvmerge.exe", {L"-J", L"C:\\a b.mp4"});
    CHECK_WEQ(line, L"\"C:\\Program Files\\MKVToolNix\\mkvmerge.exe\" -J \"C:\\a b.mp4\"");

    // No arguments: just the (quoted when needed) executable.
    CHECK_WEQ(platform::buildCommandLine(L"C:\\Tools\\mkvmerge.exe", {}), L"C:\\Tools\\mkvmerge.exe");
    CHECK_WEQ(platform::buildCommandLine(L"C:\\My Tools\\mkvmerge.exe", {}), L"\"C:\\My Tools\\mkvmerge.exe\"");

    // An empty argument is preserved as "" rather than dropped.
    CHECK_WEQ(platform::buildCommandLine(L"x.exe", {L"", L"y"}), L"x.exe \"\" y");
}

// ---- round trip through the real parser -------------------------------------------

/**
 * @brief Everything quoteArgument produces must parse back to the original
 *        argv with CommandLineToArgvW, for the plain and the nasty cases.
 */
HH_TEST(Quoting_roundTripsThroughCommandLineToArgvW) {
    // The documented mkvmerge example.
    checkRoundTrip(L"C:\\Program Files\\MKVToolNix\\mkvmerge.exe", {L"-J", L"C:\\a b.mp4"});

    // A typical mux command line with the colour flags.
    checkRoundTrip(L"C:\\Program Files\\MKVToolNix\\mkvmerge.exe",
                   {L"--output", L"B:\\YouTube Renders\\clip_REC709_HINT.mkv",
                    L"--color-matrix-coefficients", L"0:9",
                    L"--chromaticity-coordinates", L"0:0.708,0.292,0.170,0.797,0.131,0.046",
                    L"--attachment-name", L"Rec2100_PQ_to_Rec709.cube",
                    L"B:\\YouTube Renders\\clip.mp4"});

    // The tricky ones: empty, spaces, quotes, backslash runs, tabs, UNC, Unicode.
    checkRoundTrip(L"C:\\Tools\\mkvmerge.exe",
                   {L"",
                    L"a b",
                    L"a\"b",
                    L"C:\\my dir\\",
                    L"C:\\my dir\\\\",
                    L"x\\\\\"y",
                    L"x\\\"y",
                    L"\"",
                    L"trailing\\",
                    L"a\\\\b",
                    L"tab\there",
                    L"--title=He said \"hi\" and left",
                    L"C:\\my \"quoted\" dir\\",
                    L"\\\\server\\share\\clip name.mp4",
                    L"clip \u00e9\u00e8 \u65e5\u672c.mp4"});

    // Each nasty argument on its own as well, so a failure names the culprit.
    for (const wchar_t* arg : {L"", L"a b", L"a\"b", L"C:\\my dir\\", L"x\\\\\"y", L"x\\\"y", L"\"",
                               L"trailing\\", L"a\\\\b", L"\\\\\"", L"end with quote\"", L"\"start with quote"}) {
        checkRoundTrip(L"C:\\Tools\\mkvmerge.exe", {arg});
    }
}
