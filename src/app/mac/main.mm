// ---------------------------------------------------------------------------
// main.mm - HdrHint.app entry point (macOS).
//
// The macOS twin of src/main.cpp: wires the engine, the Cocoa window, the
// menu bar item and the view-models together and handles the command line:
//
//   HdrHint                       normal start (window + menu bar item)
//   HdrHint --from-panel          started by the CEP panel (quits with AME)
//   HdrHint --tray                start hidden in the menu bar
//   HdrHint --show                bring the running instance to the front
//   HdrHint --install-panel       install/update the CEP panel and exit
//   HdrHint --uninstall-panel
//   HdrHint --process <file> [--preset id] [--lut path|none] [--suffix s]
//   HdrHint --screenshot <png> [--scenario N] [--tab N] [--dark|--light] [--docked-look] [--width W --height H]
//   HdrHint --migrate-from <settings.ini>
//   HdrHint --snap <png>          write the running instance's current frame to a PNG
//
// Mac specifics:
//   * the app is a menu bar agent (LSUIElement): it gains a Dock icon and
//     the main menu while its window is open and drops them when the window
//     retreats to the menu bar;
//   * Media Encoder's startup script cannot pass arguments (File.execute on
//     the .app), so it leaves a "launch-request" marker in Application
//     Support that this process consumes to recognise an AME launch;
//   * "Start with Windows" becomes Open at Login (SMAppService);
//   * docking into AME is Windows-only; the window always floats.
// ---------------------------------------------------------------------------
#include "ame/AmeProcess.h"
#include "ame/PanelInstaller.h"
#include "ame/mac/MacDockControl.h"
#include "core/Engine.h"
#include "core/HdrPresets.h"
#include "core/IpcProtocol.h"
#include "core/Logger.h"
#include "core/PathUtil.h"
#include "core/Settings.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Process.h"
#include "platform/RecycleBin.h"
#include "platform/SingleInstance.h"
#include "platform/Terms.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "ui/app/AppViewModels.h"
#include "ui/app/MockViewModels.h"
#include "ui/gfx/TextCache.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/mac/MacStatusItem.h"
#include "ui/mac/MacView.h"
#include "ui/mac/MacWindowHost.h"
#include "ui/screens/AppShell.h"
#include "ui/theme/ThemeManager.h"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace hh;

/// Component tag for the log.
constexpr const wchar_t* kLog = L"App";

/// How fresh the startup script's launch marker must be to count (100 ns ticks).
constexpr uint64_t kLaunchMarkerMaxAgeTicks = 120ull * 10'000'000ull;

/// File the AME startup script touches right before it opens the app.
constexpr const wchar_t* kLaunchMarkerName = L"launch-request";

/// Four-character code from its spelling ("oapp" -> 'oapp').
constexpr FourCharCode fourCC(const char (&s)[5]) {
    return (static_cast<FourCharCode>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<FourCharCode>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<FourCharCode>(static_cast<unsigned char>(s[2])) << 8) |
           static_cast<FourCharCode>(static_cast<unsigned char>(s[3]));
}

/// Apple event codes for "launched as a login item" (Carbon AE constants).
constexpr FourCharCode kAEOpenApplicationId = fourCC("oapp");
constexpr FourCharCode kAEPropDataKeyword = fourCC("prdt");
constexpr FourCharCode kAELaunchedAsLogInItemCode = fourCC("lgit");

// ---- small Cocoa helpers --------------------------------------------------------

/// Wide -> NSString (never nil).
NSString* ns(std::wstring_view text) {
    const std::string utf8 = platform::toUtf8(text);
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding] ?: @"";
}

/// NSString -> wide (NFC, the engine's path form).
std::wstring wide(NSString* s) {
    if (s == nil) {
        return {};
    }
    const char* utf8 = s.UTF8String;
    return utf8 != nullptr ? platform::normalizeNfc(platform::toWide(utf8)) : std::wstring();
}

/// Writes one line to stdout (the terminal that started the headless modes).
void consoleLine(const std::wstring& text) {
    const std::string utf8 = platform::toUtf8(text) + "\n";
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
}

/**
 * @brief Modal alert (app-modal, front and centre).
 * @param message  bold first line
 * @param detail   informative text below it (may be empty)
 */
void showAlert(const std::wstring& message, const std::wstring& detail, bool error) {
    @autoreleasepool {
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = error ? NSAlertStyleCritical : NSAlertStyleInformational;
        alert.messageText = ns(message);
        alert.informativeText = ns(detail);
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
    }
}

// ---- command line -------------------------------------------------------------------

/**
 * @brief Parsed command line (same flags as the Windows build).
 */
struct Args {
    std::vector<std::wstring> all;
    bool fromPanel = false;
    bool tray = false;
    bool show = false;
    bool installPanel = false;
    bool uninstallPanel = false;
    std::wstring processFile;
    std::wstring preset;
    std::wstring lut;
    std::wstring suffix;
    std::wstring screenshot;
    int scenario = 0;
    int tab = 0;
    int theme = 0;            ///< 0 system, 1 dark, 2 light
    bool dockedLook = false;
    int width = 0;
    int height = 0;
    std::wstring migrateFrom;
    std::vector<std::wstring> files;   ///< bare paths = files to process by hand

    /// Value after @p flag, or empty.
    [[nodiscard]] std::wstring value(const wchar_t* flag) const {
        for (size_t i = 0; i + 1 < all.size(); ++i) {
            if (platform::iequals(all[i], flag)) { return all[i + 1]; }
        }
        return {};
    }
    [[nodiscard]] bool has(const wchar_t* flag) const {
        for (const auto& a : all) { if (platform::iequals(a, flag)) { return true; } }
        return false;
    }
};

/**
 * @brief Consumes the startup script's marker: true when Media Encoder asked
 *        for this launch within the last two minutes.
 */
bool consumeLaunchMarker() {
    const std::wstring marker = path::join(platform::appLocalDataFolder(), kLaunchMarkerName);
    if (!platform::isFile(marker)) {
        return false;
    }
    bool fresh = false;
    if (auto attrs = platform::fileAttributes(marker)) {
        const uint64_t now = platform::nowUtc();
        const uint64_t written = attrs.value().lastWriteUtc;
        fresh = written <= now + kLaunchMarkerMaxAgeTicks && now - std::min(now, written) <= kLaunchMarkerMaxAgeTicks;
    }
    if (auto removed = platform::deleteFile(marker); !removed) {
        HH_LOG_WARN(kLog, L"launch marker could not be removed: {}", removed.error().toString());
    }
    HH_LOG_INFO(kLog, L"launch marker found ({})", fresh ? L"fresh: Media Encoder started us" : L"stale; ignored");
    return fresh;
}

/// Parses argv (UTF-8, NFC-normalised) into Args.
Args parseArgs(int argc, const char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) { continue; }
        // LaunchServices adds "-psn_0_12345" on older systems: not ours.
        if (std::strncmp(argv[i], "-psn_", 5) == 0) { continue; }
        // "-NSDocumentRevisionsDebugMode YES" style Xcode arguments: skip pairs.
        if (argv[i][0] == '-' && argv[i][1] != '-' && argv[i][1] != '\0' && std::strncmp(argv[i], "-NS", 3) == 0) {
            ++i;
            continue;
        }
        args.all.push_back(platform::normalizeNfc(platform::toWide(argv[i])));
    }
    args.fromPanel = args.has(L"--from-panel");
    args.tray = args.has(L"--tray");
    // Started by Media Encoder? Its startup script leaves a marker (it cannot
    // pass arguments); a direct child of AME counts too. Either way: treat it
    // exactly like a panel launch (quiet start, leave when AME leaves).
    if (!args.fromPanel) {
        const DWORD parent = platform::parentProcessId(static_cast<DWORD>(::getpid()));
        const bool ameParent = parent != 0 && platform::icontains(platform::processImageName(parent), L"Adobe Media Encoder");
        if (ameParent || consumeLaunchMarker()) {
            args.fromPanel = true;
            args.tray = true;
        }
    }
    args.show = args.has(L"--show");
    args.installPanel = args.has(L"--install-panel");
    args.uninstallPanel = args.has(L"--uninstall-panel");
    args.processFile = args.value(L"--process");
    args.preset = args.value(L"--preset");
    args.lut = args.value(L"--lut");
    args.suffix = args.value(L"--suffix");
    args.screenshot = args.value(L"--screenshot");
    args.scenario = static_cast<int>(platform::parseInt(args.value(L"--scenario")).value_or(0));
    args.tab = static_cast<int>(platform::parseInt(args.value(L"--tab")).value_or(0));
    args.theme = args.has(L"--dark") ? 1 : args.has(L"--light") ? 2 : 0;
    args.dockedLook = args.has(L"--docked-look");
    args.width = static_cast<int>(platform::parseInt(args.value(L"--width")).value_or(0));
    args.height = static_cast<int>(platform::parseInt(args.value(L"--height")).value_or(0));
    args.migrateFrom = args.value(L"--migrate-from");

    // Anything that is not a flag (and not a flag value) is a file to enqueue.
    for (size_t i = 0; i < args.all.size(); ++i) {
        const std::wstring& a = args.all[i];
        if (a.rfind(L"--", 0) == 0) {
            static const wchar_t* valued[] = {L"--process", L"--preset", L"--lut", L"--suffix", L"--screenshot", L"--scenario",
                                              L"--tab", L"--width", L"--height", L"--migrate-from", L"--ext-path", L"--dock-at",
                                              L"--snap"};
            for (const wchar_t* f : valued) { if (platform::iequals(a, f)) { ++i; break; } }
            continue;
        }
        if (platform::isFile(a)) { args.files.push_back(a); }
    }
    return args;
}

// ---- the application ------------------------------------------------------------------

/**
 * @brief Everything that lives for the duration of the GUI session.
 */
class App {
public:
    explicit App(Args args) : args_(std::move(args)) {}

    /// Loads settings, presets and the engine (no window yet).
    bool initCore();
    /// Headless modes that never need a run loop. Returns an exit code, or -1 to continue.
    int runHeadless();
    /// True for --screenshot.
    [[nodiscard]] bool screenshotMode() const noexcept { return !args_.screenshot.empty(); }
    /// --screenshot: renders the mock UI offscreen and writes a PNG. Returns the exit code.
    int runScreenshotMode();
    /// Becomes the primary instance, or forwards the arguments to it and returns false.
    bool acquireInstance();
    /// Builds the window, menu bar item and view-models; starts the engine.
    bool startGui();

    // ---- application delegate hooks -------------------------------------------
    /// Cmd+Q / Quit menu / logout: true when the app may terminate now.
    bool shouldTerminate();
    /// Orderly teardown right before the process exits.
    void shutdown();
    /// Dock icon clicked / app opened again.
    void reopen();
    /// Files opened with the app (Finder, Dock icon drop).
    void openFiles(const std::vector<std::wstring>& paths);

    // ---- menu actions ----------------------------------------------------------------
    void showTab(ui::AppTab tab);
    void addFilesDialog();
    void installPanelAction();
    void toggleFloatOnTop();
    [[nodiscard]] bool floatOnTop() const noexcept { return settings_.floatingAlwaysOnTop; }
    void openGuideFile();
    void showWindow(bool activate);
    void requestQuit(bool force);
    [[nodiscard]] bool guiStarted() const noexcept { return window_ != nullptr && shell_ != nullptr; }

    /// True when this launch was a login-item launch (quiet start).
    bool loginItemLaunch = false;

private:
    bool initWindow(bool screenshotMode);
    int runScreenshot();
    /// Asks before quitting while mkvmerge writes. True = go ahead.
    bool confirmQuitIfMuxRunning();
    void wireEngine();
    void wireWindow();
    void wireStatusItem();
    void handlePanelMessage(uint32_t conn, const std::string& kind, const std::string& line);
    void handleForwardedArgs(const std::vector<std::wstring>& forwarded);
    void showToast(const ToastRequest& t);
    void applyStartAtLogin();
    /// Dock icon + main menu while the window is up, menu bar only otherwise.
    void setForeground(bool foreground);

    Args args_;

    // core
    Settings settings_;
    PresetRegistry presets_;
    std::unique_ptr<Engine> engine_;
    platform::SingleInstance instance_;

    // ui
    ui::TextCache text_;
    ui::ThemeManager themes_;
    std::unique_ptr<ui::MacWindowHost> window_;
    ame::MacDockControl dock_;
    ui::MacStatusItem status_;

    // view-models (either the real ones or the mocks for --screenshot)
    std::unique_ptr<ui::AppQueueViewModel> queueVm_;
    std::unique_ptr<ui::AppSettingsViewModel> settingsVm_;
    std::unique_ptr<ui::AppLinkViewModel> linkVm_;
    std::unique_ptr<ui::AppGuideViewModel> guideVm_;
    std::unique_ptr<ui::MockQueueViewModel> mockQueue_;
    std::unique_ptr<ui::MockSettingsViewModel> mockSettings_;
    std::unique_ptr<ui::MockLinkViewModel> mockLink_;
    std::unique_ptr<ui::MockGuideViewModel> mockGuide_;
    ui::AppShell* shell_ = nullptr;

    bool quitting_ = false;
    bool shutDown_ = false;
    bool hiddenToMenuBarOnce_ = false;
    uint64_t ameMissingSinceMs_ = 0;
    bool ameSeenRunning_ = false;   ///< AME has been alive at least once this session
};

bool App::initCore() {
    settings_.applyMachineDefaults();
    const std::wstring settingsPath = Settings::defaultPath();
    if (auto r = settings_.load(settingsPath); !r) {
        HH_LOG_WARN(kLog, L"settings load: {}", r.error().toString());
    }
    settings_.applyMachineDefaults();

    // One-time import of the Python tool's settings.ini.
    if (!settings_.migrationDone) {
        const std::wstring legacy = !args_.migrateFrom.empty() ? args_.migrateFrom : Settings::findLegacyIni();
        if (!legacy.empty()) {
            auto migrated = settings_.migrateFromLegacy(legacy);
            HH_LOG_INFO(kLog, L"legacy settings {}: {}", legacy, migrated && migrated.value() ? L"migrated" : L"nothing to migrate");
        } else {
            settings_.migrationDone = true;
        }
        settings_.save(settingsPath);
    }

    Logger::instance().setLevel(Logger::parseLevel(settings_.logLevel, LogLevel::Info));
    presets_.loadUser(settings_.expand(settings_.userPresetsFile));
    engine_ = std::make_unique<Engine>(settings_, presets_);
    return true;
}

int App::runHeadless() {
    // ---- one-shot mux ---------------------------------------------------------
    if (!args_.processFile.empty()) {
        if (!args_.suffix.empty()) { settings_.suffix = args_.suffix; }
        int last = -1;
        auto result = engine_->processOneShot(args_.processFile, args_.preset, args_.lut, [&](float p) {
            const int percent = static_cast<int>(p * 100.0f + 0.5f);
            if (percent / 10 != last / 10) { last = percent; consoleLine(L"muxing " + std::to_wstring(percent) + L"%"); }
        });
        if (!result) {
            consoleLine(L"error: " + result.error().toString());
            return 4;
        }
        consoleLine(L"created: " + result.value());
        return 0;
    }

    // ---- panel install / uninstall -------------------------------------------------
    if (args_.installPanel || args_.uninstallPanel) {
        auto r = args_.installPanel ? ame::installPanel() : ame::uninstallPanel();
        const std::wstring message = r ? (args_.installPanel ? L"The HDR Hint panel is installed." : L"The HDR Hint panel was removed.")
                                       : L"That didn't work.";
        const std::wstring detail = r ? (args_.installPanel ? L"Restart Adobe Media Encoder, then open Window > Extensions > HDR Hint."
                                                            : std::wstring())
                                      : r.error().toString();
        consoleLine(message + (detail.empty() ? L"" : L" " + detail));
        // A terminal run stays in the terminal; a Finder / script run gets a dialog.
        if (!::isatty(STDOUT_FILENO)) {
            showAlert(message, detail, !r);
        }
        return r ? 0 : 2;
    }
    return -1;
}

bool App::initWindow(bool screenshotMode) {
    text_.init();
    ui::setSharedTextCache(&text_);

    // Theme from settings (or the screenshot flags).
    const std::wstring themeSetting = screenshotMode ? (args_.theme == 1 ? L"dark" : args_.theme == 2 ? L"light" : L"system") : settings_.theme;
    themes_.setMode(platform::iequals(themeSetting, L"dark") ? ui::ThemeMode::Dark
                    : platform::iequals(themeSetting, L"light") ? ui::ThemeMode::Light : ui::ThemeMode::System);
    themes_.setAccentMode(platform::iequals(settings_.accent, L"system") ? ui::AccentMode::System : ui::AccentMode::Blue);
    themes_.setReduceTransparency(settings_.reduceTransparency);

    ui::MacWindowSpec spec;
    spec.title = L"HDR Hint";
    spec.alwaysOnTop = settings_.floatingAlwaysOnTop && !screenshotMode;
    spec.topBarHeight = ui::AppShell::kTopBarHeight;
    if (args_.width > 0 && args_.height > 0) {
        spec.initialDips = ui::Size{static_cast<float>(args_.width), static_cast<float>(args_.height)};
    }
    window_ = std::make_unique<ui::MacWindowHost>(text_, themes_);
    if (!window_->create(spec)) {
        showAlert(L"The main window could not be created.", L"", true);
        return false;
    }
    if (!screenshotMode && !settings_.floatingPlacement.empty()) {
        window_->applyPlacement(settings_.floatingPlacement);
    }

    // Theme changes repaint the whole tree and refresh the backdrop.
    themes_.addListener([this] {
        if (window_) {
            window_->root().themeChanged();
            window_->refreshBackdrop();
        }
    });
    // The top bar leaves room for the traffic lights.
    window_->onTrafficLightsChanged = [this](float inset) {
        if (shell_) { shell_->setNativeWindowControls(true, inset); }
    };
    return true;
}

void App::wireEngine() {
    engine_->onToast = [this](const ToastRequest& t) { showToast(t); };
    engine_->onPanelMessage = [this](uint32_t conn, const std::string& kind, const std::string& line) {
        handlePanelMessage(conn, kind, line);
    };
}

void App::wireWindow() {
    window_->onCloseRequested = [this] {
        // The red button retreats to the menu bar when enabled; otherwise it quits.
        if (settings_.minimizeToTray && status_.added()) {
            window_->hide();
            setForeground(false);
            if (!hiddenToMenuBarOnce_) {
                hiddenToMenuBarOnce_ = true;
                status_.notify(L"HDR Hint keeps watching",
                               L"Exports are still processed. Click the menu bar icon to reopen.");
            }
        } else {
            requestQuit(false);
        }
    };
    window_->onSystemSettingsChanged = [this] { themes_.refreshFromSystem(); };
    window_->onFilesDropped = [this](const std::vector<std::wstring>& files) {
        for (const auto& f : files) { if (engine_) { engine_->addManualFile(f); } }
        showWindow(true);
    };
    window_->onActivate = [this](bool active) { if (shell_) { shell_->setWindowActive(active); } };
    window_->onDpiChanged = [this] { if (window_) { window_->root().dpiChanged(); } };
    window_->onTick = [this] {
        if (engine_) { engine_->tick(); }
        // Remember the placement.
        if (window_) {
            const std::wstring placement = window_->placementString();
            if (!placement.empty() && placement != settings_.floatingPlacement) {
                settings_.floatingPlacement = placement;
            }
        }
        // Leave with Media Encoder when the setting says so, however this
        // instance was started. AME must have been seen running once, and a
        // running mux always finishes first.
        if (settings_.quitWithAme && !quitting_) {
            if (ame::isAmeRunning()) {
                ameSeenRunning_ = true;
                ameMissingSinceMs_ = 0;
            } else if (ameSeenRunning_) {
                const uint64_t now = platform::nowMonotonicMs();
                if (ameMissingSinceMs_ == 0) { ameMissingSinceMs_ = now; }
                if (now - ameMissingSinceMs_ > 5000 && !(engine_ && engine_->muxRunning())) {
                    HH_LOG_INFO(kLog, L"Media Encoder exited; quitting (quit_with_ame is on)");
                    requestQuit(true);
                }
            }
        }
    };
}

void App::wireStatusItem() {
    if (!status_.add(L"HDR Hint")) {
        HH_LOG_WARN(kLog, L"menu bar item could not be added");
    }
    enum : int { IdShow = 1, IdPause = 3, IdGuide = 4, IdInstall = 5, IdQuit = 6 };
    status_.setMenuProvider([this]() {
        std::vector<ui::MacStatusItem::MenuItem> items;
        items.push_back({IdShow, L"Show HDR Hint"});
        items.push_back({IdPause, L"Pause Automation", !settings_.autoProcess});
        items.push_back({0, L""});
        items.push_back({IdGuide, L"Open Export Guide"});
        items.push_back({IdInstall, L"Install Media Encoder Panel"});
        items.push_back({0, L""});
        items.push_back({IdQuit, L"Quit HDR Hint"});
        return items;
    });
    status_.onCommand = [this](int id) {
        switch (id) {
        case IdShow: showWindow(true); break;
        case IdPause: if (engine_) { engine_->setAutoProcess(!settings_.autoProcess); } break;
        case IdGuide: openGuideFile(); break;
        case IdInstall: installPanelAction(); break;
        case IdQuit: requestQuit(false); break;
        default: break;
        }
    };
    status_.onNotificationClicked = [this] { showWindow(true); };
}

void App::handlePanelMessage(uint32_t conn, const std::string& kind, const std::string& line) {
    static_cast<void>(conn);
    auto parsed = ipc::parseLine(line);
    if (!parsed) { return; }
    const ipc::json& msg = *parsed;
    // Docking messages (hello skin, panelBounds, lifecycle) have no Mac
    // counterpart; the panel's commands still work.
    if (kind == "command") {
        const std::string cmd = ipc::str(msg, "cmd");
        if (cmd == "show") { showWindow(true); }
        else if (cmd == "dock") { dock_.userDock(); }
        else if (cmd == "undock") { dock_.userUndock(); }
    }
}

void App::handleForwardedArgs(const std::vector<std::wstring>& forwarded) {
    bool show = forwarded.empty();
    for (size_t i = 0; i < forwarded.size(); ++i) {
        const std::wstring& a = forwarded[i];
        if (platform::iequals(a, L"--show") || platform::iequals(a, L"--from-panel")) { show = true; continue; }
        if (platform::iequals(a, L"--dock")) { dock_.userDock(); continue; }
        if (platform::iequals(a, L"--undock")) { dock_.userUndock(); continue; }
        if (platform::iequals(a, L"--dock-at") && i + 1 < forwarded.size()) { ++i; dock_.userDock(); continue; }
        if (platform::iequals(a, L"--snap") && i + 1 < forwarded.size()) {
            // "--snap <png>": the live UI as a PNG, composited over the opaque
            // window colour so it reads like the screen does.
            const std::wstring out = forwarded[++i];
            std::vector<uint8_t> bgra;
            UINT w = 0, h = 0;
            const bool captured = window_ && window_->captureFrame(bgra, w, h);
            if (captured) {
                const ui::Color bg = themes_.current().windowBackground;
                const float bgB = std::clamp(bg.b, 0.0f, 1.0f) * 255.0f;
                const float bgG = std::clamp(bg.g, 0.0f, 1.0f) * 255.0f;
                const float bgR = std::clamp(bg.r, 0.0f, 1.0f) * 255.0f;
                for (size_t px = 0; px + 3 < bgra.size(); px += 4) {
                    const float inv = 1.0f - static_cast<float>(bgra[px + 3]) / 255.0f;
                    bgra[px + 0] = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(bgra[px + 0]) + bgB * inv));
                    bgra[px + 1] = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(bgra[px + 1]) + bgG * inv));
                    bgra[px + 2] = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(bgra[px + 2]) + bgR * inv));
                    bgra[px + 3] = 255;
                }
            }
            if (captured && ui::mac::writePng(out, bgra.data(), w, h)) {
                HH_LOG_INFO(kLog, L"--snap wrote {} ({}x{})", out, w, h);
            } else {
                HH_LOG_WARN(kLog, L"--snap failed for {}", out);
            }
            continue;
        }
        if (platform::iequals(a, L"--install-panel")) {
            installPanelAction();
            continue;
        }
        if (a.rfind(L"--", 0) == 0) { continue; }
        if (platform::isFile(a) && engine_) { engine_->addManualFile(a); show = true; }
    }
    if (show) { showWindow(true); }
}

/**
 * @brief Mirrors Settings > "Open at login" into a login item (SMAppService).
 *
 * The login launch is recognised by its Apple event and starts quietly in the
 * menu bar. Any failure is logged, never fatal.
 */
void App::applyStartAtLogin() {
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
        if (![bundle.bundlePath.pathExtension isEqualToString:@"app"]) {
            HH_LOG_DEBUG(kLog, L"open at login: not running from an app bundle; skipped");
            return;
        }
        SMAppService* service = [SMAppService mainAppService];
        NSError* error = nil;
        if (settings_.startWithWindows) {
            if (service.status == SMAppServiceStatusEnabled) {
                return;
            }
            if (![service registerAndReturnError:&error]) {
                HH_LOG_WARN(kLog, L"open at login: register failed: {}", wide(error.localizedDescription));
            } else {
                HH_LOG_INFO(kLog, L"open at login: enabled");
            }
            return;
        }
        if (service.status == SMAppServiceStatusEnabled || service.status == SMAppServiceStatusRequiresApproval) {
            if (![service unregisterAndReturnError:&error]) {
                HH_LOG_WARN(kLog, L"open at login: unregister failed: {}", wide(error.localizedDescription));
            } else {
                HH_LOG_INFO(kLog, L"open at login: disabled");
            }
        }
    }
}

void App::setForeground(bool foreground) {
    const NSApplicationActivationPolicy wanted =
        foreground ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory;
    if (NSApp.activationPolicy != wanted) {
        [NSApp setActivationPolicy:wanted];
    }
}

void App::showWindow(bool activate) {
    if (!window_) { return; }
    setForeground(true);
    window_->show(activate);
    if (activate) { window_->bringToFront(); }
}

void App::showToast(const ToastRequest& t) {
    ui::ToastSpec spec;
    spec.text = t.text;
    spec.tone = t.tone == ToastRequest::Tone::Success ? ui::ToastTone::Success
              : t.tone == ToastRequest::Tone::Warning ? ui::ToastTone::Warning
              : t.tone == ToastRequest::Tone::Error ? ui::ToastTone::Error : ui::ToastTone::Info;
    if (!t.actionLabel.empty() && t.jobId != 0) {
        spec.actionLabel = t.actionLabel;
        const JobId id = t.jobId;
        const bool recycle = platform::iequals(t.actionLabel, platform::terms::kTrashAction);
        spec.onAction = [this, id, recycle] {
            if (!engine_) { return; }
            if (recycle) { engine_->recycleJob(id); } else { engine_->revealJob(id, true); }
        };
        spec.durationSec = recycle ? 0.0 : 6.0;
    }
    if (shell_ && window_ && window_->visible()) {
        shell_->showToast(std::move(spec));
    } else if (status_.added() && settings_.toastOnDone) {
        status_.notify(L"HDR Hint", t.text);
    }
}

bool App::confirmQuitIfMuxRunning() {
    if (!engine_ || !engine_->muxRunning()) {
        return true;
    }
    @autoreleasepool {
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"mkvmerge is still writing a file.";
        alert.informativeText = @"Quit now and cancel it?";
        // Cancel is the default (Return): quitting throws work away.
        [alert addButtonWithTitle:@"Cancel"];
        NSButton* quit = [alert addButtonWithTitle:@"Quit"];
        quit.hasDestructiveAction = YES;
        return [alert runModal] == NSAlertSecondButtonReturn;
    }
}

void App::requestQuit(bool force) {
    if (quitting_) { return; }
    if (!force && !confirmQuitIfMuxRunning()) { return; }
    quitting_ = true;
    [NSApp terminate:nil];
}

bool App::shouldTerminate() {
    if (quitting_) {
        return true;
    }
    // Cmd+Q, the Dock's Quit, logout: same question as the Quit item.
    if (!confirmQuitIfMuxRunning()) {
        return false;
    }
    quitting_ = true;
    return true;
}

void App::shutdown() {
    if (shutDown_) { return; }
    shutDown_ = true;
    quitting_ = true;
    if (engine_) { engine_->stop(false); }
    settings_.save(Settings::defaultPath());
    status_.remove();
    shell_ = nullptr;
    if (window_) { window_->destroy(); }
    ui::setSharedTextCache(nullptr);
    HH_LOG_INFO(kLog, L"exit 0");
    Logger::instance().close();
}

void App::reopen() {
    // Media Encoder's startup script re-opens a running app: consume its marker.
    const std::wstring marker = path::join(platform::appLocalDataFolder(), kLaunchMarkerName);
    if (platform::isFile(marker)) {
        if (auto removed = platform::deleteFile(marker); !removed) {
            HH_LOG_WARN(kLog, L"launch marker could not be removed: {}", removed.error().toString());
        }
    }
    showWindow(true);
}

void App::openFiles(const std::vector<std::wstring>& paths) {
    for (const auto& p : paths) {
        if (engine_ && platform::isFile(p)) { engine_->addManualFile(p); }
    }
    showWindow(true);
}

void App::showTab(ui::AppTab tab) {
    showWindow(true);
    if (shell_) { shell_->setTab(tab); }
}

void App::addFilesDialog() {
    if (!engine_) { return; }
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = YES;
        panel.message = @"Pick the exports to tag.";
        panel.prompt = @"Add";
        panel.allowedContentTypes = @[ UTTypeMovie ];
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) {
            return;
        }
        for (NSURL* url in panel.URLs) {
            if (url.isFileURL) { engine_->addManualFile(wide(url.path)); }
        }
    }
    showWindow(true);
}

void App::installPanelAction() {
    auto r = ame::installPanel();
    ToastRequest t;
    t.tone = r ? ToastRequest::Tone::Success : ToastRequest::Tone::Error;
    t.text = r ? L"Panel installed. Restart Media Encoder, then open Window > Extensions > HDR Hint."
               : L"Panel install failed: " + r.error().toString();
    showToast(t);
}

void App::toggleFloatOnTop() {
    settings_.floatingAlwaysOnTop = !settings_.floatingAlwaysOnTop;
    if (window_) { window_->setAlwaysOnTop(settings_.floatingAlwaysOnTop); }
    settings_.save(Settings::defaultPath());
    // The Settings screen shows the same switch.
    if (settingsVm_ && settingsVm_->onChanged) { settingsVm_->onChanged(); }
}

void App::openGuideFile() {
    const std::wstring guide = path::join(platform::resourceDirectory(), L"GUIDE.md");
    if (!platform::openWithShell(guide)) {
        HH_LOG_WARN(kLog, L"could not open {}", guide);
    }
}

int App::runScreenshotMode() {
    if (!initWindow(true)) {
        return 1;
    }
    return runScreenshot();
}

bool App::acquireInstance() {
    if (instance_.acquire()) {
        return true;
    }
    std::vector<std::wstring> forward = args_.all;
    if (forward.empty()) { forward.emplace_back(L"--show"); }
    if (!platform::SingleInstance::forwardToPrimary(forward)) {
        HH_LOG_WARN(kLog, L"another instance holds the lock but did not take the arguments");
    }
    return false;
}

int App::runScreenshot() {
    mockQueue_ = std::make_unique<ui::MockQueueViewModel>();
    mockSettings_ = std::make_unique<ui::MockSettingsViewModel>();
    mockLink_ = std::make_unique<ui::MockLinkViewModel>();
    mockGuide_ = std::make_unique<ui::MockGuideViewModel>();
    ui::mockApplyState(*mockQueue_, args_.scenario);

    auto shell = std::make_unique<ui::AppShell>(*mockQueue_, *mockSettings_, *mockLink_, *mockGuide_);
    shell_ = shell.get();
    window_->root().setContent(std::move(shell));
    shell_->setNativeWindowControls(true, window_->trafficLightsInset());
    shell_->setDockedLayout(args_.dockedLook);
    themes_.setDocked(args_.dockedLook, std::nullopt);
    // A PNG has no desktop behind it: paint the opaque window background.
    themes_.setReduceTransparency(true);
    window_->refreshBackdrop();
    shell_->setTab(static_cast<ui::AppTab>(std::clamp(args_.tab, 0, 2)), false);

    // Let springs settle: ~700 ms of frames, with the run loop turning so
    // deferred work (dispatch_async) runs too.
    const uint64_t start = platform::nowMonotonicMs();
    while (platform::nowMonotonicMs() - start < 700) {
        window_->root().tickAnimations();
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.016]];
    }
    // Retina-density PNG regardless of the machine's display.
    std::vector<uint8_t> bgra;
    UINT w = 0, h = 0;
    if (!window_->captureFrame(bgra, w, h, 2.0f)) {
        HH_LOG_ERROR(kLog, L"screenshot: frame capture failed");
        return 3;
    }
    if (!ui::mac::writePng(args_.screenshot, bgra.data(), w, h)) {
        HH_LOG_ERROR(kLog, L"screenshot: PNG write failed for {}", args_.screenshot);
        return 4;
    }
    HH_LOG_INFO(kLog, L"screenshot written: {} ({}x{})", args_.screenshot, w, h);
    shell_ = nullptr;
    window_->destroy();
    return 0;
}

bool App::startGui() {
    if (!initWindow(false)) { return false; }
    applyStartAtLogin();
    wireEngine();
    wireWindow();

    queueVm_ = std::make_unique<ui::AppQueueViewModel>(*engine_, window_->nsWindow());
    settingsVm_ = std::make_unique<ui::AppSettingsViewModel>(*engine_, settings_, themes_, window_->nsWindow());
    linkVm_ = std::make_unique<ui::AppLinkViewModel>(*engine_, dock_);
    guideVm_ = std::make_unique<ui::AppGuideViewModel>(window_->nsWindow());
    queueVm_->bind();
    linkVm_->bind();
    settingsVm_->onAppearanceChanged = [this] { window_->root().themeChanged(); };
    settingsVm_->onDockChanged = [] {};
    settingsVm_->onWindowBehaviourChanged = [this] {
        window_->setAlwaysOnTop(settings_.floatingAlwaysOnTop);
        applyStartAtLogin();
    };
    settingsVm_->onNotify = [this](const std::wstring& text, bool ok) {
        ToastRequest t;
        t.tone = ok ? ToastRequest::Tone::Success : ToastRequest::Tone::Error;
        t.text = text;
        showToast(t);
    };

    auto shell = std::make_unique<ui::AppShell>(*queueVm_, *settingsVm_, *linkVm_, *guideVm_);
    shell_ = shell.get();
    shell_->onMinimize = [this] { window_->minimize(); };
    shell_->onMaximize = [this] { window_->toggleZoom(); };
    shell_->onClose = [this] { if (window_->onCloseRequested) { window_->onCloseRequested(); } };
    window_->root().setContent(std::move(shell));
    shell_->setNativeWindowControls(true, window_->trafficLightsInset());
    window_->root().timeline().setReducedMotion(ui::ThemeManager::readReducedMotion());

    wireStatusItem();
    instance_.registerReceiver([this](const std::vector<std::wstring>& forwarded) { handleForwardedArgs(forwarded); });

    // Start the engine (workers + IPC); its events hop to the main queue.
    Engine* engine = engine_.get();
    const bool* quitting = &quitting_;
    auto started = engine_->start([engine, quitting]() -> bool {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!*quitting) {
                engine->onEventMessage();
            }
        });
        return true;
    });
    if (!started) {
        HH_LOG_ERROR(kLog, L"engine start: {}", started.error().toString());
        ToastRequest t;
        t.tone = ToastRequest::Tone::Error;
        t.text = L"Engine failed to start: " + started.error().toString();
        showToast(t);
    }
    for (const auto& f : args_.files) { engine_->addManualFile(f); }
    // The screens were built before the engine located mkvmerge and scanned
    // the LUT folder: re-sync them so Settings shows the real state.
    if (settingsVm_ && settingsVm_->onChanged) { settingsVm_->onChanged(); }
    if (queueVm_ && queueVm_->onChanged) { queueVm_->onChanged(); }

    // First run: offer the panel when it is not installed yet.
    if (settings_.firstRun) {
        settings_.firstRun = false;
        settings_.save(Settings::defaultPath());
        const ame::PanelStatus status = ame::queryPanelStatus();
        if (!status.installed) {
            ToastRequest t;
            t.tone = ToastRequest::Tone::Info;
            t.text = L"Install the Media Encoder panel from Settings so AME can launch HDR Hint for you.";
            showToast(t);
        }
    }

    window_->startTicking();

    // Media Encoder started us: show the window unless the user asked for a
    // quiet start. --tray on its own (or a login-item launch) stays in the menu bar.
    const bool quiet = args_.tray || settings_.startMinimized || loginItemLaunch;
    const bool ameLaunchedUs = args_.fromPanel && settings_.showOnAmeLaunch && !settings_.startMinimized;
    if (!quiet || ameLaunchedUs) {
        showWindow(!args_.fromPanel);
    } else {
        setForeground(false);
    }
    return true;
}

} // namespace

// ===========================================================================
// Application delegate + main menu
// ===========================================================================

@interface HHAppDelegate : NSObject <NSApplicationDelegate, NSMenuItemValidation>
- (instancetype)initWithApp:(App*)app;
@end

@implementation HHAppDelegate {
    App* app_;
}

- (instancetype)initWithApp:(App*)app {
    self = [super init];
    if (self != nil) {
        app_ = app;
    }
    return self;
}

/// Adds an item to @p menu (target = self unless @p action goes up the responder chain).
- (NSMenuItem*)add:(NSString*)title action:(SEL)action key:(NSString*)key to:(NSMenu*)menu ownTarget:(BOOL)own {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key ?: @""];
    if (own) {
        item.target = self;
    }
    [menu addItem:item];
    return item;
}

/// Hangs @p menu off the menu bar under @p title.
- (void)attach:(NSMenu*)menu title:(NSString*)title to:(NSMenu*)bar {
    NSMenuItem* top = [bar addItemWithTitle:title action:nil keyEquivalent:@""];
    top.submenu = menu;
}

/// The standard menu bar: App, File, Edit, View, Window, Help.
- (void)buildMainMenu {
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@""];
    NSString* name = @"HDR Hint";

    // ---- HDR Hint -------------------------------------------------------------
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name];
    [self add:[@"About " stringByAppendingString:name] action:@selector(orderFrontStandardAboutPanel:) key:@"" to:appMenu ownTarget:NO];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self add:@"Settings…" action:@selector(showSettings:) key:@"," to:appMenu ownTarget:YES];
    [self add:@"Install Media Encoder Panel" action:@selector(installPanel:) key:@"" to:appMenu ownTarget:YES];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* services = [self add:@"Services" action:nil key:@"" to:appMenu ownTarget:NO];
    NSMenu* servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    services.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self add:[@"Hide " stringByAppendingString:name] action:@selector(hide:) key:@"h" to:appMenu ownTarget:NO];
    NSMenuItem* hideOthers = [self add:@"Hide Others" action:@selector(hideOtherApplications:) key:@"h" to:appMenu ownTarget:NO];
    hideOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [self add:@"Show All" action:@selector(unhideAllApplications:) key:@"" to:appMenu ownTarget:NO];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self add:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) key:@"q" to:appMenu ownTarget:NO];
    [self attach:appMenu title:name to:bar];

    // ---- File ------------------------------------------------------------------
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [self add:@"Add Exports…" action:@selector(addFiles:) key:@"o" to:fileMenu ownTarget:YES];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [self add:@"Close Window" action:@selector(performClose:) key:@"w" to:fileMenu ownTarget:NO];
    [self attach:fileMenu title:@"File" to:bar];

    // ---- Edit (first responder: the canvas view replays these as shortcuts) -----
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [self add:@"Undo" action:@selector(undo:) key:@"z" to:editMenu ownTarget:NO];
    NSMenuItem* redo = [self add:@"Redo" action:@selector(redo:) key:@"z" to:editMenu ownTarget:NO];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:[NSMenuItem separatorItem]];
    [self add:@"Cut" action:@selector(cut:) key:@"x" to:editMenu ownTarget:NO];
    [self add:@"Copy" action:@selector(copy:) key:@"c" to:editMenu ownTarget:NO];
    [self add:@"Paste" action:@selector(paste:) key:@"v" to:editMenu ownTarget:NO];
    [self add:@"Select All" action:@selector(selectAll:) key:@"a" to:editMenu ownTarget:NO];
    [self attach:editMenu title:@"Edit" to:bar];

    // ---- View ------------------------------------------------------------------------
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [self add:@"Queue" action:@selector(showQueue:) key:@"1" to:viewMenu ownTarget:YES];
    [self add:@"Settings" action:@selector(showSettings:) key:@"2" to:viewMenu ownTarget:YES];
    [self add:@"Guide" action:@selector(showGuide:) key:@"3" to:viewMenu ownTarget:YES];
    [viewMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* fullScreen = [self add:@"Enter Full Screen" action:@selector(toggleFullScreen:) key:@"f" to:viewMenu ownTarget:NO];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [self attach:viewMenu title:@"View" to:bar];

    // ---- Window ------------------------------------------------------------------
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [self add:@"Minimize" action:@selector(performMiniaturize:) key:@"m" to:windowMenu ownTarget:NO];
    [self add:@"Zoom" action:@selector(performZoom:) key:@"" to:windowMenu ownTarget:NO];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [self add:@"Float on Top" action:@selector(toggleFloatOnTop:) key:@"" to:windowMenu ownTarget:YES];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [self add:@"Bring All to Front" action:@selector(arrangeInFront:) key:@"" to:windowMenu ownTarget:NO];
    [self attach:windowMenu title:@"Window" to:bar];
    NSApp.windowsMenu = windowMenu;

    // ---- Help --------------------------------------------------------------------------
    NSMenu* helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
    [self add:@"HDR Hint Guide" action:@selector(showGuide:) key:@"?" to:helpMenu ownTarget:YES];
    [self add:@"Open Export Guide File" action:@selector(openGuideFile:) key:@"" to:helpMenu ownTarget:YES];
    [self attach:helpMenu title:@"Help" to:bar];
    NSApp.helpMenu = helpMenu;

    NSApp.mainMenu = bar;
}

// ---- NSApplicationDelegate ---------------------------------------------------------------

- (void)applicationWillFinishLaunching:(NSNotification*)note {
    [self buildMainMenu];
    // Files opened with the app arrive through application:openURLs:.
    NSAppleEventDescriptor* event = [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
    if (event != nil && event.eventID == kAEOpenApplicationId &&
        [event paramDescriptorForKeyword:kAEPropDataKeyword].enumCodeValue == kAELaunchedAsLogInItemCode) {
        app_->loginItemLaunch = true;
        HH_LOG_INFO(kLog, L"launched as a login item");
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    if (!app_->startGui()) {
        HH_LOG_ERROR(kLog, L"GUI start failed; exiting");
        app_->shutdown();
        exit(1);
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (!app_->shouldTerminate()) {
        return NSTerminateCancel;
    }
    app_->shutdown();
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return NO;   // the menu bar item keeps running
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    app_->reopen();
    return NO;
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    std::vector<std::wstring> paths;
    for (NSURL* url in urls) {
        if (url.isFileURL) { paths.push_back(wide(url.path)); }
    }
    if (!paths.empty() && app_->guiStarted()) {
        app_->openFiles(paths);
    }
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    return YES;
}

// ---- menu actions ------------------------------------------------------------------------

- (void)showQueue:(id)sender { app_->showTab(hh::ui::AppTab::Queue); }
- (void)showSettings:(id)sender { app_->showTab(hh::ui::AppTab::Settings); }
- (void)showGuide:(id)sender { app_->showTab(hh::ui::AppTab::Guide); }
- (void)addFiles:(id)sender { app_->addFilesDialog(); }
- (void)installPanel:(id)sender { app_->installPanelAction(); }
- (void)toggleFloatOnTop:(id)sender { app_->toggleFloatOnTop(); }
- (void)openGuideFile:(id)sender { app_->openGuideFile(); }

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(toggleFloatOnTop:)) {
        item.state = app_->floatOnTop() ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return app_->guiStarted();
}

@end

// ===========================================================================
// main
// ===========================================================================

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        // A closed pipe (the panel, a script) must not kill the app.
        std::signal(SIGPIPE, SIG_IGN);

        // The log first (argument parsing already reports), then NSApp so the
        // theme and alerts can reach AppKit from the start.
        Logger::instance().open(platform::appLogsFolder(), LogLevel::Info, 2048, 5);
        NSApplication* application = [NSApplication sharedApplication];
        Args args = parseArgs(argc, argv);
        HH_LOG_INFO(kLog, L"HDR Hint {} starting ({} args)", ui::appVersionString(), args.all.size());

        auto* app = new App(std::move(args));   // lives until exit()
        if (!app->initCore()) {
            return 1;
        }

        // ---- modes that need no window ----------------------------------------------
        if (const int code = app->runHeadless(); code >= 0) {
            HH_LOG_INFO(kLog, L"exit {}", code);
            Logger::instance().close();
            return code;
        }

        // ---- offscreen screenshot: a window that is never shown ------------------
        if (app->screenshotMode()) {
            [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [application finishLaunching];
            const int code = app->runScreenshotMode();
            HH_LOG_INFO(kLog, L"exit {}", code);
            Logger::instance().close();
            return code;
        }

        // ---- single instance ------------------------------------------------------------
        if (!app->acquireInstance()) {
            HH_LOG_INFO(kLog, L"arguments forwarded to the running instance; exit 0");
            Logger::instance().close();
            return 0;
        }

        // ---- the GUI: a menu bar agent until the window shows ----------------------------
        static HHAppDelegate* s_delegate = [[HHAppDelegate alloc] initWithApp:app];
        application.delegate = s_delegate;
        [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [application run];
        // terminate: normally exits from applicationShouldTerminate:; be tidy if run returns.
        app->shutdown();
        return 0;
    }
}
