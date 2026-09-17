// ---------------------------------------------------------------------------
// IniFile.h - tiny ordered INI reader/writer (UTF-8, comments preserved).
//
//   [section]
//   ; comment
//   key = value
//
// Keys are case-insensitive. Values are trimmed; a value wrapped in double
// quotes keeps its inner whitespace verbatim. Lists use '|' as separator.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#include <string>
#include <vector>

namespace hh {

class IniFile {
public:
    /// Loads from disk (BOM tolerated). A missing file is not an error: the object is empty.
    Result<void> load(const std::wstring& path);
    /// Writes atomically (no BOM), preserving section/key order and comments.
    Result<void> save(const std::wstring& path) const;
    /// Parses from text (for tests).
    void parse(std::wstring_view text);
    /// Serialises to text.
    [[nodiscard]] std::wstring serialize() const;

    [[nodiscard]] bool has(std::wstring_view section, std::wstring_view key) const;
    [[nodiscard]] std::wstring get(std::wstring_view section, std::wstring_view key, std::wstring_view fallback = L"") const;
    [[nodiscard]] bool getBool(std::wstring_view section, std::wstring_view key, bool fallback) const;
    [[nodiscard]] long long getInt(std::wstring_view section, std::wstring_view key, long long fallback) const;
    [[nodiscard]] double getDouble(std::wstring_view section, std::wstring_view key, double fallback) const;
    [[nodiscard]] std::vector<std::wstring> getList(std::wstring_view section, std::wstring_view key) const;

    void set(std::wstring_view section, std::wstring_view key, std::wstring_view value);
    void setBool(std::wstring_view section, std::wstring_view key, bool value);
    void setInt(std::wstring_view section, std::wstring_view key, long long value);
    void setList(std::wstring_view section, std::wstring_view key, const std::vector<std::wstring>& values);

    /// Adds a comment line before a key (or at the section end when the key is absent).
    void setComment(std::wstring_view section, std::wstring_view key, std::wstring_view comment);

    [[nodiscard]] std::vector<std::wstring> sections() const;
    [[nodiscard]] std::vector<std::wstring> keys(std::wstring_view section) const;
    void clear();

private:
    struct Line {
        enum class Kind { Blank, Comment, Section, KeyValue };
        Kind kind = Kind::Blank;
        std::wstring text;    ///< raw text for Blank/Comment/Section
        std::wstring key;     ///< for KeyValue
        std::wstring value;   ///< for KeyValue (unquoted)
    };
    struct Section {
        std::wstring name;
        std::vector<Line> lines;
    };

    Section* findSection(std::wstring_view name);
    const Section* findSection(std::wstring_view name) const;
    Section& ensureSection(std::wstring_view name);

    std::vector<Line> preamble_;      ///< comments/blank lines before the first section
    std::vector<Section> sections_;
};

} // namespace hh
