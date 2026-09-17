// ---------------------------------------------------------------------------
// test_framework.h - the deliberately tiny harness behind hdrhint_tests.
//
//   HH_TEST(PathUtil_stem) {
//       CHECK_WEQ(hh::path::stem(L"clip.mp4"), L"clip");
//   }
//
// Tests register themselves during static initialisation; test_main.cpp owns
// the registry, the scratch-folder helpers and the entry point. A failed check
// never aborts the test: it is recorded with file/line/expression and the test
// keeps running, so one pass of the runner reports everything that is wrong.
//
// Every CHECK* macro evaluates to a bool so a test can guard follow-up access:
//
//   if (CHECK_EQ(items.size(), 1)) { CHECK_WEQ(items[0].outputPath, L"..."); }
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hh::test {

/// Signature of a test body.
using TestFn = void (*)();

/**
 * @brief One registered test.
 */
struct TestCase {
    const char* name = "";   ///< identifier given to HH_TEST
    const char* file = "";   ///< source file that defined it
    int line = 0;            ///< line of the HH_TEST macro
    TestFn fn = nullptr;     ///< the body
};

/// The global test list. A function-local static so registration during
/// static initialisation of other translation units is always safe.
std::vector<TestCase>& registry();

/**
 * @brief Appends a test to the registry; one instance per HH_TEST.
 */
struct Registrar {
    Registrar(const char* name, const char* file, int line, TestFn fn);
};

/// Records a failed check for the test that is currently running and prints it.
void reportFailure(const char* file, int line, const char* expression, const std::string& detail);

/// Number of failed checks in the test currently running.
int currentFailureCount();

// ---- scratch folders ----------------------------------------------------------

/// "%TEMP%\hdrhint_tests" (created on first use, never removed).
std::wstring testRootFolder();

/**
 * @brief A unique, empty directory under testRootFolder() that is deleted
 *        (recursively) when the guard goes out of scope.
 */
class ScratchDir {
public:
    explicit ScratchDir(std::wstring_view tag);
    ~ScratchDir();
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    /// Absolute path of the folder (no trailing separator).
    [[nodiscard]] const std::wstring& path() const noexcept { return path_; }
    /// True when the folder could be created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// "<path>\<name>".
    [[nodiscard]] std::wstring file(std::wstring_view name) const;
    /// Writes raw bytes to "<path>\<name>" (overwrites). False on failure.
    [[nodiscard]] bool writeFile(std::wstring_view name, std::string_view bytes) const;
    /// Reads all bytes of "<path>\<name>"; empty when unreadable.
    [[nodiscard]] std::string readFile(std::wstring_view name) const;
    /// True when "<path>\<name>" exists.
    [[nodiscard]] bool exists(std::wstring_view name) const;

private:
    std::wstring path_;
    bool valid_ = false;
};

// ---- value formatting ---------------------------------------------------------

/// UTF-16 -> UTF-8 for printing wide values (lossy on unpaired surrogates).
std::string utf8FromWide(std::wstring_view wide);

namespace detail {

/// Integer types that std::cmp_equal accepts (no bool, no character types).
template <class T>
inline constexpr bool isPlainInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
    !std::is_same_v<T, char32_t>;

} // namespace detail

/**
 * @brief Renders any value CHECK_EQ might see as readable text.
 */
template <class T>
std::string describe(const T& value) {
    using U = std::remove_cvref_t<T>;
    // Booleans first: they are integral and must not print as 0/1.
    if constexpr (std::is_same_v<U, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) {
        return "\"" + std::string(value) + "\"";
    } else if constexpr (std::is_same_v<U, std::wstring> || std::is_same_v<U, std::wstring_view>) {
        return "L\"" + utf8FromWide(value) + "\"";
    } else if constexpr (std::is_convertible_v<U, const char*>) {
        const char* p = value;
        return p ? ("\"" + std::string(p) + "\"") : std::string("(null)");
    } else if constexpr (std::is_convertible_v<U, const wchar_t*>) {
        const wchar_t* p = value;
        return p ? ("L\"" + utf8FromWide(p) + "\"") : std::string("(null)");
    } else if constexpr (std::is_enum_v<U>) {
        return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<U>>(value)));
    } else if constexpr (std::is_floating_point_v<U>) {
        return std::format("{}", value);
    } else if constexpr (std::is_arithmetic_v<U>) {
        return std::to_string(value);
    } else if constexpr (std::is_pointer_v<U>) {
        return std::format("{}", static_cast<const void*>(value));
    } else {
        return "<value>";
    }
}

/**
 * @brief Equality that never trips signed/unsigned warnings for integers.
 */
template <class A, class B>
bool equalValues(const A& a, const B& b) {
    using UA = std::remove_cvref_t<A>;
    using UB = std::remove_cvref_t<B>;
    if constexpr (detail::isPlainInteger<UA> && detail::isPlainInteger<UB>) {
        return std::cmp_equal(a, b);
    } else {
        return a == b;
    }
}

/// CHECK backend.
inline bool checkTrue(const char* file, int line, const char* expression, bool ok) {
    if (!ok) { reportFailure(file, line, expression, ""); }
    return ok;
}

/// CHECK_EQ backend: prints both sides when they differ.
template <class A, class B>
bool checkEqual(const char* file, int line, const char* expression, const A& a, const B& b) {
    if (equalValues(a, b)) { return true; }
    reportFailure(file, line, expression, "    left:  " + describe(a) + "\n    right: " + describe(b));
    return false;
}

/// CHECK_WEQ backend: wide strings compared verbatim, printed as UTF-8.
inline bool checkWideEqual(const char* file, int line, const char* expression, std::wstring_view a, std::wstring_view b) {
    if (a == b) { return true; }
    reportFailure(file, line, expression,
                  "    left:  L\"" + utf8FromWide(a) + "\"\n    right: L\"" + utf8FromWide(b) + "\"");
    return false;
}

/// CHECK_NEAR backend for floating point values.
inline bool checkNear(const char* file, int line, const char* expression, double a, double b, double eps) {
    if (std::fabs(a - b) <= eps) { return true; }
    reportFailure(file, line, expression,
                  std::format("    left:  {}\n    right: {}\n    eps:   {}", a, b, eps));
    return false;
}

} // namespace hh::test

// ---- macros ---------------------------------------------------------------------

/// Defines and registers a test: HH_TEST(Name) { ... }
#define HH_TEST(name)                                                                       \
    static void hh_test_body_##name();                                                      \
    static const ::hh::test::Registrar hh_test_registrar_##name(#name, __FILE__, __LINE__,  \
                                                                &hh_test_body_##name);      \
    static void hh_test_body_##name()

/// Records a failure when @p expr is false. Evaluates to the bool.
#define CHECK(expr) ::hh::test::checkTrue(__FILE__, __LINE__, #expr, static_cast<bool>(expr))
/// Records a failure when @p expr is true.
#define CHECK_FALSE(expr) ::hh::test::checkTrue(__FILE__, __LINE__, "!(" #expr ")", !static_cast<bool>(expr))
/// Records a failure (printing both values) when a != b.
#define CHECK_EQ(a, b) ::hh::test::checkEqual(__FILE__, __LINE__, #a " == " #b, (a), (b))
/// Wide-string equality (accepts std::wstring, std::wstring_view and literals).
#define CHECK_WEQ(a, b) ::hh::test::checkWideEqual(__FILE__, __LINE__, #a " == " #b, (a), (b))
/// Floating point equality within @p eps.
#define CHECK_NEAR(a, b, eps) \
    ::hh::test::checkNear(__FILE__, __LINE__, #a " ~= " #b, static_cast<double>(a), static_cast<double>(b), static_cast<double>(eps))
