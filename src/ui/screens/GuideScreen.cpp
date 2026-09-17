// ---------------------------------------------------------------------------
// GuideScreen.cpp - the Guide tab.
//
// Layout: a fixed header row (title, spacer, "Copy summary", "Open GUIDE.md")
// above a ScrollView that renders the guide markdown with RichTextView.
// ---------------------------------------------------------------------------
#include "ui/screens/GuideScreen.h"

#include "core/Logger.h"
#include "ui/controls/Button.h"
#include "ui/controls/Label.h"
#include "ui/controls/RichTextView.h"
#include "ui/controls/ScrollView.h"
#include "ui/controls/Toast.h"
#include "ui/core/OverlayHost.h"
#include "ui/core/RootView.h"
#include "ui/screens/GuideContent.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Guide";

// Header paddings and the readable column width of the guide text.
constexpr float kHeaderTop = 12.0f;
constexpr float kHeaderBottom = 8.0f;
constexpr float kHeaderSide = 16.0f;
constexpr float kHeaderSpacing = 8.0f;
constexpr float kContentInset = 16.0f;
constexpr float kMaxContentWidth = 640.0f;
// Below this width the header buttons stack under the title.
constexpr float kHeaderWrapWidth = 380.0f;

} // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------
GuideScreen::GuideScreen(IGuideViewModel& vm)
    : vm_(vm)
{
    stack().axis = Axis::Vertical;
    buildHeader();
    buildBody();
}

GuideScreen::~GuideScreen() = default;

// ---------------------------------------------------------------------------
// measure
// ---------------------------------------------------------------------------
Size GuideScreen::measure(const Constraints& c)
{
    // Fill whatever box the shell gives; the scroll view absorbs the rest.
    Size s = Widget::measure(c);
    if (c.hasBoundedWidth()) s.w = c.maxW;
    if (c.hasBoundedHeight()) s.h = c.maxH;
    s.w = std::max(s.w, 0.0f);
    s.h = std::max(s.h, 0.0f);
    return c.constrain(s);
}

// ---------------------------------------------------------------------------
// header
// ---------------------------------------------------------------------------
void GuideScreen::buildHeader()
{
    // Title on the left, actions on the right; stacks when very narrow.
    auto header = std::make_unique<Widget>();
    header->stack().axis = Axis::Horizontal;
    header->stack().spacing = kHeaderSpacing;
    header->stack().padding = Insets{kHeaderTop, kHeaderSide, kHeaderBottom, kHeaderSide};
    header->stack().crossAlign = CrossAlign::Center;
    header->stack().wrapIfNarrowerThan = kHeaderWrapWidth;
    header_ = add(std::move(header));
    if (!header_) return;

    title_ = header_->add(std::make_unique<Label>(L"Export guide", typography::title(), LabelTone::Primary));
    header_->add(std::make_unique<Spacer>());

    // Copy summary: clipboard + toast confirmation.
    auto copy = std::make_unique<Button>(L"Copy summary", ButtonKind::Secondary);
    copy->setTooltipText(L"Copy the export recipe to the clipboard");
    copy->onClick = [this]() { copySummary(); };
    copyButton_ = header_->add(std::move(copy));

    // Open the markdown file in the default editor.
    auto open = std::make_unique<Button>(L"Open GUIDE.md", ButtonKind::Plain);
    open->setTooltipText(L"Open the guide in your markdown viewer");
    open->onClick = [this]() {
        HH_LOG_INFO(kLog, L"Opening GUIDE.md");
        vm_.openGuideFile();
    };
    openButton_ = header_->add(std::move(open));
}

// ---------------------------------------------------------------------------
// body
// ---------------------------------------------------------------------------
void GuideScreen::buildBody()
{
    auto scroll = std::make_unique<ScrollView>();
    scroll->layoutParams().flexGrow = 1.0f;
    scroll->setContentInsets(Insets::all(kContentInset));
    scroll_ = add(std::move(scroll));
    if (!scroll_) return;

    // The guide text, capped at a readable column width.
    auto text = std::make_unique<RichTextView>();
    text->setMaxContentWidth(kMaxContentWidth);

    // The model normally serves the embedded resource; fall back to the
    // compiled-in copy so the tab is never blank.
    std::wstring markdown = vm_.markdown();
    if (markdown.empty()) {
        HH_LOG_WARN(kLog, L"Guide markdown is empty; using the built-in copy");
        markdown = builtInGuideMarkdown();
    }
    text->setMarkdown(markdown);
    text_ = scroll_->setContentAs(std::move(text));
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
void GuideScreen::copySummary()
{
    vm_.copySummary();
    HH_LOG_INFO(kLog, L"Export summary copied to the clipboard");

    // The toast lives in the root's overlay; a detached screen just skips it.
    RootView* rv = root();
    if (!rv) return;
    ToastSpec spec;
    spec.text = L"Summary copied";
    spec.tone = ToastTone::Success;
    rv->overlay().toasts().show(std::move(spec));
}

} // namespace hh::ui
