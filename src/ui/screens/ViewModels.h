// ---------------------------------------------------------------------------
// ViewModels.h - the interfaces the screens bind to.
//
// The app implements them over the Engine/Settings; the screenshot harness
// implements them with canned data. Screens never include core headers other
// than JobModel.h (for the enums).
// ---------------------------------------------------------------------------
#pragma once

#include "core/JobModel.h"
#include "platform/Win.h"

#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

/// One row of the queue.
struct JobView {
    JobId id = 0;
    uint32_t generation = 1;
    std::wstring name;              ///< file name
    std::wstring subtitle;          ///< "3840x2160 · 59.94 fps · Rec.2100 PQ · Nvidia · out: clip_REC709_HINT.mkv"
    JobState state = JobState::Discovered;
    std::wstring stateLabel;        ///< "Encoding 43%", "Muxing 12%", "Done", ...
    std::wstring stateReason;       ///< Held/Failed text
    TransferKind transfer = TransferKind::Unknown;
    float progress = -1.0f;         ///< -1 = indeterminate / none
    bool showProgress = false;
    bool held = false;
    std::wstring presetId;
    std::wstring lutPath;           ///< empty = none
    bool attachLut = false;
    std::wstring hintPath;
    std::wstring outputPath;
    bool canRun = false;            ///< Run / Retry / Run anyway
    std::wstring runLabel;          ///< "Run", "Retry", "Run anyway", "Resume"
    bool canHold = false;
    bool canReveal = false;         ///< output or hint exists
    bool canRevealHint = false;
    bool canRemove = true;
    bool canEditPlan = false;       ///< preset/LUT popups enabled
    std::wstring mkvmergeCommand;   ///< for "Copy mkvmerge command"
    std::wstring lastError;         ///< mkvmerge's last error line
};

struct Choice {
    std::wstring label;
    std::wstring value;
    std::wstring subtitle;
};

/**
 * @brief Value of the "Choose a .cube file..." entry in every LUT chooser.
 *
 * '?' and ':' are illegal in Windows file names, so this can never collide
 * with a real LUT path. Screens that see it selected call browseLut()
 * instead of treating it as a path.
 */
inline constexpr const wchar_t* kBrowseLutValue = L"?hdrhint:browse-lut?";

class IQueueViewModel {
public:
    virtual ~IQueueViewModel() = default;
    [[nodiscard]] virtual std::vector<JobView> jobs() const = 0;
    [[nodiscard]] virtual std::vector<Choice> presetChoices(TransferKind t) const = 0;
    [[nodiscard]] virtual std::vector<Choice> lutChoices() const = 0;     ///< first entry is "None" with empty value
    virtual void run(JobId id) = 0;
    virtual void hold(JobId id) = 0;
    virtual void resume(JobId id) = 0;
    virtual void remove(JobId id) = 0;
    virtual void reveal(JobId id, bool hintFile) = 0;
    virtual void setPreset(JobId id, const std::wstring& presetId) = 0;
    virtual void setLut(JobId id, const std::wstring& lutPath) = 0;
    /// Opens a .cube picker and applies the result to the job; no-op on cancel.
    virtual void browseLut(JobId id) = 0;
    virtual void setAttachLut(JobId id, bool attach) = 0;
    virtual void copyCommand(JobId id) = 0;
    [[nodiscard]] virtual bool autoProcess() const = 0;
    virtual void setAutoProcess(bool on) = 0;
    [[nodiscard]] virtual std::wstring footerSummary() const = 0;   ///< "3 jobs · 1 done"
    virtual void addFiles(const std::vector<std::wstring>& paths) = 0;  ///< drag & drop
    /// Screens subscribe to be re-rendered.
    std::function<void()> onChanged;
};

struct SettingsView {
    std::wstring mkvmergePath;      ///< as configured (may be empty = auto)
    std::wstring mkvmergeStatus;    ///< "v82.0 · C:\Program Files\MKVToolNix" or "not found"
    bool mkvmergeOk = false;
    std::wstring lutFolder;
    std::wstring defaultLutPq;
    std::wstring defaultLutHlg;
    std::wstring presetPq;
    std::wstring presetHlg;
    std::wstring suffix;
    bool attachLutByDefault = true;
    std::vector<std::wstring> watchFolders;
    bool autoProcess = true;
    bool autoProcessAme = true;
    bool autoProcessWatched = true;
    bool recycleOriginal = true;
    bool dockInsideAme = true;
    bool alwaysOnTop = true;
    bool minimizeToTray = true;
    bool startMinimized = false;
    bool startWithWindows = false;
    bool quitWithAme = true;
    bool showOnAmeLaunch = true;
    int appearance = 0;             ///< 0 system, 1 dark, 2 light
    int accent = 0;                 ///< 0 blue, 1 system
    bool reduceTransparency = false;
    std::wstring panelStatus;       ///< "Installed v1.0.0" / "Not installed"
    bool panelInstalled = false;
    std::wstring aboutLine;         ///< "HDR Hint 1.0.0 · mkvmerge v82.0"
};

class ISettingsViewModel {
public:
    virtual ~ISettingsViewModel() = default;
    [[nodiscard]] virtual SettingsView view() const = 0;
    [[nodiscard]] virtual std::vector<Choice> presetChoices(TransferKind t) const = 0;
    [[nodiscard]] virtual std::vector<Choice> lutChoices() const = 0;
    virtual void setMkvmergePath(const std::wstring& path) = 0;
    virtual void browseMkvmerge() = 0;
    virtual void setLutFolder(const std::wstring& path) = 0;
    virtual void browseLutFolder() = 0;
    virtual void setDefaultLut(TransferKind t, const std::wstring& path) = 0;
    /// Opens a .cube picker and stores the result as the default for t.
    virtual void browseLut(TransferKind t) = 0;
    virtual void setDefaultPreset(TransferKind t, const std::wstring& presetId) = 0;
    virtual void setSuffix(const std::wstring& suffix) = 0;
    virtual void setAttachLutByDefault(bool on) = 0;
    virtual void addWatchFolder() = 0;          ///< opens a folder picker
    virtual void removeWatchFolder(const std::wstring& folder) = 0;
    virtual void setAutoProcess(bool on) = 0;
    virtual void setAutoProcessAme(bool on) = 0;
    virtual void setAutoProcessWatched(bool on) = 0;
    virtual void setRecycleOriginal(bool on) = 0;
    virtual void setDockInsideAme(bool on) = 0;
    virtual void setAlwaysOnTop(bool on) = 0;
    virtual void setMinimizeToTray(bool on) = 0;
    virtual void setStartMinimized(bool on) = 0;
    virtual void setStartWithWindows(bool on) = 0;
    virtual void setQuitWithAme(bool on) = 0;
    virtual void setShowOnAmeLaunch(bool on) = 0;
    virtual void setAppearance(int mode) = 0;
    virtual void setAccent(int mode) = 0;
    virtual void setReduceTransparency(bool on) = 0;
    virtual void installPanel() = 0;
    virtual void openLogFolder() = 0;
    virtual void reprobeMkvmerge() = 0;
    std::function<void()> onChanged;
};

/// AME link state for the top bar.
struct LinkView {
    bool panelLinked = false;
    bool logFound = false;
    bool queueRunning = false;
    bool docked = false;
    bool dockingSupported = true;   ///< false on macOS: the dock toggles hide themselves
    std::wstring tooltip;
};

class ILinkViewModel {
public:
    virtual ~ILinkViewModel() = default;
    [[nodiscard]] virtual LinkView link() const = 0;
    virtual void toggleDock() = 0;
    std::function<void()> onChanged;
};

class IGuideViewModel {
public:
    virtual ~IGuideViewModel() = default;
    /// The guide markdown (from the embedded resource).
    [[nodiscard]] virtual std::wstring markdown() const = 0;
    /// Short settings summary for the clipboard.
    [[nodiscard]] virtual std::wstring summaryText() const = 0;
    virtual void copySummary() = 0;
    virtual void openGuideFile() = 0;
};

} // namespace hh::ui
