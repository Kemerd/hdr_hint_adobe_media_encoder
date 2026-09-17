// ---------------------------------------------------------------------------
// SettingsScreen.h - the Settings tab: grouped inset lists bound to
// ISettingsViewModel (tools, defaults, watch folders, behaviour, AME).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"
#include "ui/screens/ViewModels.h"

#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

class Button;
class InsetGroup;
class PopupButton;
class ScrollView;
class SegmentedControl;
class SettingsRow;
class TextField;
class ToggleSwitch;

class SettingsScreen : public Widget {
public:
    explicit SettingsScreen(ISettingsViewModel& vm);
    ~SettingsScreen() override;
    void refresh();

private:
    /// Fills the screen with the constraint box so the scroll view gets the space.
    Size measure(const Constraints& c) override;

    // ---- construction -------------------------------------------------------
    void buildTools();
    void buildDefaults();
    void buildWatchFolders();
    void buildBehaviour();
    void buildMediaEncoder();
    /// Adds a title/subtitle row with a trailing switch wired to @p apply.
    ToggleSwitch* addToggleRow(InsetGroup* group, std::wstring title, std::wstring subtitle,
                               std::function<void(bool)> apply);
    /// Adds a title/subtitle row with a trailing push button wired to @p onClick.
    Button* addButtonRow(InsetGroup* group, std::wstring title, std::wstring subtitle, std::wstring buttonText,
                         std::function<void()> onClick, SettingsRow** rowOut = nullptr);
    /// Adds a row with a trailing pop-up button; the callback receives the chosen value.
    PopupButton* addPopupRow(InsetGroup* group, std::wstring title, std::function<void(const std::wstring&)> apply);

    // ---- refresh helpers ----------------------------------------------------
    /// Re-creates the watch folder rows from the last view (deferred to measure()).
    void rebuildWatchFolders();
    void applyTools(const SettingsView& view, bool force);
    void applyDefaults(const SettingsView& view);
    void applyBehaviour(const SettingsView& view);
    void applyMediaEncoder(const SettingsView& view, bool force);

    ISettingsViewModel& vm_;
    /// True while refresh() pushes values into controls: callbacks must not write back.
    bool updating_ = false;
    /// The watch folder rows need re-creating before the next layout pass.
    bool watchDirty_ = false;
    /// The last view applied (diffing avoids needless repaints and rebuilds).
    SettingsView lastView_;
    bool hasLastView_ = false;

    ScrollView* scroll_ = nullptr;
    Widget* groups_ = nullptr;

    // Tools
    SettingsRow* mkvmergeRow_ = nullptr;
    SettingsRow* lutFolderRow_ = nullptr;

    // Defaults
    PopupButton* lutPq_ = nullptr;
    PopupButton* lutHlg_ = nullptr;
    PopupButton* presetPq_ = nullptr;
    PopupButton* presetHlg_ = nullptr;
    TextField* suffix_ = nullptr;
    ToggleSwitch* attachLut_ = nullptr;

    // Watch folders
    InsetGroup* watchGroup_ = nullptr;

    // Behaviour
    ToggleSwitch* autoProcess_ = nullptr;
    ToggleSwitch* recycle_ = nullptr;
    ToggleSwitch* dock_ = nullptr;
    ToggleSwitch* alwaysOnTop_ = nullptr;
    ToggleSwitch* minimizeToTray_ = nullptr;
    ToggleSwitch* startMinimized_ = nullptr;
    ToggleSwitch* startWithWindows_ = nullptr;
    ToggleSwitch* quitWithAme_ = nullptr;
    SegmentedControl* appearance_ = nullptr;
    SegmentedControl* accent_ = nullptr;
    ToggleSwitch* reduceTransparency_ = nullptr;

    // Media Encoder
    SettingsRow* panelRow_ = nullptr;
    Button* panelButton_ = nullptr;
    SettingsRow* aboutRow_ = nullptr;
};

} // namespace hh::ui
