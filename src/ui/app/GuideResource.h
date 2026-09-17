// ---------------------------------------------------------------------------
// GuideResource.h - where the export guide's markdown comes from.
//
// The guide ships three ways so the Guide tab never comes up blank:
//   1. embedded in HdrHint.exe as RCDATA (resources/HdrHint.rc, id 201)
//   2. GUIDE.md next to the executable (copied by the build)
//   3. docs/GUIDE.md in the source tree (when running from a build folder)
// and, when all of those are missing (unit tests, a stripped exe), the text
// compiled into GuideContent.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <cstdint>
#include <string>

namespace hh::ui {

/**
 * @brief Loads the guide markdown from the first source that works.
 *
 * Order: embedded resource, "<exe>\GUIDE.md", "<exe>\..\..\docs\GUIDE.md",
 * then builtInGuideMarkdown(). Line endings are normalised to "\n".
 * Never throws; always returns non-empty text.
 */
std::wstring loadGuideMarkdown();

/**
 * @brief First on-disk copy of the guide that exists.
 * @return "<exe>\GUIDE.md" or the repo docs copy, or empty when neither exists.
 */
std::wstring locateGuideFile();

/**
 * @brief Decodes UTF-8 bytes (BOM optional) into a wide string with "\n" line endings.
 * @param bytes  raw UTF-8; may be null (returns empty)
 * @param size   number of bytes
 */
std::wstring decodeGuideBytes(const uint8_t* bytes, size_t size);

} // namespace hh::ui
