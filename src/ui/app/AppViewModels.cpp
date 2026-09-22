// ---------------------------------------------------------------------------
// AppViewModels.cpp - screens <-> Engine / Settings / ThemeManager.
// ---------------------------------------------------------------------------
#include "ui/app/AppViewModels.h"

#include "ame/PanelInstaller.h"
#include "core/HdrPresets.h"
#include "core/Logger.h"
#include "core/MkvmergeRunner.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/RecycleBin.h"
#include "platform/Terms.h"
#include "platform/Utf.h"
#include "ui/app/GuideResource.h"
#include "ui/screens/GuideContent.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"ViewModel";

/// Product name used in the About line.
constexpr const wchar_t* kProductName = L"HDR Hint";

/// Separator used between subtitle / summary parts everywhere in the UI.
constexpr const wchar_t* kDot = L" · ";

/**
 * @brief 0..1 -> whole percent, clamped.
 */
int percentOf(float progress) noexcept {
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clamped * 100.0f));
}

/**
 * @brief True when the text already ends in a percentage ("Encoding 42%").
 */
bool endsWithPercent(std::wstring_view text) noexcept {
    const std::wstring_view trimmed = platform::trim(text);
    return !trimmed.empty() && trimmed.back() == L'%';
}

/**
 * @brief The chip text for a job.
 * @param job            the engine job
 * @param shownProgress  the row's progress (-1 = none)
 */
std::wstring stateLabelFor(const Job& job, float shownProgress) {
    switch (job.state) {
    case JobState::Discovered:
        return L"Detected";
    case JobState::Encoding: {
        // The engine's phase is more specific ("Finalizing", "Encoding in AME");
        // fall back to a plain word when it is empty.
        std::wstring label(platform::trim(job.phase));
        if (label.empty()) {
            label = L"Encoding";
        }
        // Append the percentage unless the phase text already carries one.
        if (shownProgress >= 0.0f && !endsWithPercent(label)) {
            label += std::format(L" {}%", percentOf(shownProgress));
        }
        return label;
    }
    case JobState::Ready:
        return L"Ready";
    case JobState::Held:
        return L"Held";
    case JobState::Muxing:
        return std::format(L"Muxing {}%", percentOf(shownProgress < 0.0f ? 0.0f : shownProgress));
    case JobState::Verifying:
        return L"Verifying";
    case JobState::Done:
        return L"Done";
    case JobState::Failed:
        return L"Failed";
    case JobState::SkippedSdr:
        return L"SDR - skipped";
    case JobState::Cancelled:
        return L"Cancelled";
    }
    // Unknown enum value (corrupt store): show the raw name rather than nothing.
    return toString(job.state);
}

/**
 * @brief Run / Retry / Run anyway / Run again depending on where the job ended.
 */
const wchar_t* runLabelFor(JobState state) noexcept {
    switch (state) {
    case JobState::Failed:     return L"Retry";
    case JobState::SkippedSdr: return L"Run anyway";
    case JobState::Done:       return L"Run again";
    default:                   return L"Run";
    }
}

/**
 * @brief States from which the user may (re)start the mux.
 */
bool canRunFrom(JobState state) noexcept {
    switch (state) {
    case JobState::Held:
    case JobState::Ready:
    case JobState::Failed:
    case JobState::SkippedSdr:
    case JobState::Cancelled:
    case JobState::Done:
        return true;
    default:
        return false;
    }
}

/**
 * @brief States in which the preset / LUT popups are still meaningful.
 */
bool canEditPlanIn(JobState state) noexcept {
    switch (state) {
    case JobState::Discovered:
    case JobState::Encoding:
    case JobState::Ready:
    case JobState::Held:
    case JobState::Failed:
    case JobState::SkippedSdr:
    case JobState::Cancelled:
        return true;
    default:
        return false;
    }
}

/**
 * @brief mkvmerge's last error line for the card subtitle.
 *
 * Prefers the recorded error text; for a failed job without one, scans the
 * output tail backwards for a line mentioning "error", else the last line.
 */
std::wstring lastErrorFor(const Job& job) {
    const std::wstring_view recorded = platform::trim(job.mux.errorText);
    if (!recorded.empty()) {
        return std::wstring(recorded);
    }
    if (job.state != JobState::Failed) {
        return {};
    }
    // Walk the tail backwards: the error is usually the last thing printed.
    std::wstring lastNonEmpty;
    for (auto it = job.mux.lastLines.rbegin(); it != job.mux.lastLines.rend(); ++it) {
        const std::wstring_view line = platform::trim(*it);
        if (line.empty()) {
            continue;
        }
        if (platform::icontains(line, L"error")) {
            return std::wstring(line);
        }
        if (lastNonEmpty.empty()) {
            lastNonEmpty = std::wstring(line);
        }
    }
    // No "error" line: the state reason (engine text) beats a random tail line.
    if (!platform::trim(job.stateReason).empty()) {
        return job.stateReason;
    }
    return lastNonEmpty;
}

/**
 * @brief Human text for the number of jobs: "1 job", "3 jobs".
 */
std::wstring jobCountText(size_t count) {
    return std::format(L"{} {}", count, count == 1 ? L"job" : L"jobs");
}

/**
 * @brief Maps the settings' theme word to the segmented index (0 system, 1 dark, 2 light).
 */
int appearanceIndexFor(std::wstring_view theme) noexcept {
    if (platform::iequals(theme, L"dark")) {
        return 1;
    }
    if (platform::iequals(theme, L"light")) {
        return 2;
    }
    return 0;
}

/**
 * @brief Maps the settings' accent word to the segmented index (0 blue, 1 system).
 */
int accentIndexFor(std::wstring_view accent) noexcept {
    return platform::iequals(accent, L"system") ? 1 : 0;
}

/**
 * @brief Mode index -> ThemeMode (out-of-range = System).
 */
ThemeMode themeModeFor(int index) noexcept {
    switch (index) {
    case 1:  return ThemeMode::Dark;
    case 2:  return ThemeMode::Light;
    default: return ThemeMode::System;
    }
}

} // namespace

// The .cube picker lives further down, next to the settings view model that
// uses it most; both view models need it, so declare it here.
static std::wstring pickCubeFile(NativeWindowHandle owner, const std::wstring& startFolder);

// ===========================================================================
// AppQueueViewModel
// ===========================================================================

AppQueueViewModel::AppQueueViewModel(hh::Engine& engine, NativeWindowHandle ownerForDialogs)
    : engine_(engine), owner_(ownerForDialogs), alive_(std::make_shared<bool>(true)) {}

/**
 * @brief Flags the liveness token so a still-registered engine callback
 *        never calls into a destroyed object.
 */
AppQueueViewModel::~AppQueueViewModel() {
    if (alive_) {
        *alive_ = false;
    }
}

void AppQueueViewModel::bind() {
    // Binding twice would chain this object behind itself and fire onChanged twice.
    if (bound_) {
        HH_LOG_DEBUG(kLog, L"AppQueueViewModel::bind called twice; ignored");
        return;
    }
    bound_ = true;
    // Keep whatever the app already wired (e.g. the tray badge) and add ourselves.
    std::function<void()> previous = std::move(engine_.onJobsChanged);
    std::weak_ptr<bool> alive = alive_;
    engine_.onJobsChanged = [this, previous, alive]() {
        if (previous) {
            previous();
        }
        // The token is reset by the destructor; skip when we are gone.
        const std::shared_ptr<bool> token = alive.lock();
        if (!token || !*token) {
            return;
        }
        notify();
    };
}

void AppQueueViewModel::notify() {
    if (onChanged) {
        onChanged();
    }
}

void AppQueueViewModel::notifyUser(const std::wstring& message, bool ok) {
    if (ok) {
        HH_LOG_INFO(kLog, L"{}", message);
    } else {
        HH_LOG_WARN(kLog, L"{}", message);
    }
    if (onNotify) {
        onNotify(message, ok);
    }
}

// ---- queries -------------------------------------------------------------

std::vector<JobView> AppQueueViewModel::jobs() const {
    const std::vector<Job> all = engine_.jobs();
    std::vector<JobView> out;
    out.reserve(all.size());
    for (const Job& job : all) {
        out.push_back(makeView(job));
    }
    return out;
}

JobView AppQueueViewModel::makeView(const Job& job) const {
    JobView v;
    v.id = job.id;
    v.generation = job.generation;
    v.name = job.displayName();
    v.state = job.state;
    v.stateReason = job.stateReason;
    v.transfer = job.transfer;
    v.outputPath = job.outputPath;

    // The effective plan gives the preset / LUT / hint the mux would use right now.
    const EffectivePlan plan = engine_.resolvePlan(job);
    v.hintPath = !job.hintPath.empty() ? job.hintPath : plan.hintPath;
    v.presetId = plan.presetId;
    v.lutPath = plan.lutPath;
    v.attachLut = plan.attachLut;
    v.held = job.overrides.hold || job.state == JobState::Held;

    // Progress: Encoding only knows a percentage once something reported one
    // (0 means "not yet"), Muxing always has one, everything else is static.
    switch (job.state) {
    case JobState::Encoding:
        v.progress = job.progress > 0.0f ? std::clamp(job.progress, 0.0f, 1.0f) : -1.0f;
        v.showProgress = v.progress >= 0.0f;
        break;
    case JobState::Muxing:
        v.progress = std::clamp(job.progress, 0.0f, 1.0f);
        v.showProgress = true;
        break;
    default:
        v.progress = -1.0f;
        v.showProgress = false;
        break;
    }
    v.stateLabel = stateLabelFor(job, v.progress);

    // Subtitle: what the export is, plus where the result lands.
    std::vector<std::wstring> parts;
    const std::wstring described = job.video.describe();
    if (!platform::trim(described).empty()) {
        parts.push_back(described);
    }
    const std::wstring hintName = path::fileName(v.hintPath);
    if (!hintName.empty()) {
        parts.push_back(L"out: " + hintName);
    }
    v.subtitle = platform::join(parts, kDot);

    // Which buttons the card offers.
    v.canRun = canRunFrom(job.state);
    v.runLabel = runLabelFor(job.state);
    v.canHold = (job.state == JobState::Ready || job.state == JobState::Encoding) && !v.held;
    v.canEditPlan = canEditPlanIn(job.state);
    v.canRemove = true;
    // Reveal needs a file on disk; a planned hint path does not count yet.
    v.canRevealHint = !v.hintPath.empty() && platform::exists(v.hintPath);
    v.canReveal = v.canRevealHint || (!v.outputPath.empty() && platform::exists(v.outputPath));

    v.mkvmergeCommand = commandFor(job, plan);
    v.lastError = lastErrorFor(job);
    return v;
}

std::wstring AppQueueViewModel::commandFor(const Job& job, const EffectivePlan& plan) const {
    // A mux that already ran recorded the exact line; that is the truth.
    if (!platform::trim(job.mux.commandLine).empty()) {
        return job.mux.commandLine;
    }
    // Otherwise preview what dispatch would launch. Without an input or a hint
    // path there is nothing sensible to show.
    if (job.outputPath.empty() || plan.hintPath.empty()) {
        return {};
    }
    const MkvmergeInfo& info = engine_.mkvmergeInfo();
    const Settings& settings = engine_.settings();

    MuxPlan m;
    // Not found yet: show the bare name so the preview is still readable.
    m.mkvmergePath = info.path.empty() ? std::wstring(platform::terms::kMkvmergeExe) : info.path;
    m.supportsUiLanguage = info.supportsUiLanguage;
    m.inputPath = job.outputPath;
    m.hintPath = plan.hintPath;
    m.partialPath = path::partialPathFor(plan.hintPath);
    if (const HdrPreset* preset = engine_.presets().find(plan.presetId)) {
        m.preset = *preset;
    } else {
        // Unknown preset: keep the id for the log, emit no colour flags.
        m.preset = HdrPreset{};
        m.preset.id = plan.presetId;
    }
    m.lutPath = plan.attachLut ? plan.lutPath : std::wstring();
    m.attachLut = plan.attachLut;
    m.attachmentMime = settings.attachmentMime.empty() ? std::wstring(L"application/x-cube") : settings.attachmentMime;
    m.trackId = 0;   // the worker substitutes the identified video track at dispatch
    m.lowerPriority = settings.lowerPriority;
    m.failOnWarnings = settings.failOnWarnings;
    m.setTitle = settings.setTitle;
    m.title = path::stem(job.outputPath);
    m.onConflict = settings.onConflict.empty() ? std::wstring(L"increment") : settings.onConflict;
    m.keepPartialOnFailure = settings.keepPartialOnFailure;
    return MkvmergeRunner::commandLine(m);
}

std::vector<Choice> AppQueueViewModel::presetChoices(TransferKind t) const {
    std::vector<Choice> out;
    for (const HdrPreset& preset : engine_.presets().forTransfer(t)) {
        if (preset.id.empty()) {
            continue;
        }
        Choice c;
        c.label = preset.label.empty() ? preset.id : preset.label;
        c.value = preset.id;
        // User presets get a hint so they are distinguishable from the built-ins.
        c.subtitle = preset.builtIn ? std::wstring() : std::wstring(L"User preset");
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Choice> AppQueueViewModel::lutChoices() const {
    std::vector<Choice> out;
    // "None" is always first with an empty value (the screens rely on it).
    out.push_back(Choice{L"None", std::wstring(), std::wstring()});
    for (const std::wstring& lut : engine_.availableLuts()) {
        if (platform::trim(lut).empty()) {
            continue;
        }
        Choice c;
        c.label = path::fileName(lut);
        c.value = lut;
        c.subtitle = path::parent(lut);
        out.push_back(std::move(c));
    }
    // Last entry: reach any .cube on disk, not just the folder and recents.
    out.push_back(Choice{L"Choose a .cube file...", kBrowseLutValue, std::wstring()});
    return out;
}

bool AppQueueViewModel::autoProcess() const {
    return engine_.settings().autoProcess;
}

std::wstring AppQueueViewModel::footerSummary() const {
    const std::vector<Job> all = engine_.jobs();
    if (all.empty()) {
        return L"No jobs";
    }
    size_t done = 0;
    for (const Job& job : all) {
        if (job.state == JobState::Done) {
            ++done;
        }
    }
    return std::format(L"{}{}{} done", jobCountText(all.size()), kDot, done);
}

// ---- commands ------------------------------------------------------------

void AppQueueViewModel::run(JobId id) {
    engine_.runJob(id);
}

void AppQueueViewModel::hold(JobId id) {
    engine_.holdJob(id);
}

void AppQueueViewModel::resume(JobId id) {
    engine_.resumeJob(id);
}

void AppQueueViewModel::remove(JobId id) {
    engine_.removeJob(id);
}

void AppQueueViewModel::reveal(JobId id, bool hintFile) {
    engine_.revealJob(id, hintFile);
}

void AppQueueViewModel::setPreset(JobId id, const std::wstring& presetId) {
    const std::optional<Job> job = engine_.job(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"setPreset: unknown job {}", id);
        return;
    }
    // Change exactly one field of the overrides; empty = back to the default.
    JobOverrides o = job->overrides;
    if (platform::trim(presetId).empty()) {
        o.presetId.reset();
    } else {
        o.presetId = presetId;
    }
    // A no-op write would still re-evaluate a Held job; skip it.
    if (o.presetId == job->overrides.presetId) {
        return;
    }
    engine_.setJobOverrides(id, o);
}

void AppQueueViewModel::setLut(JobId id, const std::wstring& lutPath) {
    const std::optional<Job> job = engine_.job(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"setLut: unknown job {}", id);
        return;
    }
    // An explicit empty string means "no LUT" (distinct from "use the default").
    JobOverrides o = job->overrides;
    o.lutPath = lutPath;
    if (o.lutPath == job->overrides.lutPath) {
        return;
    }
    engine_.setJobOverrides(id, o);
}

/**
 * @brief Picks a .cube from disk and applies it to one job.
 *
 * Starts in the folder of whatever the job already uses, so replacing a LUT
 * with its neighbour is two clicks. The engine remembers the file, which puts
 * it in every chooser from now on.
 */
void AppQueueViewModel::browseLut(JobId id) {
    const std::optional<Job> job = engine_.job(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"browseLut: unknown job {}", id);
        return;
    }
    // Start next to the LUT this job currently resolves to, else the folder.
    std::wstring start;
    if (job->overrides.lutPath && !job->overrides.lutPath->empty()) {
        start = path::parent(engine_.settings().expand(*job->overrides.lutPath));
    }
    if (start.empty() || !platform::isDirectory(start)) {
        start = engine_.settings().expand(engine_.settings().lutFolder);
    }
    const std::wstring picked = pickCubeFile(owner_, start);
    if (picked.empty()) {
        // Cancelled: re-render so the card's pop-up leaves the "Choose..."
        // entry and shows the LUT the job actually uses.
        notify();
        return;
    }
    HH_LOG_INFO(kLog, L"job {} LUT picked from disk: '{}'", id, picked);
    // Remembering it first means the pop-up already lists the file when the
    // override lands and the card refreshes.
    engine_.rememberLut(picked);
    setLut(id, picked);
}

void AppQueueViewModel::setAttachLut(JobId id, bool attach) {
    const std::optional<Job> job = engine_.job(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"setAttachLut: unknown job {}", id);
        return;
    }
    JobOverrides o = job->overrides;
    o.attachLut = attach;
    if (o.attachLut == job->overrides.attachLut) {
        return;
    }
    engine_.setJobOverrides(id, o);
}

void AppQueueViewModel::copyCommand(JobId id) {
    const std::optional<Job> job = engine_.job(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"copyCommand: unknown job {}", id);
        return;
    }
    const std::wstring command = commandFor(*job, engine_.resolvePlan(*job));
    if (command.empty()) {
        notifyUser(L"No mkvmerge command for this job yet", false);
        return;
    }
    const bool ok = copyTextToClipboard(owner_, command);
    notifyUser(ok ? L"mkvmerge command copied" : L"Could not copy to the clipboard", ok);
}

void AppQueueViewModel::setAutoProcess(bool on) {
    // The engine persists the flag and releases "Waiting for you" jobs itself.
    engine_.setAutoProcess(on);
}

void AppQueueViewModel::addFiles(const std::vector<std::wstring>& paths) {
    if (paths.empty()) {
        return;
    }
    for (const std::wstring& p : paths) {
        if (platform::trim(p).empty()) {
            continue;
        }
        engine_.addManualFile(p);
    }
}

// ===========================================================================
// AppSettingsViewModel
// ===========================================================================

AppSettingsViewModel::AppSettingsViewModel(hh::Engine& engine, hh::Settings& settings, ThemeManager& themes, NativeWindowHandle owner)
    : engine_(engine), settings_(settings), themes_(themes), owner_(owner) {}

void AppSettingsViewModel::notify() {
    if (onChanged) {
        onChanged();
    }
}

void AppSettingsViewModel::notifyUser(const std::wstring& message, bool ok) {
    if (ok) {
        HH_LOG_INFO(kLog, L"{}", message);
    } else {
        HH_LOG_WARN(kLog, L"{}", message);
    }
    if (onNotify) {
        onNotify(message, ok);
    }
}

void AppSettingsViewModel::commit() {
    // The engine re-applies worker settings, refreshes LUTs and saves the INI.
    engine_.settingsChanged();
    notify();
}

void AppSettingsViewModel::persist() {
    // UI-only settings: nothing for the workers, just write the file.
    if (auto r = settings_.save(Settings::defaultPath()); !r) {
        HH_LOG_WARN(kLog, L"settings save failed: {}", r.error().toString());
    }
    notify();
}

// ---- queries -------------------------------------------------------------

SettingsView AppSettingsViewModel::view() const {
    SettingsView v;
    const MkvmergeInfo& info = engine_.mkvmergeInfo();

    // Tools.
    v.mkvmergePath = settings_.mkvmergePath;
    v.mkvmergeOk = info.ok;
    if (!info.found) {
        v.mkvmergeStatus = L"not found - install MKVToolNix";
    } else if (!info.ok) {
        // Found but unusable (too old): say why, next to the version.
        const std::wstring why = platform::trim(info.error).empty() ? std::wstring(L"unsupported version")
                                                                    : info.error;
        v.mkvmergeStatus = info.shortVersion() + kDot + why;
    } else {
        v.mkvmergeStatus = info.shortVersion() + kDot + path::parent(info.path);
    }
    v.lutFolder = settings_.lutFolder;

    // Defaults (expanded so the popups can match them against real paths).
    v.defaultLutPq = settings_.defaultLutFor(static_cast<int>(TransferKind::PQ));
    v.defaultLutHlg = settings_.defaultLutFor(static_cast<int>(TransferKind::HLG));
    v.presetPq = settings_.defaultPresetFor(static_cast<int>(TransferKind::PQ));
    v.presetHlg = settings_.defaultPresetFor(static_cast<int>(TransferKind::HLG));
    v.suffix = settings_.suffix;
    v.attachLutByDefault = settings_.attachLut;

    // Watch folders the user added (learned folders are not editable here).
    v.watchFolders.reserve(settings_.extraWatchFolders.size());
    for (const std::wstring& raw : settings_.extraWatchFolders) {
        const std::wstring expanded = settings_.expand(raw);
        if (!platform::trim(expanded).empty()) {
            v.watchFolders.push_back(expanded);
        }
    }

    // Behaviour.
    v.autoProcess = settings_.autoProcess;
    v.autoProcessAme = settings_.autoProcessAme;
    v.autoProcessWatched = settings_.autoProcessWatched;
    v.recycleOriginal = !platform::iequals(settings_.recycleMode, L"never");
    v.dockInsideAme = !platform::iequals(settings_.dockMode, L"floating");
    v.alwaysOnTop = settings_.floatingAlwaysOnTop;
    v.minimizeToTray = settings_.minimizeToTray;
    v.startMinimized = settings_.startMinimized;
    v.startWithWindows = settings_.startWithWindows;
    v.quitWithAme = settings_.quitWithAme;
    v.showOnAmeLaunch = settings_.showOnAmeLaunch;
    v.appearance = appearanceIndexFor(settings_.theme);
    v.accent = accentIndexFor(settings_.accent);
    v.reduceTransparency = settings_.reduceTransparency;

    // Media Encoder panel.
    const ame::PanelStatus panel = ame::queryPanelStatus();
    v.panelInstalled = panel.installed;
    if (panel.installed) {
        v.panelStatus = L"Installed";
        if (!panel.installedVersion.empty()) {
            v.panelStatus += L" v" + panel.installedVersion;
        }
        if (panel.updateAvailable()) {
            v.panelStatus += kDot + std::wstring(L"update to v") + panel.bundledVersion + L" available";
        }
        if (!panel.debugModeOk) {
            v.panelStatus += kDot + std::wstring(L"PlayerDebugMode off");
        }
    } else {
        v.panelStatus = L"Not installed";
    }

    // About.
    v.aboutLine = std::wstring(kProductName) + L" " + appVersionString() + kDot + L"mkvmerge " + info.shortVersion();
    return v;
}

std::vector<Choice> AppSettingsViewModel::presetChoices(TransferKind t) const {
    std::vector<Choice> out;
    for (const HdrPreset& preset : engine_.presets().forTransfer(t)) {
        if (preset.id.empty()) {
            continue;
        }
        Choice c;
        c.label = preset.label.empty() ? preset.id : preset.label;
        c.value = preset.id;
        c.subtitle = preset.builtIn ? std::wstring() : std::wstring(L"User preset");
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Choice> AppSettingsViewModel::lutChoices() const {
    std::vector<Choice> out;
    out.push_back(Choice{L"None", std::wstring(), std::wstring()});
    for (const std::wstring& lut : engine_.availableLuts()) {
        if (platform::trim(lut).empty()) {
            continue;
        }
        Choice c;
        c.label = path::fileName(lut);
        c.value = lut;
        c.subtitle = path::parent(lut);
        out.push_back(std::move(c));
    }
    // Last entry: reach any .cube on disk, not just the folder and recents.
    out.push_back(Choice{L"Choose a .cube file...", kBrowseLutValue, std::wstring()});
    return out;
}

// ---- file dialogs --------------------------------------------------------

/**
 * @brief Picks a .cube file, starting wherever the current value points.
 *
 * The same dialog for the settings defaults and for a single job's LUT
 * override, so a LUT that lives nowhere near the configured LUT folder is
 * always one click away.
 */
static std::wstring pickCubeFile(NativeWindowHandle owner, const std::wstring& startFolder) {
    return showPathPicker(owner, false, L"Choose a LUT (.cube)", startFolder, L"Cube LUT (*.cube)", L"*.cube");
}

std::wstring AppSettingsViewModel::pickPath(bool pickFolder, const wchar_t* title, const std::wstring& startFolder,
                                            const wchar_t* filterLabel, const wchar_t* filterPattern) const {
    return showPathPicker(owner_, pickFolder, title, startFolder, filterLabel, filterPattern);
}

void AppSettingsViewModel::browseMkvmerge() {
    // Start next to the configured binary, else in the usual install folder.
    std::wstring start;
    const std::wstring configured = settings_.expand(settings_.mkvmergePath);
    if (!configured.empty()) {
        start = path::parent(configured);
    }
#if defined(_WIN32)
    if (start.empty() || !platform::isDirectory(start)) {
        start = path::join(platform::programFilesX64Folder(), L"MKVToolNix");
    }
    const std::wstring picked = pickPath(false, L"Locate mkvmerge.exe", start, L"mkvmerge (mkvmerge.exe)", L"mkvmerge.exe");
    if (picked.empty()) {
        return;
    }
    // Any other executable would only produce confusing errors later.
    if (!platform::iequals(path::fileName(picked), L"mkvmerge.exe")) {
        notifyUser(L"That is not mkvmerge.exe: " + path::fileName(picked), false);
        return;
    }
    setMkvmergePath(picked);
#else
    // macOS: the MKVToolNix app bundle or the bare binary (Homebrew) both work.
    if (start.empty() || !platform::isDirectory(start)) {
        start = platform::programFilesX64Folder();
    }
    std::wstring picked = pickPath(false, L"Locate mkvmerge or the MKVToolNix app", start, L"mkvmerge", L"mkvmerge");
    if (picked.empty()) {
        return;
    }
    if (platform::iendsWith(picked, L".app")) {
        picked = path::join(path::join(picked, L"Contents/MacOS"), L"mkvmerge");
    }
    // Any other executable would only produce confusing errors later.
    if (!platform::iequals(path::fileName(picked), L"mkvmerge") || !platform::isFile(picked)) {
        notifyUser(L"That is not mkvmerge: " + path::fileName(picked), false);
        return;
    }
    setMkvmergePath(picked);
#endif
}

void AppSettingsViewModel::browseLutFolder() {
    std::wstring start = settings_.expand(settings_.lutFolder);
    if (start.empty() || !platform::isDirectory(start)) {
        start = platform::resourceDirectory();
    }
    const std::wstring picked = pickPath(true, L"Choose the LUT folder", start, nullptr, nullptr);
    if (picked.empty()) {
        return;
    }
    setLutFolder(picked);
}

void AppSettingsViewModel::addWatchFolder() {
    const std::wstring picked = pickPath(true, L"Choose a folder to watch", platform::documentsFolder(), nullptr, nullptr);
    if (picked.empty()) {
        return;
    }
    // The engine validates, de-duplicates, saves and starts watching.
    engine_.addWatchFolder(picked);
    notify();
}

// ---- setters: tools / defaults -----------------------------------------

void AppSettingsViewModel::setMkvmergePath(const std::wstring& path) {
    const std::wstring trimmed(platform::trim(path));
    if (trimmed == settings_.mkvmergePath) {
        return;
    }
    settings_.mkvmergePath = trimmed;
    HH_LOG_INFO(kLog, L"mkvmerge path set to '{}'", trimmed.empty() ? L"<auto>" : trimmed);
    commit();
}

void AppSettingsViewModel::setLutFolder(const std::wstring& path) {
    const std::wstring trimmed(platform::trim(path));
    if (trimmed == settings_.lutFolder) {
        return;
    }
    settings_.lutFolder = trimmed;
    HH_LOG_INFO(kLog, L"LUT folder set to '{}'", trimmed);
    commit();
}

void AppSettingsViewModel::setDefaultLut(TransferKind t, const std::wstring& path) {
    const std::wstring trimmed(platform::trim(path));
    switch (t) {
    case TransferKind::PQ:
        if (trimmed == settings_.pqLutPath) {
            return;
        }
        settings_.pqLutPath = trimmed;
        break;
    case TransferKind::HLG:
        if (trimmed == settings_.hlgLutPath) {
            return;
        }
        settings_.hlgLutPath = trimmed;
        break;
    default:
        // SDR exports get no LUT; Unknown has no default slot.
        HH_LOG_WARN(kLog, L"setDefaultLut: no default LUT slot for {}", toString(t));
        return;
    }
    HH_LOG_INFO(kLog, L"default LUT for {} set to '{}'", toString(t), trimmed);
    commit();
}

/**
 * @brief Picks a .cube from disk and stores it as the default for a transfer.
 *
 * The chooser only lists what the LUT folder and the recents hold, so this is
 * the way to point at a LUT that lives anywhere else.
 */
void AppSettingsViewModel::browseLut(TransferKind t) {
    // Start where the current default points, else in the configured folder.
    std::wstring start;
    const std::wstring current = settings_.defaultLutFor(static_cast<int>(t));
    if (!current.empty()) {
        start = path::parent(current);
    }
    if (start.empty() || !platform::isDirectory(start)) {
        start = settings_.expand(settings_.lutFolder);
    }
    if (start.empty() || !platform::isDirectory(start)) {
        start = platform::exeDirectory();
    }
    const std::wstring picked = pickCubeFile(owner_, start);
    if (picked.empty()) {
        // Cancelled: the pop-up is still showing the "Choose..." entry, so the
        // screen has to re-sync it back to the configured LUT.
        notify();
        return;
    }
    // Remember it so it appears in this chooser and in every job's LUT list.
    engine_.rememberLut(picked);
    setDefaultLut(t, picked);
    // setDefaultLut bails out when the value is unchanged, but the recents
    // list just moved, so the screen still needs a repaint.
    notify();
}

void AppSettingsViewModel::setDefaultPreset(TransferKind t, const std::wstring& presetId) {
    const std::wstring trimmed(platform::trim(presetId));
    if (trimmed.empty()) {
        HH_LOG_WARN(kLog, L"setDefaultPreset: empty preset id ignored");
        return;
    }
    switch (t) {
    case TransferKind::PQ:
        if (trimmed == settings_.presetPq) {
            return;
        }
        settings_.presetPq = trimmed;
        break;
    case TransferKind::HLG:
        if (trimmed == settings_.presetHlg) {
            return;
        }
        settings_.presetHlg = trimmed;
        break;
    default:
        HH_LOG_WARN(kLog, L"setDefaultPreset: no default preset slot for {}", toString(t));
        return;
    }
    HH_LOG_INFO(kLog, L"default preset for {} set to '{}'", toString(t), trimmed);
    commit();
}

void AppSettingsViewModel::setSuffix(const std::wstring& suffix) {
    const std::wstring trimmed(platform::trim(suffix));
    // The suffix becomes part of a file name; refuse what Windows would refuse.
    if (trimmed.empty()) {
        notifyUser(L"The output suffix cannot be empty", false);
        return;
    }
    if (path::hasInvalidFileNameChars(trimmed)) {
        notifyUser(L"The suffix contains characters not allowed in file names", false);
        return;
    }
    if (trimmed == settings_.suffix) {
        return;
    }
    settings_.suffix = trimmed;
    HH_LOG_INFO(kLog, L"output suffix set to '{}'", trimmed);
    commit();
}

void AppSettingsViewModel::setAttachLutByDefault(bool on) {
    if (settings_.attachLut == on) {
        return;
    }
    settings_.attachLut = on;
    commit();
}

void AppSettingsViewModel::removeWatchFolder(const std::wstring& folder) {
    if (platform::trim(folder).empty()) {
        return;
    }
    engine_.removeWatchFolder(folder);
    notify();
}

// ---- setters: behaviour ---------------------------------------------------

void AppSettingsViewModel::setAutoProcess(bool on) {
    engine_.setAutoProcess(on);
    notify();
}

void AppSettingsViewModel::setAutoProcessAme(bool on) {
    if (settings_.autoProcessAme == on) {
        return;
    }
    settings_.autoProcessAme = on;
    HH_LOG_INFO(kLog, L"auto-process from Media Encoder {}", on ? L"on" : L"off");
    commit();
}

void AppSettingsViewModel::setAutoProcessWatched(bool on) {
    if (settings_.autoProcessWatched == on) {
        return;
    }
    settings_.autoProcessWatched = on;
    HH_LOG_INFO(kLog, L"auto-process from watch folders {}", on ? L"on" : L"off");
    commit();
}

void AppSettingsViewModel::setRecycleOriginal(bool on) {
    const std::wstring mode = on ? L"auto" : L"never";
    if (platform::iequals(settings_.recycleMode, mode)) {
        return;
    }
    settings_.recycleMode = mode;
    HH_LOG_INFO(kLog, L"recycle mode set to {}", mode);
    commit();
}

void AppSettingsViewModel::setDockInsideAme(bool on) {
    const std::wstring mode = on ? L"auto" : L"floating";
    if (platform::iequals(settings_.dockMode, mode)) {
        return;
    }
    settings_.dockMode = mode;
    HH_LOG_INFO(kLog, L"dock mode set to {}", mode);
    persist();
    if (onDockChanged) {
        onDockChanged();
    }
}

void AppSettingsViewModel::setAlwaysOnTop(bool on) {
    if (settings_.floatingAlwaysOnTop == on) {
        return;
    }
    settings_.floatingAlwaysOnTop = on;
    persist();
    if (onWindowBehaviourChanged) {
        onWindowBehaviourChanged();
    }
}

void AppSettingsViewModel::setMinimizeToTray(bool on) {
    if (settings_.minimizeToTray == on) {
        return;
    }
    settings_.minimizeToTray = on;
    persist();
    if (onWindowBehaviourChanged) {
        onWindowBehaviourChanged();
    }
}

void AppSettingsViewModel::setStartMinimized(bool on) {
    if (settings_.startMinimized == on) {
        return;
    }
    settings_.startMinimized = on;
    persist();
    if (onWindowBehaviourChanged) {
        onWindowBehaviourChanged();
    }
}

void AppSettingsViewModel::setStartWithWindows(bool on) {
    if (settings_.startWithWindows == on) {
        return;
    }
    // The Run key itself is written by the app (it knows its own exe path);
    // this only records the wish and pokes the window-behaviour hook.
    settings_.startWithWindows = on;
    persist();
    if (onWindowBehaviourChanged) {
        onWindowBehaviourChanged();
    }
}

void AppSettingsViewModel::setShowOnAmeLaunch(bool on) {
    if (settings_.showOnAmeLaunch == on) {
        return;
    }
    settings_.showOnAmeLaunch = on;
    persist();
    notify();
}

void AppSettingsViewModel::setQuitWithAme(bool on) {
    if (settings_.quitWithAme == on) {
        return;
    }
    settings_.quitWithAme = on;
    persist();
    if (onWindowBehaviourChanged) {
        onWindowBehaviourChanged();
    }
}

// ---- setters: appearance --------------------------------------------------

void AppSettingsViewModel::setAppearance(int mode) {
    // Anything outside the three segments means "system".
    const int clamped = (mode >= 0 && mode <= 2) ? mode : 0;
    const wchar_t* word = clamped == 1 ? L"dark" : clamped == 2 ? L"light" : L"system";
    settings_.theme = word;
    themes_.setMode(themeModeFor(clamped));
    HH_LOG_INFO(kLog, L"appearance set to {}", word);
    persist();
    if (onAppearanceChanged) {
        onAppearanceChanged();
    }
}

void AppSettingsViewModel::setAccent(int mode) {
    const bool system = mode == 1;
    settings_.accent = system ? L"system" : L"blue";
    themes_.setAccentMode(system ? AccentMode::System : AccentMode::Blue);
    HH_LOG_INFO(kLog, L"accent set to {}", settings_.accent);
    persist();
    if (onAppearanceChanged) {
        onAppearanceChanged();
    }
}

void AppSettingsViewModel::setReduceTransparency(bool on) {
    settings_.reduceTransparency = on;
    themes_.setReduceTransparency(on);
    HH_LOG_INFO(kLog, L"reduce transparency {}", on ? L"on" : L"off");
    persist();
    if (onAppearanceChanged) {
        onAppearanceChanged();
    }
}

// ---- actions ---------------------------------------------------------------

void AppSettingsViewModel::installPanel() {
    const Result<void> r = ame::installPanel();
    if (r) {
        notifyUser(L"Panel installed - restart Media Encoder to load it", true);
    } else {
        notifyUser(L"Panel install failed: " + r.error().toString(), false);
    }
    // The panel status line changed either way.
    notify();
}

void AppSettingsViewModel::openLogFolder() {
    // The logger knows where it writes; before open() it is empty, so fall
    // back to the app's local data folder (where the log will live).
    std::wstring dir = Logger::instance().directory();
    if (dir.empty()) {
        dir = platform::appLocalDataFolder();
    }
    if (dir.empty() || !platform::openFolder(dir)) {
        notifyUser(L"Could not open the log folder", false);
    }
}

void AppSettingsViewModel::reprobeMkvmerge() {
    engine_.reprobeMkvmerge();
    const MkvmergeInfo& info = engine_.mkvmergeInfo();
    if (info.ok) {
        notifyUser(L"mkvmerge " + info.shortVersion() + L" found", true);
    } else if (info.found) {
        notifyUser(L"mkvmerge " + info.shortVersion() + L" is too old", false);
    } else {
        notifyUser(L"mkvmerge not found - install MKVToolNix", false);
    }
    notify();
}

// ===========================================================================
// AppLinkViewModel
// ===========================================================================

AppLinkViewModel::AppLinkViewModel(hh::Engine& engine, hh::ame::IDockControl& dock)
    : engine_(engine), dock_(dock), alive_(std::make_shared<bool>(true)) {}

AppLinkViewModel::~AppLinkViewModel() {
    if (alive_) {
        *alive_ = false;
    }
}

void AppLinkViewModel::bind() {
    // Binding twice would chain this object behind itself and fire onChanged twice.
    if (bound_) {
        HH_LOG_DEBUG(kLog, L"AppLinkViewModel::bind called twice; ignored");
        return;
    }
    bound_ = true;
    std::weak_ptr<bool> alive = alive_;

    // Engine link state (log found, panel connected, queue running).
    std::function<void()> previousLink = std::move(engine_.onLinkStateChanged);
    engine_.onLinkStateChanged = [this, previousLink, alive]() {
        if (previousLink) {
            previousLink();
        }
        const std::shared_ptr<bool> token = alive.lock();
        if (!token || !*token) {
            return;
        }
        notify();
    };

    // Dock state (the dock toggle glyph / tooltip).
    std::function<void(ame::DockState)> previousDock = std::move(dock_.onStateChanged);
    dock_.onStateChanged = [this, previousDock, alive](ame::DockState state) {
        if (previousDock) {
            previousDock(state);
        }
        const std::shared_ptr<bool> token = alive.lock();
        if (!token || !*token) {
            return;
        }
        notify();
    };
}

void AppLinkViewModel::notify() {
    if (onChanged) {
        onChanged();
    }
}

LinkView AppLinkViewModel::link() const {
    const LinkState s = engine_.linkState();
    LinkView v;
    v.panelLinked = s.panelConnected;
    v.logFound = s.logFound;
    v.queueRunning = s.queueRunning;
    v.docked = dock_.docked();
    v.dockingSupported = dock_.supported();

    // Tooltip: one fact per line, most useful first.
    std::vector<std::wstring> lines;
    lines.push_back(s.panelConnected ? L"Media Encoder panel connected" : L"Media Encoder panel not connected");
    if (s.logFound) {
        lines.push_back(L"AME log: " + path::ellipsizeMiddle(s.logPath, 60));
    } else {
        lines.push_back(L"AME log not found");
    }
    std::wstring queue = s.queueRunning ? L"Queue running" : L"Queue idle";
    if (s.activeJobs > 0) {
        queue += kDot + std::format(L"{} active {}", s.activeJobs, s.activeJobs == 1 ? L"job" : L"jobs");
    }
    lines.push_back(std::move(queue));
    if (s.watchFolders > 0) {
        lines.push_back(std::format(L"{} watch {}", s.watchFolders, s.watchFolders == 1 ? L"folder" : L"folders"));
    }
    lines.push_back(L"mkvmerge " + (s.mkvmergeVersion.empty() ? std::wstring(L"not found") : s.mkvmergeVersion));
    if (v.docked) {
        lines.push_back(L"Docked inside Media Encoder");
    }
    v.tooltip = platform::join(lines, L"\n");
    return v;
}

void AppLinkViewModel::toggleDock() {
    if (dock_.docked()) {
        HH_LOG_INFO(kLog, L"user undock");
        dock_.userUndock();
    } else {
        HH_LOG_INFO(kLog, L"user dock");
        dock_.userDock();
    }
    // The controller reports real transitions through onStateChanged; this
    // refresh covers the "nothing happened" case so the glyph never lies.
    notify();
}

// ===========================================================================
// AppGuideViewModel
// ===========================================================================

AppGuideViewModel::AppGuideViewModel(NativeWindowHandle owner) : owner_(owner) {}

void AppGuideViewModel::notifyUser(const std::wstring& message, bool ok) {
    if (ok) {
        HH_LOG_INFO(kLog, L"{}", message);
    } else {
        HH_LOG_WARN(kLog, L"{}", message);
    }
    if (onNotify) {
        onNotify(message, ok);
    }
}

std::wstring AppGuideViewModel::markdown() const {
    if (markdownCache_.empty()) {
        markdownCache_ = loadGuideMarkdown();
    }
    return markdownCache_;
}

std::wstring AppGuideViewModel::summaryText() const {
    // With a settings source the summary names the real suffix / LUT / presets.
    if (settings_) {
        const SettingsView view = settings_->view();
        return guideSummaryText(&view);
    }
    return guideSummaryText(nullptr);
}

void AppGuideViewModel::copySummary() {
    const std::wstring text = summaryText();
    if (platform::trim(text).empty()) {
        notifyUser(L"Nothing to copy", false);
        return;
    }
    const bool ok = copyTextToClipboard(owner_, text);
    notifyUser(ok ? L"Summary copied" : L"Could not copy to the clipboard", ok);
}

void AppGuideViewModel::openGuideFile() {
    // Prefer a real file the user can keep; otherwise materialise the embedded
    // copy in the app's data folder so the button always opens something.
    std::wstring file = locateGuideFile();
    if (file.empty()) {
        const std::wstring dir = platform::appLocalDataFolder();
        if (!dir.empty()) {
            const std::wstring target = path::join(dir, L"GUIDE.md");
            if (auto r = platform::writeAllAtomic(target, platform::toUtf8(markdown())); r) {
                file = target;
            } else {
                HH_LOG_WARN(kLog, L"cannot write {}: {}", target, r.error().toString());
            }
        }
    }
    if (file.empty()) {
        notifyUser(L"The guide file is not available", false);
        return;
    }
    if (!platform::openWithShell(file)) {
        notifyUser(L"Could not open " + path::fileName(file), false);
        return;
    }
    HH_LOG_INFO(kLog, L"opened guide {}", file);
}

} // namespace hh::ui
