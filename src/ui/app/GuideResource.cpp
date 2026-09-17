// ---------------------------------------------------------------------------
// GuideResource.cpp - embedded resource -> files on disk -> built-in text.
// ---------------------------------------------------------------------------
#include "ui/app/GuideResource.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"
#include "ui/screens/GuideContent.h"

#include <string_view>
#include <vector>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Guide";

// IDR_GUIDE_MARKDOWN from resources/resource.h. The literal is repeated here
// because that header belongs to the executable target only; the UI library
// must not reach into it.
constexpr WORD kGuideResourceId = 201;

// A guide bigger than this is not a guide; refuse to slurp it.
constexpr uint64_t kMaxGuideBytes = 4ull * 1024 * 1024;

/**
 * @brief Reads the RCDATA resource out of the running executable.
 * @return The decoded markdown, or empty when the resource is absent (tests,
 *         a host that is not HdrHint.exe) or unreadable.
 */
std::wstring loadEmbeddedGuide() {
    // A null module means "the executable that started this process".
    HMODULE module = ::GetModuleHandleW(nullptr);
    if (!module) {
        HH_LOG_WARN(kLog, L"GetModuleHandleW failed ({})", ::GetLastError());
        return {};
    }

    // Resource lookup: not finding it is normal outside the real exe.
    HRSRC found = ::FindResourceW(module, MAKEINTRESOURCEW(kGuideResourceId), RT_RCDATA);
    if (!found) {
        HH_LOG_DEBUG(kLog, L"embedded guide resource not present ({})", ::GetLastError());
        return {};
    }
    HGLOBAL loaded = ::LoadResource(module, found);
    if (!loaded) {
        HH_LOG_WARN(kLog, L"LoadResource failed ({})", ::GetLastError());
        return {};
    }

    // Resources live in the image; LockResource returns a pointer, nothing to free.
    const void* data = ::LockResource(loaded);
    const DWORD size = ::SizeofResource(module, found);
    if (!data || size == 0) {
        HH_LOG_WARN(kLog, L"embedded guide resource is empty");
        return {};
    }
    return decodeGuideBytes(static_cast<const uint8_t*>(data), static_cast<size_t>(size));
}

/**
 * @brief Reads and decodes one markdown file; empty when missing or unreadable.
 */
std::wstring readGuideFile(const std::wstring& file) {
    if (file.empty() || !platform::isFile(file)) {
        return {};
    }
    auto bytes = platform::readAll(file, kMaxGuideBytes);
    if (!bytes) {
        HH_LOG_WARN(kLog, L"cannot read {}: {}", file, bytes.error().toString());
        return {};
    }
    const std::vector<uint8_t>& buffer = bytes.value();
    return decodeGuideBytes(buffer.data(), buffer.size());
}

/**
 * @brief The on-disk locations we try, in order: next to the exe, then the
 *        source tree's docs folder (two levels up from build\Release).
 */
std::vector<std::wstring> guideFileCandidates() {
    std::vector<std::wstring> out;
    const std::wstring exeDir = platform::exeDirectory();
    if (exeDir.empty()) {
        HH_LOG_WARN(kLog, L"exe directory unknown; skipping on-disk guide lookup");
        return out;
    }
    out.push_back(path::join(exeDir, L"GUIDE.md"));

    // fullPath collapses the "..\.." so logs and Explorer show a clean path.
    const std::wstring repoCopy = platform::fullPath(path::join(exeDir, L"..\\..\\docs\\GUIDE.md"));
    if (!repoCopy.empty()) {
        out.push_back(repoCopy);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// decodeGuideBytes
// ---------------------------------------------------------------------------

std::wstring decodeGuideBytes(const uint8_t* bytes, size_t size) {
    if (!bytes || size == 0) {
        return {};
    }
    // Editors on Windows love to prepend a UTF-8 BOM; it is not content.
    if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bytes += 3;
        size -= 3;
    }
    if (size == 0) {
        return {};
    }

    // toWide never throws; invalid sequences become U+FFFD.
    std::wstring text = platform::toWide(std::string_view(reinterpret_cast<const char*>(bytes), size));

    // One line ending convention for the renderer: "\n". Handle CRLF first,
    // then any stray classic-Mac "\r" that survived.
    text = platform::replaceAll(text, L"\r\n", L"\n");
    text = platform::replaceAll(text, L"\r", L"\n");
    return text;
}

// ---------------------------------------------------------------------------
// locateGuideFile
// ---------------------------------------------------------------------------

std::wstring locateGuideFile() {
    for (const std::wstring& candidate : guideFileCandidates()) {
        if (!candidate.empty() && platform::isFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// loadGuideMarkdown
// ---------------------------------------------------------------------------

std::wstring loadGuideMarkdown() {
    // 1. The copy baked into the executable is the authoritative one.
    std::wstring text = loadEmbeddedGuide();
    if (!text.empty()) {
        return text;
    }

    // 2./3. Files on disk (build output, then the source tree).
    for (const std::wstring& candidate : guideFileCandidates()) {
        text = readGuideFile(candidate);
        if (!text.empty()) {
            HH_LOG_INFO(kLog, L"guide loaded from {}", candidate);
            return text;
        }
    }

    // 4. Last resort: the text compiled into the UI library.
    HH_LOG_INFO(kLog, L"guide resource and files unavailable; using the built-in text");
    text = builtInGuideMarkdown();
    if (text.empty()) {
        // Even the built-in copy is empty: give the renderer something honest.
        text = L"# HDR Hint\n\nThe export guide could not be loaded.\n";
    }
    return text;
}

} // namespace hh::ui
