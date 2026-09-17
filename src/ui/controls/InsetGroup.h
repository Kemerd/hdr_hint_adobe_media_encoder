// ---------------------------------------------------------------------------
// InsetGroup.h - macOS "grouped inset" settings list.
//
//   TOOLS                         <- header caption (uppercase, secondary)
//   +---------------------------+
//   | mkvmerge   C:\..  [Browse]|  <- SettingsRow (title, subtitle, accessory)
//   | LUT folder ...            |
//   +---------------------------+
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <memory>
#include <string>

namespace hh::ui {

class SettingsRow : public Widget {
public:
    SettingsRow(std::wstring title, std::wstring subtitle = L"");

    void setTitle(std::wstring title);
    void setSubtitle(std::wstring subtitle);
    /// Secondary/destructive colouring for the subtitle (e.g. "not found").
    void setSubtitleDestructive(bool on);
    /// The trailing control (toggle, popup, button, text field...).
    Widget* setAccessory(std::unique_ptr<Widget> accessory);
    [[nodiscard]] Widget* accessory() const noexcept { return accessory_; }
    /// Puts the accessory on its own line below the text (long paths / text fields).
    void setAccessoryBelow(bool below) { below_ = below; invalidateLayout(); }

    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paintSelf(Canvas& c) override;

private:
    Widget* title_ = nullptr;
    Widget* subtitle_ = nullptr;
    Widget* accessory_ = nullptr;
    bool below_ = false;
};

class InsetGroup : public Widget {
public:
    explicit InsetGroup(std::wstring header = L"");

    void setHeader(std::wstring header);
    /// Optional trailing widget in the header line (e.g. "+ Add" button).
    Widget* setHeaderAccessory(std::unique_ptr<Widget> w);
    SettingsRow* addRow(std::wstring title, std::wstring subtitle = L"");
    /// Any widget as a row (e.g. a watch-folder line with a remove button).
    Widget* addCustomRow(std::unique_ptr<Widget> row);
    void clearRows();
    /// Shown inside the group when there are no rows.
    void setEmptyText(std::wstring text);
    [[nodiscard]] size_t rowCount() const noexcept { return rows_ ? rows_->children().size() : 0; }

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    void paintOverlay(Canvas& c) override;

private:
    Widget* headerRow_ = nullptr;
    Widget* header_ = nullptr;
    Widget* headerAccessory_ = nullptr;
    Widget* rows_ = nullptr;
    Widget* empty_ = nullptr;
};

} // namespace hh::ui
