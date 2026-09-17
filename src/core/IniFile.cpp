// ---------------------------------------------------------------------------
// IniFile.cpp - tiny ordered INI reader/writer (UTF-8, comments preserved).
//
// The document is kept as a list of lines grouped by section so a load/save
// round trip changes nothing but the values that were set. Section names and
// keys compare case-insensitively; the original spelling is what gets written
// back.
// ---------------------------------------------------------------------------
#include "core/IniFile.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"

#include <algorithm>
#include <string>
#include <vector>

namespace hh {

namespace {

constexpr const wchar_t* kLog = L"IniFile";

// Files larger than this are refused: an INI is never that big, a corrupt one might be.
constexpr uint64_t kMaxIniBytes = 16ull * 1024 * 1024;

/// ASCII whitespace test for the quoting decision.
bool isSpace(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f';
}

/**
 * @brief Values that would not survive a trim on reload get double quotes.
 */
std::wstring quoteIfNeeded(const std::wstring& value) {
    if (value.empty()) {
        return value;
    }
    const bool edgeSpace = isSpace(value.front()) || isSpace(value.back());
    const bool looksQuoted = value.size() >= 2 && value.front() == L'"' && value.back() == L'"';
    if (edgeSpace || looksQuoted) {
        return L"\"" + value + L"\"";
    }
    return value;
}

/**
 * @brief Line breaks inside a value or key would corrupt the file; flatten them.
 */
std::wstring singleLine(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) {
        if (c == L'\r' || c == L'\n') {
            c = L' ';
        }
    }
    return out;
}

/**
 * @brief Drops carriage returns so a comment can be split on '\n' alone.
 */
std::wstring stripCr(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c != L'\r') {
            out += c;
        }
    }
    return out;
}

/**
 * @brief Bytes -> text. UTF-16 LE with BOM, UTF-8 (BOM optional), and as a
 *        last resort the ANSI code page (what the old Python tool wrote).
 */
std::wstring decodeText(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }

    // UTF-16 LE BOM: decode the code units directly.
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        const size_t count = (bytes.size() - 2) / 2;
        std::wstring w(count, L'\0');
        for (size_t i = 0; i < count; ++i) {
            const size_t at = 2 + i * 2;
            w[i] = static_cast<wchar_t>(static_cast<unsigned>(bytes[at]) | (static_cast<unsigned>(bytes[at + 1]) << 8));
        }
        return w;
    }

    // UTF-8, with or without BOM.
    size_t start = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        start = 3;
    }
    const std::string_view utf8(reinterpret_cast<const char*>(bytes.data()) + start, bytes.size() - start);
    std::wstring w = platform::toWide(utf8);

    // Replacement characters that were not in the source mean the bytes were
    // not UTF-8 at all; retry with the ANSI page before giving up.
    if (w.find(L'�') != std::wstring::npos && utf8.find("\xEF\xBF\xBD") == std::string_view::npos) {
        std::wstring ansi = platform::fromCodePage(utf8, CP_ACP);
        if (!ansi.empty()) {
            return ansi;
        }
    }
    return w;
}

} // namespace

// ---------------------------------------------------------------------------
// Load / save / parse / serialize
// ---------------------------------------------------------------------------

/**
 * @brief Loads from disk. A missing file leaves the object empty (success).
 */
Result<void> IniFile::load(const std::wstring& path) {
    clear();
    if (platform::trim(path).empty()) {
        return Error::text(L"IniFile::load: empty path");
    }
    if (!platform::exists(path)) {
        return {};
    }

    auto data = platform::readAll(path, kMaxIniBytes);
    if (!data) {
        return data.error();
    }
    parse(decodeText(data.value()));
    return {};
}

/**
 * @brief Writes UTF-8 without BOM, atomically. The parent folder is created on demand.
 */
Result<void> IniFile::save(const std::wstring& path) const {
    if (platform::trim(path).empty()) {
        return Error::text(L"IniFile::save: empty path");
    }
    const std::wstring dir = path::parent(path);
    if (!dir.empty()) {
        if (auto made = platform::createDirectories(dir); !made) {
            return made.error();
        }
    }
    const std::string utf8 = platform::toUtf8(serialize());
    return platform::writeAllAtomic(path, utf8);
}

/**
 * @brief Parses text into the line model. Anything unrecognised is kept as a
 *        comment line so nothing is lost on the next save.
 */
void IniFile::parse(std::wstring_view text) {
    clear();
    Section* current = nullptr;

    // A byte-order mark that survived decoding must not become part of the first line.
    if (!text.empty() && text.front() == L'﻿') {
        text.remove_prefix(1);
    }

    size_t pos = 0;
    while (pos < text.size()) {
        // Slice the next line (LF or CRLF).
        const size_t nl = text.find(L'\n', pos);
        std::wstring_view raw = (nl == std::wstring_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::wstring_view::npos) ? text.size() : nl + 1;
        if (!raw.empty() && raw.back() == L'\r') {
            raw.remove_suffix(1);
        }

        const std::wstring_view t = platform::trim(raw);
        Line line;

        if (t.empty()) {
            line.kind = Line::Kind::Blank;
            line.text = std::wstring(raw);
        } else if (t.front() == L';' || t.front() == L'#') {
            line.kind = Line::Kind::Comment;
            line.text = std::wstring(raw);
        } else if (t.size() >= 2 && t.front() == L'[' && t.back() == L']') {
            // New section; its header line is kept verbatim as the first line.
            Section section;
            section.name = std::wstring(platform::trim(t.substr(1, t.size() - 2)));
            Line header;
            header.kind = Line::Kind::Section;
            header.text = std::wstring(raw);
            section.lines.push_back(std::move(header));
            sections_.push_back(std::move(section));
            current = &sections_.back();
            continue;
        } else {
            const size_t eq = t.find(L'=');
            if (eq == std::wstring_view::npos) {
                // Not a key/value: preserve as-is.
                line.kind = Line::Kind::Comment;
                line.text = std::wstring(raw);
            } else {
                line.kind = Line::Kind::KeyValue;
                line.key = std::wstring(platform::trim(t.substr(0, eq)));
                std::wstring_view v = platform::trim(t.substr(eq + 1));
                // "quoted value" keeps its inner whitespace verbatim.
                if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') {
                    v = v.substr(1, v.size() - 2);
                }
                line.value = std::wstring(v);
            }
        }

        // Route the line: current section, the preamble, or (for orphan keys
        // before any header) an unnamed section.
        if (current != nullptr) {
            current->lines.push_back(std::move(line));
        } else if (line.kind == Line::Kind::KeyValue) {
            Section& orphan = ensureSection(L"");
            orphan.lines.push_back(std::move(line));
            current = &orphan;
        } else {
            preamble_.push_back(std::move(line));
        }
    }
}

/**
 * @brief Text form: preamble, then each section with one blank line between.
 */
std::wstring IniFile::serialize() const {
    std::wstring out;

    // Emits a single line in its canonical form.
    const auto emit = [&out](const Line& l) {
        switch (l.kind) {
        case Line::Kind::Blank:
            out += L"\r\n";
            break;
        case Line::Kind::Comment:
        case Line::Kind::Section:
            out += l.text;
            out += L"\r\n";
            break;
        case Line::Kind::KeyValue:
            out += l.key;
            out += L" = ";
            out += quoteIfNeeded(l.value);
            out += L"\r\n";
            break;
        }
    };

    // Preamble without its trailing blank lines (the separator is added below).
    size_t preEnd = preamble_.size();
    while (preEnd > 0 && preamble_[preEnd - 1].kind == Line::Kind::Blank) {
        --preEnd;
    }
    for (size_t i = 0; i < preEnd; ++i) {
        emit(preamble_[i]);
    }
    bool needBlank = preEnd > 0;

    for (const Section& s : sections_) {
        // Trailing blanks are dropped so exactly one separates sections.
        size_t end = s.lines.size();
        while (end > 0 && s.lines[end - 1].kind == Line::Kind::Blank) {
            --end;
        }
        if (end == 0) {
            continue;
        }
        if (needBlank) {
            out += L"\r\n";
        }
        for (size_t i = 0; i < end; ++i) {
            emit(s.lines[i]);
        }
        needBlank = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

bool IniFile::has(std::wstring_view section, std::wstring_view key) const {
    const Section* s = findSection(section);
    if (s == nullptr) {
        return false;
    }
    const std::wstring_view k = platform::trim(key);
    for (const Line& l : s->lines) {
        if (l.kind == Line::Kind::KeyValue && platform::iequals(l.key, k)) {
            return true;
        }
    }
    return false;
}

std::wstring IniFile::get(std::wstring_view section, std::wstring_view key, std::wstring_view fallback) const {
    const Section* s = findSection(section);
    if (s == nullptr) {
        return std::wstring(fallback);
    }
    const std::wstring_view k = platform::trim(key);
    for (const Line& l : s->lines) {
        if (l.kind == Line::Kind::KeyValue && platform::iequals(l.key, k)) {
            return l.value;
        }
    }
    return std::wstring(fallback);
}

bool IniFile::getBool(std::wstring_view section, std::wstring_view key, bool fallback) const {
    if (!has(section, key)) {
        return fallback;
    }
    const std::optional<bool> v = platform::parseBool(platform::trim(get(section, key)));
    return v.value_or(fallback);
}

long long IniFile::getInt(std::wstring_view section, std::wstring_view key, long long fallback) const {
    if (!has(section, key)) {
        return fallback;
    }
    const std::optional<long long> v = platform::parseInt(platform::trim(get(section, key)));
    return v.value_or(fallback);
}

double IniFile::getDouble(std::wstring_view section, std::wstring_view key, double fallback) const {
    if (!has(section, key)) {
        return fallback;
    }
    const std::optional<double> v = platform::parseDouble(platform::trim(get(section, key)));
    return v.value_or(fallback);
}

/**
 * @brief '|'-separated list, each item trimmed, empties dropped.
 */
std::vector<std::wstring> IniFile::getList(std::wstring_view section, std::wstring_view key) const {
    std::vector<std::wstring> out;
    if (!has(section, key)) {
        return out;
    }
    for (const std::wstring& piece : platform::split(get(section, key), L'|', true)) {
        const std::wstring_view t = platform::trim(piece);
        if (!t.empty()) {
            out.emplace_back(t);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

/**
 * @brief Updates the first matching key or appends a new one at the end of
 *        the section body (before its trailing blank lines).
 */
void IniFile::set(std::wstring_view section, std::wstring_view key, std::wstring_view value) {
    const std::wstring_view k = platform::trim(key);
    if (k.empty()) {
        HH_LOG_WARN(kLog, L"set: empty key in section [{}] ignored", section);
        return;
    }
    Section& s = ensureSection(section);
    const std::wstring flat = singleLine(value);

    // Existing key: replace the value in place.
    for (Line& l : s.lines) {
        if (l.kind == Line::Kind::KeyValue && platform::iequals(l.key, k)) {
            l.value = flat;
            return;
        }
    }

    // New key: insert after the last non-blank line (never before the header).
    size_t at = s.lines.size();
    while (at > 0 && s.lines[at - 1].kind == Line::Kind::Blank) {
        --at;
    }
    if (at == 0 && !s.lines.empty() && s.lines[0].kind == Line::Kind::Section) {
        at = 1;
    }
    Line l;
    l.kind = Line::Kind::KeyValue;
    l.key = singleLine(k);
    l.value = flat;
    s.lines.insert(s.lines.begin() + static_cast<std::ptrdiff_t>(at), std::move(l));
}

void IniFile::setBool(std::wstring_view section, std::wstring_view key, bool value) {
    set(section, key, value ? L"true" : L"false");
}

void IniFile::setInt(std::wstring_view section, std::wstring_view key, long long value) {
    set(section, key, std::to_wstring(value));
}

/**
 * @brief Joins with '|'. Items are trimmed; an item containing '|' would not
 *        survive the round trip, so the character is replaced by a space.
 */
void IniFile::setList(std::wstring_view section, std::wstring_view key, const std::vector<std::wstring>& values) {
    std::vector<std::wstring> clean;
    clean.reserve(values.size());
    for (const std::wstring& v : values) {
        std::wstring item(platform::trim(v));
        if (item.empty()) {
            continue;
        }
        std::replace(item.begin(), item.end(), L'|', L' ');
        clean.push_back(std::move(item));
    }
    set(section, key, platform::join(clean, L"|"));
}

/**
 * @brief Inserts "; comment" lines before @p key (or at the section end when
 *        the key is absent). Re-applying the same comment is a no-op so
 *        repeated saves do not pile up duplicates.
 */
void IniFile::setComment(std::wstring_view section, std::wstring_view key, std::wstring_view comment) {
    // Build the comment lines; multi-line comments become one line each.
    std::vector<Line> lines;
    for (const std::wstring& piece : platform::split(stripCr(comment), L'\n', false)) {
        const std::wstring_view t = platform::trim(piece);
        Line l;
        l.kind = Line::Kind::Comment;
        if (t.empty()) {
            l.text = L";";
        } else if (t.front() == L';' || t.front() == L'#') {
            l.text = std::wstring(t);
        } else {
            l.text = L"; " + std::wstring(t);
        }
        lines.push_back(std::move(l));
    }
    if (lines.empty()) {
        return;
    }

    Section& s = ensureSection(section);
    const std::wstring_view k = platform::trim(key);

    // Where does the block go?
    size_t insertAt = std::wstring::npos;
    for (size_t i = 0; i < s.lines.size(); ++i) {
        if (s.lines[i].kind == Line::Kind::KeyValue && platform::iequals(s.lines[i].key, k)) {
            insertAt = i;
            break;
        }
    }
    if (insertAt == std::wstring::npos) {
        insertAt = s.lines.size();
        while (insertAt > 0 && s.lines[insertAt - 1].kind == Line::Kind::Blank) {
            --insertAt;
        }
        if (insertAt == 0 && !s.lines.empty() && s.lines[0].kind == Line::Kind::Section) {
            insertAt = 1;
        }
    }

    // Already there (immediately preceding)? Then leave the file alone.
    if (insertAt >= lines.size()) {
        bool same = true;
        for (size_t i = 0; i < lines.size(); ++i) {
            const Line& existing = s.lines[insertAt - lines.size() + i];
            if (existing.kind != Line::Kind::Comment || existing.text != lines[i].text) {
                same = false;
                break;
            }
        }
        if (same) {
            return;
        }
    }

    s.lines.insert(s.lines.begin() + static_cast<std::ptrdiff_t>(insertAt), lines.begin(), lines.end());
}

// ---------------------------------------------------------------------------
// Structure queries
// ---------------------------------------------------------------------------

/**
 * @brief Section names in file order (the unnamed orphan section is skipped).
 */
std::vector<std::wstring> IniFile::sections() const {
    std::vector<std::wstring> out;
    out.reserve(sections_.size());
    for (const Section& s : sections_) {
        if (!s.name.empty()) {
            out.push_back(s.name);
        }
    }
    return out;
}

/**
 * @brief Keys of a section in file order.
 */
std::vector<std::wstring> IniFile::keys(std::wstring_view section) const {
    std::vector<std::wstring> out;
    const Section* s = findSection(section);
    if (s == nullptr) {
        return out;
    }
    for (const Line& l : s->lines) {
        if (l.kind == Line::Kind::KeyValue) {
            out.push_back(l.key);
        }
    }
    return out;
}

void IniFile::clear() {
    preamble_.clear();
    sections_.clear();
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

IniFile::Section* IniFile::findSection(std::wstring_view name) {
    const std::wstring_view n = platform::trim(name);
    for (Section& s : sections_) {
        if (platform::iequals(s.name, n)) {
            return &s;
        }
    }
    return nullptr;
}

const IniFile::Section* IniFile::findSection(std::wstring_view name) const {
    const std::wstring_view n = platform::trim(name);
    for (const Section& s : sections_) {
        if (platform::iequals(s.name, n)) {
            return &s;
        }
    }
    return nullptr;
}

/**
 * @brief Finds or appends a section (with its "[name]" header line).
 */
IniFile::Section& IniFile::ensureSection(std::wstring_view name) {
    if (Section* s = findSection(name)) {
        return *s;
    }
    Section section;
    section.name = singleLine(platform::trim(name));
    if (!section.name.empty()) {
        Line header;
        header.kind = Line::Kind::Section;
        header.text = L"[" + section.name + L"]";
        section.lines.push_back(std::move(header));
    }
    sections_.push_back(std::move(section));
    return sections_.back();
}

} // namespace hh
