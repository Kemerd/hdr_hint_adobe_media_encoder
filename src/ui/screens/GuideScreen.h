// ---------------------------------------------------------------------------
// GuideScreen.h - the Guide tab: a header with "Copy summary" / "Open
// GUIDE.md" and the rendered export guide in a scroll view.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"
#include "ui/screens/ViewModels.h"

namespace hh::ui {

class Button;
class Label;
class RichTextView;
class ScrollView;

class GuideScreen : public Widget {
public:
    explicit GuideScreen(IGuideViewModel& vm);
    ~GuideScreen() override;

private:
    /// Fills the screen with the constraint box so the scroll view gets the space.
    Size measure(const Constraints& c) override;

    void buildHeader();
    void buildBody();
    /// Copies the recipe to the clipboard and confirms with a toast.
    void copySummary();

    IGuideViewModel& vm_;
    Widget* header_ = nullptr;
    Label* title_ = nullptr;
    Button* copyButton_ = nullptr;
    Button* openButton_ = nullptr;
    ScrollView* scroll_ = nullptr;
    RichTextView* text_ = nullptr;
};

} // namespace hh::ui
