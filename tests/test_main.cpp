// ---------------------------------------------------------------------------
// test_main.cpp - entry point and registry for hdrhint_tests.
//
// Usage:
//   hdrhint_tests                 run everything
//   hdrhint_tests <substring>     run the tests whose name contains <substring>
//   hdrhint_tests --list          print the test names and exit
//
// Exit code is 0 when every test passed, 1 otherwise. Output is UTF-8 so the
// wide values printed by CHECK_EQ survive a Windows console.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"
#include "platform/Win.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace hh::test {

namespace {

/**
 * @brief Mutable state of the run: which test is executing and its tally.
 */
struct RunState {
    const TestCase* current = nullptr;   ///< test currently executing (nullptr between tests)
    int failuresInCurrent = 0;           ///< failed checks in the current test
    int checksFailedTotal = 0;           ///< failed checks over the whole run
};

/// Single run state (function-local static for the same reason as registry()).
RunState& runState() {
    static RunState state;
    return state;
}

/// Prints a UTF-8 line to stdout.
void printLine(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

/**
 * @brief Removes a directory tree (best effort, depth-limited).
 *
 * Files are deleted first, then subfolders recursively, then the folder
 * itself. A depth cap keeps a reparse-point loop from recursing forever.
 */
void removeTree(const std::wstring& dir, int depth) {
    // Never wander deeper than a handful of levels: scratch folders are flat.
    if (dir.empty() || depth > 8) { return; }

    // Delete every entry; directories recurse.
    auto listing = platform::listDirectory(dir);
    if (listing.ok()) {
        for (const platform::DirEntry& entry : listing.value()) {
            if (entry.name.empty()) { continue; }
            const std::wstring full = path::join(dir, entry.name);
            if (entry.isDirectory) {
                removeTree(full, depth + 1);
            } else {
                (void)platform::deleteFile(full);
            }
        }
    }

    // Finally the folder itself. Failure is not fatal: it is a temp folder.
#if defined(_WIN32)
    const std::wstring extended = platform::toExtendedPath(dir);
    if (!::RemoveDirectoryW(extended.c_str())) {
        // Fall back to the plain path in case the prefix confused a shim.
        ::RemoveDirectoryW(dir.c_str());
    }
#else
    ::rmdir(platform::toUtf8(dir).c_str());
#endif
}

/// This process's id, for unique scratch-folder names.
unsigned long currentProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

/// Monotonic counter so two ScratchDirs created in the same tick differ.
unsigned nextScratchSerial() {
    static unsigned serial = 0;
    return ++serial;
}

} // namespace

// ---- registry ---------------------------------------------------------------------

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char* name, const char* file, int line, TestFn fn) {
    // Defensive: a null body would crash the runner much later; skip it now.
    if (!name || !fn) { return; }
    registry().push_back(TestCase{name, file ? file : "", line, fn});
}

// ---- failure reporting ------------------------------------------------------------

void reportFailure(const char* file, int line, const char* expression, const std::string& detail) {
    RunState& state = runState();
    ++state.failuresInCurrent;
    ++state.checksFailedTotal;

    // "  FAIL tests\test_x.cpp(42): expr" followed by the optional value dump.
    std::string text = "  FAIL ";
    text += file ? file : "?";
    text += "(" + std::to_string(line) + "): ";
    text += expression ? expression : "";
    printLine(text);
    if (!detail.empty()) { printLine(detail); }
}

int currentFailureCount() {
    return runState().failuresInCurrent;
}

// ---- scratch folders ----------------------------------------------------------------

std::wstring testRootFolder() {
    static std::wstring root;
    if (!root.empty()) { return root; }

    // Prefer the platform helper; fall back to GetTempPathW when it is empty.
    std::wstring temp = platform::tempFolder();
#if defined(_WIN32)
    if (temp.empty()) {
        wchar_t buffer[MAX_PATH + 1] = {};
        const DWORD n = ::GetTempPathW(MAX_PATH, buffer);
        if (n > 0 && n <= MAX_PATH) { temp.assign(buffer, n); }
    }
#endif
    // Strip a trailing separator so the join below never doubles it.
    while (!temp.empty() && (temp.back() == L'\\' || temp.back() == L'/')) { temp.pop_back(); }
    if (temp.empty()) { temp = L"."; }

    root = path::join(temp, L"hdrhint_tests");
    (void)platform::createDirectories(root);
    return root;
}

ScratchDir::ScratchDir(std::wstring_view tag) {
    // Build "<root>\<tag>_<pid>_<serial>" so parallel runs never collide.
    std::wstring safeTag(tag);
    for (wchar_t& c : safeTag) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L' ') { c = L'_'; }
    }
    if (safeTag.empty()) { safeTag = L"scratch"; }
    path_ = path::join(testRootFolder(), safeTag + L"_" + std::to_wstring(currentProcessId()) + L"_" +
                                              std::to_wstring(nextScratchSerial()));

    // Start from a clean folder even if a crashed run left one behind.
    removeTree(path_, 0);
    valid_ = platform::createDirectories(path_).ok();
    if (!valid_) {
        printLine("  WARN could not create scratch folder " + platform::toUtf8(path_));
    }
}

ScratchDir::~ScratchDir() {
    removeTree(path_, 0);
}

std::wstring ScratchDir::file(std::wstring_view name) const {
    return path::join(path_, name);
}

bool ScratchDir::writeFile(std::wstring_view name, std::string_view bytes) const {
    if (!valid_ || name.empty()) { return false; }
    return platform::writeAllAtomic(file(name), bytes).ok();
}

std::string ScratchDir::readFile(std::wstring_view name) const {
    if (!valid_ || name.empty()) { return {}; }
    auto r = platform::readAll(file(name));
    if (!r.ok()) { return {}; }
    return std::string(r.value().begin(), r.value().end());
}

bool ScratchDir::exists(std::wstring_view name) const {
    if (!valid_ || name.empty()) { return false; }
    return platform::exists(file(name));
}

// ---- formatting -----------------------------------------------------------------------

std::string utf8FromWide(std::wstring_view wide) {
    return platform::toUtf8(wide);
}

} // namespace hh::test

// ---- entry point ------------------------------------------------------------------------

/**
 * @brief Runs every registered test (optionally filtered) and prints a summary.
 */
int main(int argc, char** argv) {
    using namespace hh::test;

    // UTF-8 console output so wide strings in failure dumps are readable.
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    // Command line: "--list" or a single substring filter.
    bool listOnly = false;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) { continue; }
        const std::string_view arg(argv[i]);
        if (arg == "--list") { listOnly = true; }
        else if (!arg.empty() && arg[0] != '-') { filter = std::string(arg); }
    }

    // Stable order regardless of link order: sort by name.
    std::vector<TestCase> tests = registry();
    std::sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
        return std::string_view(a.name) < std::string_view(b.name);
    });

    if (listOnly) {
        for (const TestCase& t : tests) { printLine(t.name); }
        return 0;
    }

    // Log to the scratch root so a failing test can be diagnosed afterwards.
    const std::wstring logDir = testRootFolder();
    (void)hh::Logger::instance().open(logDir, hh::LogLevel::Debug, 4096, 2);

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const TestCase& t : tests) {
        // Apply the substring filter.
        if (!filter.empty() && std::string_view(t.name).find(filter) == std::string_view::npos) {
            ++skipped;
            continue;
        }

        RunState& state = runState();
        state.current = &t;
        state.failuresInCurrent = 0;
        printLine(std::string("[ RUN  ] ") + t.name);

        // A test must never take the runner down with it.
        try {
            if (t.fn) { t.fn(); }
        } catch (const std::exception& ex) {
            reportFailure(t.file, t.line, "unhandled std::exception", std::string("    what: ") + ex.what());
        } catch (...) {
            reportFailure(t.file, t.line, "unhandled non-standard exception", "");
        }

        if (state.failuresInCurrent == 0) {
            ++passed;
            printLine(std::string("[   OK ] ") + t.name);
        } else {
            ++failed;
            printLine(std::string("[ FAIL ] ") + t.name + " (" + std::to_string(state.failuresInCurrent) + " failed checks)");
        }
        state.current = nullptr;
    }

    // Summary line in the documented "N passed, M failed" form.
    std::string summary = std::to_string(passed) + " passed, " + std::to_string(failed) + " failed";
    if (skipped > 0) { summary += ", " + std::to_string(skipped) + " skipped by filter"; }
    printLine(summary);
    if (failed > 0) {
        printLine("log: " + utf8FromWide(hh::Logger::instance().filePath()));
    }

    hh::Logger::instance().close();
    return failed > 0 ? 1 : 0;
}
