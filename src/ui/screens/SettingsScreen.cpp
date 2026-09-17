// ---------------------------------------------------------------------------
// SettingsScreen.cpp - the Settings tab.
//
// Layout: a ScrollView whose content is a vertical stack of InsetGroups.
// Every control writes straight to the view model; the view model answers
// with onChanged, which calls refresh() to push the authoritative values
// back into the controls. A guard flag keeps that round trip from echoing.
// ---------------------------------------------------------------------------
#include "ui/screens/SettingsScreen.h"

#include "core/Logger.h"
#include "ui/controls/Button.h"
#include "ui/controls/InsetGroup.h"
#include "ui/controls/Label.h"
#include "ui/controls/PopupButton.h"
#include "ui/controls/ScrollView.h"
#include "ui/controls/SegmentedControl.h"
#include "ui/controls/TextField.h"
#include "ui/controls/ToggleSwitch.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Settings";

// Spacing between the inset groups and the scroll view's edge padding.
constexpr float kGroupSpacing = 20.0f;
constexpr float kContentInset = 16.0f;
// Trailing text field width for the output suffix.
constexpr float kSuffixWidth = 180.0f;
// Fixed segment width of the appearance switcher.
constexpr float kAppearanceSegmentWidth = 70.0f;

// Characters Windows refuses in a file name; the suffix becomes part of one.
constexpr std::wstring_view kForbiddenSuffixChars = L"\\/:*?\"<>|";

/**
 * @brief Converts view-model choices into pop-up items (label, subtitle, value).
 */
std::vector<PopupItem> toPopupItems(const std::vector<Choice>& choices)
{
    std::vector<PopupItem> items;
    items.reserve(choices.size());
    for (const Choice& choice : choices) {
        PopupItem item;
        item.label = choice.label;
        item.subtitle = choice.subtitle;
        item.value = choice.value;
        items.push_back(std::move(item));
    }
    return items;
}

/**
 * @brief True when two item lists show the same labels, subtitles and values.
 *
 * Used to skip setItems() on refresh, which would otherwise drop the selection
 * and repaint for nothing.
 */
bool sameItems(const std::vector<PopupItem>& a, const std::vector<PopupItem>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].label != b[i].label || a[i].value != b[i].value || a[i].subtitle != b[i].subtitle) return false;
    }
    return true;
}

/**
 * @brief The last path component, for placeholders that show a missing LUT.
 */
std::wstring fileNameOf(const std::wstring& path)
{
    const size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return path;
    return path.substr(cut + 1);
}

/**
 * @brief Pushes items + the configured value into a pop-up without firing callbacks.
 *
 * When the configured value is not in the list (a LUT file that vanished, a
 * preset id from a newer build) the control shows the raw value as its
 * placeholder rather than silently selecting something else.
 */
void syncPopup(PopupButton* popup, std::vector<PopupItem> items, const std::wstring& value, bool valueIsPath)
{
    if (!popup) return;

    // Only replace the list when it changed: setItems resets the selection.
    if (!sameItems(popup->items(), items)) popup->setItems(std::move(items));

    // Select by value; fall back to a descriptive placeholder when absent.
    if (popup->setSelectedValue(value)) return;
    popup->setSelectedIndex(-1);
    if (value.empty()) {
        popup->setPlaceholder(L"None");
    } else {
        popup->setPlaceholder(valueIsPath ? fileNameOf(value) + L" (missing)" : value + L" (unknown)");
    }
}

/**
 * @brief Reads the value of a pop-up item by index; false when out of range.
 */
bool popupValueAt(const PopupButton* popup, int index, std::wstring& out)
{
    if (!popup || index < 0) return false;
    const auto& items = popup->items();
    const size_t i = static_cast<size_t>(index);
    if (i >= items.size()) return false;
    out = items[i].value;
    return true;
}

/**
 * @brief Validator for the output suffix: non-empty and file-name safe.
 * @return An error message, or empty when the text is acceptable.
 */
std::wstring validateSuffix(const std::wstring& text)
{
    if (text.empty()) return L"The suffix cannot be empty";
    if (text.find_first_of(kForbiddenSuffixChars) != std::wstring::npos) {
        return L"The suffix cannot contain \\ / : * ? \" < > |";
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------
SettingsScreen::SettingsScreen(ISettingsViewModel& vm)
    : vm_(vm)
{
    // The screen is a vertical stack holding one flexible scroll view.
    stack().axis = Axis::Vertical;

    // The scroll view owns a vertical stack of inset groups with 16 dip of
    // breathing room around them.
    auto scroll = std::make_unique<ScrollView>();
    scroll->layoutParams().flexGrow = 1.0f;
    scroll->setContentInsets(Insets::all(kContentInset));
    scroll_ = add(std::move(scroll));

    auto groups = std::make_unique<Widget>();
    groups->stack().axis = Axis::Vertical;
    groups->stack().spacing = kGroupSpacing;
    groups->stack().crossAlign = CrossAlign::Stretch;
    groups_ = scroll_->setContent(std::move(groups));

    // Build every group, then pull the current values in.
    buildTools();
    buildDefaults();
    buildWatchFolders();
    buildBehaviour();
    buildMediaEncoder();

    // Listen for model changes for as long as this screen exists.
    vm_.onChanged = [this]() { refresh(); };

    // First population: refresh() marks the folder rows dirty; build them now
    // so the initial layout already has them (no callback is running yet).
    refresh();
    rebuildWatchFolders();
}

SettingsScreen::~SettingsScreen()
{
    // Nothing must call back into a dead screen.
    vm_.onChanged = nullptr;
}

// ---------------------------------------------------------------------------
// measure
// ---------------------------------------------------------------------------
Size SettingsScreen::measure(const Constraints& c)
{
    // Row re-creation is deferred to here: measure() runs from the frame
    // loop, never from inside a row widget's own click handler, so the
    // widget being destroyed is never the one currently executing.
    if (watchDirty_) rebuildWatchFolders();

    // A screen always fills the space it is given; the scroll view inside
    // takes care of content taller than the window.
    Size s = Widget::measure(c);
    if (c.hasBoundedWidth()) s.w = c.maxW;
    if (c.hasBoundedHeight()) s.h = c.maxH;
    s.w = std::max(s.w, 0.0f);
    s.h = std::max(s.h, 0.0f);
    return c.constrain(s);
}

// ---------------------------------------------------------------------------
// row helpers
// ---------------------------------------------------------------------------
ToggleSwitch* SettingsScreen::addToggleRow(InsetGroup* group, std::wstring title, std::wstring subtitle,
                                           std::function<void(bool)> apply)
{
    if (!group) {
        HH_LOG_ERROR(kLog, L"addToggleRow: no group for '{}'", title);
        return nullptr;
    }
    SettingsRow* row = group->addRow(std::move(title), std::move(subtitle));
    if (!row) return nullptr;

    // The switch writes through to the model unless refresh() is driving it.
    auto toggle = std::make_unique<ToggleSwitch>(false);
    ToggleSwitch* raw = toggle.get();
    toggle->onChanged = [this, apply = std::move(apply)](bool on) {
        if (updating_ || !apply) return;
        apply(on);
    };
    row->setAccessory(std::move(toggle));
    return raw;
}

Button* SettingsScreen::addButtonRow(InsetGroup* group, std::wstring title, std::wstring subtitle,
                                     std::wstring buttonText, std::function<void()> onClick, SettingsRow** rowOut)
{
    if (rowOut) *rowOut = nullptr;
    if (!group) {
        HH_LOG_ERROR(kLog, L"addButtonRow: no group for '{}'", title);
        return nullptr;
    }
    SettingsRow* row = group->addRow(std::move(title), std::move(subtitle));
    if (!row) return nullptr;
    if (rowOut) *rowOut = row;

    // Secondary buttons: these are actions, not the primary call of the tab.
    auto button = std::make_unique<Button>(std::move(buttonText), ButtonKind::Secondary);
    Button* raw = button.get();
    button->onClick = [this, onClick = std::move(onClick)]() {
        if (updating_ || !onClick) return;
        onClick();
    };
    row->setAccessory(std::move(button));
    return raw;
}

PopupButton* SettingsScreen::addPopupRow(InsetGroup* group, std::wstring title,
                                         std::function<void(const std::wstring&)> apply)
{
    if (!group) {
        HH_LOG_ERROR(kLog, L"addPopupRow: no group for '{}'", title);
        return nullptr;
    }
    SettingsRow* row = group->addRow(std::move(title));
    if (!row) return nullptr;

    // Items arrive through refresh(); the callback resolves index -> value.
    auto popup = std::make_unique<PopupButton>();
    PopupButton* raw = popup.get();
    popup->onChanged = [this, raw, apply = std::move(apply)](int index) {
        if (updating_ || !apply) return;
        std::wstring value;
        if (!popupValueAt(raw, index, value)) {
            HH_LOG_WARN(kLog, L"Pop-up index {} out of range", index);
            return;
        }
        apply(value);
    };
    row->setAccessory(std::move(popup));
    return raw;
}

// ---------------------------------------------------------------------------
// TOOLS
// ---------------------------------------------------------------------------
void SettingsScreen::buildTools()
{
    if (!groups_) return;
    InsetGroup* group = groups_->add(std::make_unique<InsetGroup>(L"TOOLS"));
    if (!group) return;

    // mkvmerge: status line under the title, Browse picks the executable.
    addButtonRow(group, L"mkvmerge", L"", L"Browse", [this]() { vm_.browseMkvmerge(); }, &mkvmergeRow_);

    // LUT folder: the directory scanned for .cube files.
    addButtonRow(group, L"LUT folder", L"", L"Browse", [this]() { vm_.browseLutFolder(); }, &lutFolderRow_);
}

// ---------------------------------------------------------------------------
// DEFAULTS
// ---------------------------------------------------------------------------
void SettingsScreen::buildDefaults()
{
    if (!groups_) return;
    InsetGroup* group = groups_->add(std::make_unique<InsetGroup>(L"DEFAULTS"));
    if (!group) return;

    // Per-transfer default LUT and preset.
    lutPq_ = addPopupRow(group, L"Default LUT (PQ)",
                         [this](const std::wstring& v) { vm_.setDefaultLut(TransferKind::PQ, v); });
    lutHlg_ = addPopupRow(group, L"Default LUT (HLG)",
                          [this](const std::wstring& v) { vm_.setDefaultLut(TransferKind::HLG, v); });
    presetPq_ = addPopupRow(group, L"PQ preset",
                            [this](const std::wstring& v) { vm_.setDefaultPreset(TransferKind::PQ, v); });
    presetHlg_ = addPopupRow(group, L"HLG preset",
                             [this](const std::wstring& v) { vm_.setDefaultPreset(TransferKind::HLG, v); });

    // Output suffix: a validated text field that commits on Enter / blur.
    if (SettingsRow* row = group->addRow(L"Output suffix")) {
        auto field = std::make_unique<TextField>(L"", L"_REC709_HINT");
        TextField* raw = field.get();
        field->setWidthDips(kSuffixWidth);
        field->setValidator([](const std::wstring& text) { return validateSuffix(text); });
        field->onSubmit = [this, raw](const std::wstring& text) {
            if (updating_ || !raw) return;
            // Invalid text stays in the field with its red outline; the model
            // only ever receives a suffix the file system will accept.
            if (!raw->valid()) {
                HH_LOG_WARN(kLog, L"Suffix rejected: {}", raw->validationError());
                return;
            }
            if (hasLastView_ && text == lastView_.suffix) return;
            vm_.setSuffix(text);
        };
        row->setAccessory(std::move(field));
        suffix_ = raw;
    }

    // Attach the default LUT to new jobs.
    attachLut_ = addToggleRow(group, L"Attach LUT by default", L"", [this](bool on) { vm_.setAttachLutByDefault(on); });
}

// ---------------------------------------------------------------------------
// WATCH FOLDERS
// ---------------------------------------------------------------------------
void SettingsScreen::buildWatchFolders()
{
    if (!groups_) return;
    watchGroup_ = groups_->add(std::make_unique<InsetGroup>(L"WATCH FOLDERS"));
    if (!watchGroup_) return;

    // "+ Add" lives in the header line; it opens the model's folder picker.
    auto addButton = std::make_unique<Button>(L"+ Add", ButtonKind::Plain);
    addButton->setCompact(true);
    addButton->setTooltipText(L"Watch another export folder");
    addButton->onClick = [this]() {
        if (updating_) return;
        vm_.addWatchFolder();
    };
    watchGroup_->setHeaderAccessory(std::move(addButton));

    // Shown inside the group while there are no rows.
    watchGroup_->setEmptyText(L"No folders. The AME encoding log is still tailed.");
}

void SettingsScreen::rebuildWatchFolders()
{
    watchDirty_ = false;
    if (!watchGroup_) return;

    // Start from an empty group; the rows are cheap to re-create.
    watchGroup_->clearRows();

    for (const std::wstring& folder : lastView_.watchFolders) {
        if (folder.empty()) continue;

        // One horizontal line: path (middle-trimmed) + trash button.
        auto row = std::make_unique<Widget>();
        row->stack().axis = Axis::Horizontal;
        row->stack().spacing = 8.0f;
        row->stack().padding = Insets{2.0f, 10.0f, 2.0f, 16.0f};
        row->stack().crossAlign = CrossAlign::Center;

        auto path = std::make_unique<Label>(folder, typography::body(), LabelTone::Primary);
        path->setTrimming(Trimming::Middle);
        path->setTooltipText(folder);
        path->layoutParams().flexGrow = 1.0f;
        row->addChild(std::move(path));

        // The click hands the model a copy of the path; the row itself is
        // only replaced on the next layout pass (see measure()).
        auto remove = std::make_unique<IconButton>(IconId::Trash);
        remove->setTooltipText(L"Stop watching this folder");
        remove->onClick = [this, folder]() {
            if (updating_) return;
            HH_LOG_INFO(kLog, L"Removing watch folder {}", folder);
            vm_.removeWatchFolder(folder);
        };
        row->addChild(std::move(remove));

        watchGroup_->addCustomRow(std::move(row));
    }
    invalidateLayout();
}

// ---------------------------------------------------------------------------
// BEHAVIOUR
// ---------------------------------------------------------------------------
void SettingsScreen::buildBehaviour()
{
    if (!groups_) return;
    InsetGroup* group = groups_->add(std::make_unique<InsetGroup>(L"BEHAVIOUR"));
    if (!group) return;

    autoProcess_ = addToggleRow(group, L"Auto-process new exports", L"", [this](bool on) { vm_.setAutoProcess(on); });
    recycle_ = addToggleRow(group, L"Move original to Recycle Bin after success", L"",
                            [this](bool on) { vm_.setRecycleOriginal(on); });
    dock_ = addToggleRow(group, L"Dock inside Media Encoder",
                         L"Covers the HDR Hint panel (Window > Extensions > HDR Hint) or any panel you pick",
                         [this](bool on) { vm_.setDockInsideAme(on); });
    alwaysOnTop_ = addToggleRow(group, L"Always on top (floating)", L"", [this](bool on) { vm_.setAlwaysOnTop(on); });
    minimizeToTray_ = addToggleRow(group, L"Minimize to tray", L"", [this](bool on) { vm_.setMinimizeToTray(on); });
    startMinimized_ = addToggleRow(group, L"Start minimized to tray", L"", [this](bool on) { vm_.setStartMinimized(on); });
    startWithWindows_ = addToggleRow(group, L"Start with Windows",
                                     L"Only needed if Media Encoder does not start HDR Hint itself",
                                     [this](bool on) { vm_.setStartWithWindows(on); });
    quitWithAme_ = addToggleRow(group, L"Quit when Media Encoder quits", L"", [this](bool on) { vm_.setQuitWithAme(on); });

    // Appearance: System / Dark / Light with fixed-width segments.
    if (SettingsRow* row = group->addRow(L"Appearance")) {
        auto seg = std::make_unique<SegmentedControl>(std::vector<std::wstring>{L"System", L"Dark", L"Light"}, 0);
        seg->setSegmentWidth(kAppearanceSegmentWidth);
        seg->onChanged = [this](int index) {
            if (updating_) return;
            vm_.setAppearance(index);
        };
        appearance_ = seg.get();
        row->setAccessory(std::move(seg));
    }

    // Accent: the app blue or the Windows accent colour.
    if (SettingsRow* row = group->addRow(L"Accent")) {
        auto seg = std::make_unique<SegmentedControl>(std::vector<std::wstring>{L"Blue", L"System"}, 0);
        seg->onChanged = [this](int index) {
            if (updating_) return;
            vm_.setAccent(index);
        };
        accent_ = seg.get();
        row->setAccessory(std::move(seg));
    }

    reduceTransparency_ = addToggleRow(group, L"Reduce transparency", L"",
                                       [this](bool on) { vm_.setReduceTransparency(on); });
}

// ---------------------------------------------------------------------------
// MEDIA ENCODER
// ---------------------------------------------------------------------------
void SettingsScreen::buildMediaEncoder()
{
    if (!groups_) return;
    InsetGroup* group = groups_->add(std::make_unique<InsetGroup>(L"MEDIA ENCODER"));
    if (!group) return;

    // The button label flips between Install and Update in refresh().
    panelButton_ = addButtonRow(group, L"AME panel", L"", L"Install panel", [this]() { vm_.installPanel(); }, &panelRow_);

    addButtonRow(group, L"Logs", L"hdrhint.log and its rotated copies", L"Open log folder",
                 [this]() { vm_.openLogFolder(); });

    addButtonRow(group, L"mkvmerge", L"Run detection again after installing or updating MKVToolNix", L"Re-check",
                 [this]() { vm_.reprobeMkvmerge(); });

    // Version line, no accessory.
    aboutRow_ = group->addRow(L"About", L"");
}

// ---------------------------------------------------------------------------
// refresh
// ---------------------------------------------------------------------------
void SettingsScreen::refresh()
{
    // A nested refresh can only come from a control echoing a value we just
    // set; the guard already suppressed the model write, so there is nothing
    // newer to apply.
    if (updating_) {
        HH_LOG_DEBUG(kLog, L"refresh() re-entered; ignored");
        return;
    }

    const SettingsView view = vm_.view();
    const bool force = !hasLastView_;
    updating_ = true;

    applyTools(view, force);
    applyDefaults(view);
    applyBehaviour(view);
    applyMediaEncoder(view, force);

    // Watch folder rows are rebuilt at the next layout pass (see measure()).
    if (force || lastView_.watchFolders != view.watchFolders) {
        watchDirty_ = true;
        invalidateLayout();
    }

    lastView_ = view;
    hasLastView_ = true;
    updating_ = false;
    HH_LOG_DEBUG(kLog, L"refresh applied ({} watch folders)", view.watchFolders.size());
}

void SettingsScreen::applyTools(const SettingsView& view, bool force)
{
    // mkvmerge status: the version + path when found, red text otherwise.
    if (mkvmergeRow_ && (force || view.mkvmergeStatus != lastView_.mkvmergeStatus || view.mkvmergeOk != lastView_.mkvmergeOk)) {
        mkvmergeRow_->setSubtitle(view.mkvmergeStatus.empty() ? (view.mkvmergeOk ? L"" : L"not found") : view.mkvmergeStatus);
        mkvmergeRow_->setSubtitleDestructive(!view.mkvmergeOk);
    }

    // LUT folder path.
    if (lutFolderRow_ && (force || view.lutFolder != lastView_.lutFolder)) {
        lutFolderRow_->setSubtitle(view.lutFolder.empty() ? L"Not set" : view.lutFolder);
    }
}

void SettingsScreen::applyDefaults(const SettingsView& view)
{
    // LUT and preset lists can change whenever the LUT folder is rescanned,
    // so they are always re-read; syncPopup only touches what differs.
    const std::vector<PopupItem> luts = toPopupItems(vm_.lutChoices());
    syncPopup(lutPq_, luts, view.defaultLutPq, true);
    syncPopup(lutHlg_, luts, view.defaultLutHlg, true);
    syncPopup(presetPq_, toPopupItems(vm_.presetChoices(TransferKind::PQ)), view.presetPq, false);
    syncPopup(presetHlg_, toPopupItems(vm_.presetChoices(TransferKind::HLG)), view.presetHlg, false);

    // Never clobber a suffix the user is editing right now.
    if (suffix_ && !suffix_->focused() && suffix_->text() != view.suffix) {
        suffix_->setText(view.suffix, false);
    }

    if (attachLut_ && attachLut_->isOn() != view.attachLutByDefault) attachLut_->setOn(view.attachLutByDefault);
}

void SettingsScreen::applyBehaviour(const SettingsView& view)
{
    // Each switch only moves when the model disagrees with what it shows.
    const auto sync = [](ToggleSwitch* toggle, bool on) {
        if (toggle && toggle->isOn() != on) toggle->setOn(on);
    };
    sync(autoProcess_, view.autoProcess);
    sync(recycle_, view.recycleOriginal);
    sync(dock_, view.dockInsideAme);
    sync(alwaysOnTop_, view.alwaysOnTop);
    sync(minimizeToTray_, view.minimizeToTray);
    sync(startMinimized_, view.startMinimized);
    sync(startWithWindows_, view.startWithWindows);
    sync(quitWithAme_, view.quitWithAme);
    sync(reduceTransparency_, view.reduceTransparency);

    // Segmented values are clamped to the items each control actually has.
    if (appearance_) {
        const int mode = std::clamp(view.appearance, 0, 2);
        if (appearance_->selected() != mode) appearance_->setSelected(mode);
    }
    if (accent_) {
        const int mode = std::clamp(view.accent, 0, 1);
        if (accent_->selected() != mode) accent_->setSelected(mode);
    }
}

void SettingsScreen::applyMediaEncoder(const SettingsView& view, bool force)
{
    // Panel state drives both the subtitle and the button label.
    if (panelRow_ && (force || view.panelStatus != lastView_.panelStatus)) {
        panelRow_->setSubtitle(view.panelStatus.empty() ? L"Not installed" : view.panelStatus);
    }
    if (panelButton_) {
        const wchar_t* label = view.panelInstalled ? L"Update panel" : L"Install panel";
        if (panelButton_->text() != label) panelButton_->setText(label);
    }

    if (aboutRow_ && (force || view.aboutLine != lastView_.aboutLine)) {
        aboutRow_->setSubtitle(view.aboutLine);
    }
}

} // namespace hh::ui
