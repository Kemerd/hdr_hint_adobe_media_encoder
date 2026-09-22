// ---------------------------------------------------------------------------
// AppViewModels.h - the real view-models: screens <-> Engine / Settings.
//
// Each class adapts one screen interface from ui/screens/ViewModels.h to the
// engine. They own no state of their own beyond callbacks; every query reads
// the engine fresh so the screens can re-render from scratch on onChanged.
//
// Threading: everything here runs on the UI thread. The engine fires its
// callbacks on the UI thread as well (see Engine.h), so no locking is needed.
// ---------------------------------------------------------------------------
#pragma once

#include "ame/DockController.h"
#include "core/Engine.h"
#include "core/JobModel.h"
#include "core/Settings.h"
#include "platform/Win.h"
#include "ui/screens/ViewModels.h"
#include "ui/theme/ThemeManager.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hh::ui {

/**
 * @brief Puts text on the clipboard as CF_UNICODETEXT.
 * @param owner  window that becomes the clipboard owner (may be null)
 * @param text   the text; an empty string clears the clipboard
 * @return true when the clipboard now holds @p text
 */
bool copyTextToClipboard(HWND owner, std::wstring_view text);

/**
 * @brief "1.0.0" read from the executable's VERSIONINFO block (falls back to
 *        the compiled-in version when the block is missing).
 */
std::wstring appVersionString();

// ===========================================================================
// Queue
// ===========================================================================

/**
 * @brief Queue tab over the engine's job table.
 */
class AppQueueViewModel final : public IQueueViewModel {
public:
    /**
     * @param engine           the engine (must outlive this object)
     * @param ownerForDialogs  HWND used as the clipboard owner
     */
    AppQueueViewModel(hh::Engine& engine, HWND ownerForDialogs);
    ~AppQueueViewModel() override;
    AppQueueViewModel(const AppQueueViewModel&) = delete;
    AppQueueViewModel& operator=(const AppQueueViewModel&) = delete;

    /// Hooks Engine::onJobsChanged (chaining any callback already there) so onChanged fires.
    void bind();

    [[nodiscard]] std::vector<JobView> jobs() const override;
    [[nodiscard]] std::vector<Choice> presetChoices(TransferKind t) const override;
    [[nodiscard]] std::vector<Choice> lutChoices() const override;
    void run(JobId id) override;
    void hold(JobId id) override;
    void resume(JobId id) override;
    void remove(JobId id) override;
    void reveal(JobId id, bool hintFile) override;
    void setPreset(JobId id, const std::wstring& presetId) override;
    void setLut(JobId id, const std::wstring& lutPath) override;
    void browseLut(JobId id) override;
    void setAttachLut(JobId id, bool attach) override;
    void copyCommand(JobId id) override;
    [[nodiscard]] bool autoProcess() const override;
    void setAutoProcess(bool on) override;
    [[nodiscard]] std::wstring footerSummary() const override;
    void addFiles(const std::vector<std::wstring>& paths) override;

    /// Maps one engine job to its row (public so tests can check the labels).
    [[nodiscard]] JobView makeView(const hh::Job& job) const;

    /// Something to tell the user (toast): message + success flag ("Command copied").
    std::function<void(const std::wstring& message, bool ok)> onNotify;

private:
    /// The mkvmerge command line for a job: the recorded one, else a preview.
    [[nodiscard]] std::wstring commandFor(const hh::Job& job, const hh::EffectivePlan& plan) const;
    /// Fires onChanged when subscribed.
    void notify();
    /// Fires onNotify when subscribed (and logs either way).
    void notifyUser(const std::wstring& message, bool ok);

    hh::Engine& engine_;
    HWND owner_ = nullptr;
    /// Shared liveness token: the engine callback checks it before touching this.
    std::shared_ptr<bool> alive_;
    /// bind() ran already (a second call would chain us twice).
    bool bound_ = false;
};

// ===========================================================================
// Settings
// ===========================================================================

/**
 * @brief Settings tab over Settings + Engine + ThemeManager.
 */
class AppSettingsViewModel final : public ISettingsViewModel {
public:
    /**
     * @param engine    the engine (re-applies settings to its workers)
     * @param settings  the live settings object the engine reads
     * @param themes    theme manager for appearance / accent / transparency
     * @param owner     HWND that owns the file dialogs
     */
    AppSettingsViewModel(hh::Engine& engine, hh::Settings& settings, ThemeManager& themes, HWND owner);
    ~AppSettingsViewModel() override = default;
    AppSettingsViewModel(const AppSettingsViewModel&) = delete;
    AppSettingsViewModel& operator=(const AppSettingsViewModel&) = delete;

    [[nodiscard]] SettingsView view() const override;
    [[nodiscard]] std::vector<Choice> presetChoices(TransferKind t) const override;
    [[nodiscard]] std::vector<Choice> lutChoices() const override;
    void setMkvmergePath(const std::wstring& path) override;
    void browseMkvmerge() override;
    void setLutFolder(const std::wstring& path) override;
    void browseLutFolder() override;
    void setDefaultLut(TransferKind t, const std::wstring& path) override;
    void browseLut(TransferKind t) override;
    void setDefaultPreset(TransferKind t, const std::wstring& presetId) override;
    void setSuffix(const std::wstring& suffix) override;
    void setAttachLutByDefault(bool on) override;
    void addWatchFolder() override;
    void removeWatchFolder(const std::wstring& folder) override;
    void setAutoProcess(bool on) override;
    void setAutoProcessAme(bool on) override;
    void setAutoProcessWatched(bool on) override;
    void setRecycleOriginal(bool on) override;
    void setDockInsideAme(bool on) override;
    void setAlwaysOnTop(bool on) override;
    void setMinimizeToTray(bool on) override;
    void setStartMinimized(bool on) override;
    void setStartWithWindows(bool on) override;
    void setQuitWithAme(bool on) override;
    void setShowOnAmeLaunch(bool on) override;
    void setAppearance(int mode) override;
    void setAccent(int mode) override;
    void setReduceTransparency(bool on) override;
    void installPanel() override;
    void openLogFolder() override;
    void reprobeMkvmerge() override;

    /// Theme / accent / transparency changed: the window re-applies its backdrop.
    std::function<void()> onAppearanceChanged;
    /// "Dock inside Media Encoder" toggled: the app enables/disables the DockController.
    std::function<void()> onDockChanged;
    /// Always-on-top / tray / start-minimized / quit-with-AME changed.
    std::function<void()> onWindowBehaviourChanged;
    /// Something to tell the user (toast): message + success flag.
    std::function<void(const std::wstring& message, bool ok)> onNotify;

private:
    /// Settings that the workers care about: engine re-apply (which also saves) + onChanged.
    void commit();
    /// UI-only settings: save the INI + onChanged (no worker round trip).
    void persist();
    /// Fires onChanged when subscribed.
    void notify();
    /// Fires onNotify when subscribed (and logs either way).
    void notifyUser(const std::wstring& message, bool ok);
    /// Shows IFileOpenDialog; empty when cancelled or unavailable.
    [[nodiscard]] std::wstring pickPath(bool pickFolder, const wchar_t* title, const std::wstring& startFolder,
                                        const wchar_t* filterLabel, const wchar_t* filterPattern) const;

    hh::Engine& engine_;
    hh::Settings& settings_;
    ThemeManager& themes_;
    HWND owner_ = nullptr;
};

// ===========================================================================
// Link (top-bar status dot)
// ===========================================================================

/**
 * @brief AME link state for the status dot + dock toggle.
 */
class AppLinkViewModel final : public ILinkViewModel {
public:
    AppLinkViewModel(hh::Engine& engine, hh::ame::DockController& dock);
    ~AppLinkViewModel() override;
    AppLinkViewModel(const AppLinkViewModel&) = delete;
    AppLinkViewModel& operator=(const AppLinkViewModel&) = delete;

    /// Hooks Engine::onLinkStateChanged and DockController::onStateChanged (chaining existing callbacks).
    void bind();

    [[nodiscard]] LinkView link() const override;
    void toggleDock() override;

private:
    void notify();

    hh::Engine& engine_;
    hh::ame::DockController& dock_;
    std::shared_ptr<bool> alive_;
    /// bind() ran already (a second call would chain us twice).
    bool bound_ = false;
};

// ===========================================================================
// Guide
// ===========================================================================

/**
 * @brief Guide tab: embedded markdown + clipboard summary + "open the file".
 */
class AppGuideViewModel final : public IGuideViewModel {
public:
    /// @param owner HWND used as the clipboard owner
    explicit AppGuideViewModel(HWND owner);
    ~AppGuideViewModel() override = default;

    [[nodiscard]] std::wstring markdown() const override;
    [[nodiscard]] std::wstring summaryText() const override;
    void copySummary() override;
    void openGuideFile() override;

    /// Optional: the summary then reflects the live settings instead of the defaults.
    void setSettingsSource(const ISettingsViewModel* settings) noexcept { settings_ = settings; }

    /// Something to tell the user (toast): message + success flag.
    std::function<void(const std::wstring& message, bool ok)> onNotify;

private:
    void notifyUser(const std::wstring& message, bool ok);

    HWND owner_ = nullptr;
    const ISettingsViewModel* settings_ = nullptr;
    /// The markdown is immutable for the life of the process; decode it once.
    mutable std::wstring markdownCache_;
};

} // namespace hh::ui
