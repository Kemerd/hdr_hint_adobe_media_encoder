// ---------------------------------------------------------------------------
// GuideContent.h - the text behind the Guide tab.
//
// Two pieces of content live here so the app model, the Guide screen and the
// screenshot harness all draw from the same source:
//   * guideSummaryText()      - the one-line export recipe for the clipboard
//   * builtInGuideMarkdown()  - a compact copy of docs/GUIDE.md used when the
//                               embedded resource cannot be loaded
// ---------------------------------------------------------------------------
#pragma once

#include "ui/screens/ViewModels.h"

#include <string>

namespace hh::ui {

/**
 * @brief Builds the short export recipe that "Copy summary" puts on the clipboard.
 *
 * The recipe lists every Media Encoder setting that matters for an HDR export,
 * separated by " · ". When @p view is given, the output naming rule
 * ("Output: <stem><suffix>.mkv") is appended using the configured suffix.
 *
 * @param view  Current settings, or nullptr for the recipe alone.
 * @return      A single line of text (no trailing newline).
 */
[[nodiscard]] std::wstring guideSummaryText(const SettingsView* view);

/**
 * @brief Returns a compact, faithful copy of docs/GUIDE.md as markdown.
 *
 * This is the fallback the Guide tab renders when the RCDATA resource is
 * missing or unreadable; it uses only the markdown subset RichTextView knows.
 *
 * @return  Markdown text (LF line endings).
 */
[[nodiscard]] std::wstring builtInGuideMarkdown();

} // namespace hh::ui
