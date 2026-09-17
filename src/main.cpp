// ---------------------------------------------------------------------------
// main.cpp - HdrHint.exe entry point.
//
// Wires the engine (workers + job state machine), the Direct2D window, the
// AME docking controller, the tray icon and the view-models together, and
// handles the command line:
//
//   HdrHint.exe                       normal start (floating window + tray)
//   HdrHint.exe --from-panel          started by the CEP panel (quits with AME)
//   HdrHint.exe --tray                start hidden in the tray
//   HdrHint.exe --show                bring the running instance to the front
//   HdrHint.exe --install-panel       install/update the CEP panel and exit
//   HdrHint.exe --uninstall-panel
//   HdrHint.exe --process <file> [--preset id] [--lut path|none] [--suffix s]
//   HdrHint.exe --screenshot <png> [--scenario N] [--tab N] [--dark|--light] [--docked-look] [--width W --height H]
//   HdrHint.exe --migrate-from <settings.ini>
//   HdrHint.exe --dock | --undock | --dock-at x,y   (forwarded to the running instance)
//   HdrHint.exe --snap <png>          write the running instance's current frame to a PNG
// ---------------------------------------------------------------------------
#include "ame/DockController.h"
#include "ame/AmeProcess.h"
#include "ame/PanelInstaller.h"
#include "core/Engine.h"
#include "core/HdrPresets.h"
#include "core/IpcProtocol.h"
#include "core/Logger.h"
#include "core/Settings.h"
#include "platform/Handle.h"
#include "platform/KnownFolders.h"
#include "platform/Process.h"
#include "platform/RecycleBin.h"
#include "platform/Registry.h"
#include "platform/SingleInstance.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "platform/Win.h"
#include "ui/app/AppViewModels.h"
#include "ui/app/MockViewModels.h"
#include "ui/gfx/GraphicsDevice.h"
#include "ui/gfx/TextCache.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/screens/AppShell.h"
#include "ui/theme/ThemeManager.h"
#include "ui/window/Messages.h"
#include "ui/window/TrayIcon.h"
#include "ui/window/WindowHost.h"
#include "resource.h"

#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace hh;

// ---- command line -------------------------------------------------------------

/**
 * @brief Parsed command line.
 */
struct Args {
    std::vector<std::wstring> all;
    bool fromPanel = false;
    bool tray = false;
    bool show = false;
    bool installPanel = false;
    bool uninstallPanel = false;
    bool relaunched = false;
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

/// Parses GetCommandLineW into Args.
Args parseArgs() {
    Args args;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) { if (argv[i]) { args.all.emplace_back(argv[i]); } }
        ::LocalFree(argv);
    }
    args.fromPanel = args.has(L"--from-panel");
    args.tray = args.has(L"--tray");
    // Started by Media Encoder's startup script? ExtendScript's File.execute()
    // is a ShellExecute with no way to pass arguments, so the parent process
    // is what tells us: treat it exactly like a panel launch (quiet start in
    // the tray, dock when the panel appears, leave when AME leaves).
    if (!args.fromPanel) {
        const DWORD parent = platform::parentProcessId(::GetCurrentProcessId());
        if (parent != 0 && platform::icontains(platform::processImageName(parent), L"Adobe Media Encoder")) {
            args.fromPanel = true;
            args.tray = true;
        }
    }
    args.show = args.has(L"--show");
    args.installPanel = args.has(L"--install-panel");
    args.uninstallPanel = args.has(L"--uninstall-panel");
    args.relaunched = args.has(L"--relaunched");
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
            // Skip the value of flags that take one.
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

// ---- console helpers for the headless path -----------------------------------

/// Attaches to the parent console (when launched from a terminal) and writes a line.
class ConsoleWriter {
public:
    ConsoleWriter() {
        // A GUI subsystem exe has no console; borrow the parent's when there is one.
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
            attached_ = true;
        }
    }
    ~ConsoleWriter() { if (attached_) { ::FreeConsole(); } }
    void line(const std::wstring& text) const {
        if (out_ == nullptr || out_ == INVALID_HANDLE_VALUE) { return; }
        const std::wstring withNewline = text + L"\r\n";
        DWORD written = 0;
        if (!::WriteConsoleW(out_, withNewline.c_str(), static_cast<DWORD>(withNewline.size()), &written, nullptr)) {
            // Redirected stdout: fall back to a UTF-8 file write.
            const std::string utf8 = platform::toUtf8(withNewline);
            ::WriteFile(out_, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
private:
    HANDLE out_ = nullptr;
    bool attached_ = false;
};

// ---- PNG writer for --screenshot ----------------------------------------------

/// Encodes BGRA pixels to a PNG with WIC.
bool savePng(const std::wstring& path, const std::vector<uint8_t>& bgra, UINT width, UINT height) {
    if (bgra.empty() || width == 0 || height == 0) { return false; }
    platform::ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { return false; }
    platform::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) { return false; }
    platform::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) { return false; }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) { return false; }
    platform::ComPtr<IWICBitmapFrameEncode> frame;
    platform::ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props.Get()))) { return false; }
    if (FAILED(frame->SetSize(width, height))) { return false; }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) { return false; }
    const UINT stride = width * 4;
    if (bgra.size() < static_cast<size_t>(stride) * height) { return false; }
    // The swap chain is premultiplied; un-premultiply for a faithful PNG.
    std::vector<uint8_t> straight(bgra);
    for (size_t i = 0; i + 3 < straight.size(); i += 4) {
        const uint8_t a = straight[i + 3];
        if (a != 0 && a != 255) {
            for (int c = 0; c < 3; ++c) {
                straight[i + c] = static_cast<uint8_t>(std::min(255, (straight[i + c] * 255) / a));
            }
        }
    }
    if (FAILED(frame->WritePixels(height, stride, static_cast<UINT>(straight.size()), straight.data()))) { return false; }
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// ---- the application ------------------------------------------------------------

/**
 * @brief Everything that lives for the duration of the GUI session.
 */
class App {
public:
    App(HINSTANCE hInstance, Args args) : hInstance_(hInstance), args_(std::move(args)) {}

    /// Runs the GUI. Returns the process exit code.
    int run();

private:
    bool initCore();
    bool initWindow(bool screenshotMode);
    void wireEngine();
    void wireWindow();
    void wireDock();
    void wireTray();
    void applyDockState(ame::DockState state);
    void handlePanelMessage(uint32_t conn, const std::string& kind, const std::string& line);
    void handleForwardedArgs(const std::vector<std::wstring>& forwarded);
    void showWindow(bool activate);
    void requestQuit(bool force);
    int runScreenshot();
    void showToast(const ToastRequest& t);
    void applyStartWithWindows();

    HINSTANCE hInstance_;
    Args args_;

    // core
    Settings settings_;
    PresetRegistry presets_;
    std::unique_ptr<Engine> engine_;
    platform::SingleInstance instance_;

    // ui
    ui::GraphicsDevice device_;
    ui::TextCache text_;
    ui::ThemeManager themes_;
    std::unique_ptr<ui::WindowHost> window_;
    std::unique_ptr<ame::DockController> dock_;
    ui::TrayIcon tray_;
    HICON icon_ = nullptr;

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
    bool hiddenToTrayOnce_ = false;
    uint64_t ameMissingSinceMs_ = 0;
    bool ameSeenRunning_ = false;   ///< AME has been alive at least once this session
};

/// Loads settings, presets and the engine (no window yet).
bool App::initCore() {
    settings_.applyMachineDefaults();
    const std::wstring settingsPath = Settings::defaultPath();
    if (auto r = settings_.load(settingsPath); !r) {
        HH_LOG_WARN(L"App", L"settings load: {}", r.error().toString());
    }
    settings_.applyMachineDefaults();

    // One-time import of the Python tool's settings.ini.
    if (!settings_.migrationDone) {
        const std::wstring legacy = !args_.migrateFrom.empty() ? args_.migrateFrom : Settings::findLegacyIni();
        if (!legacy.empty()) {
            auto migrated = settings_.migrateFromLegacy(legacy);
            HH_LOG_INFO(L"App", L"legacy settings {}: {}", legacy, migrated && migrated.value() ? L"migrated" : L"nothing to migrate");
        } else {
            settings_.migrationDone = true;
        }
        settings_.save(settingsPath);
    }
    applyStartWithWindows();

    Logger::instance().setLevel(Logger::parseLevel(settings_.logLevel, LogLevel::Info));
    presets_.loadUser(settings_.expand(settings_.userPresetsFile));
    engine_ = std::make_unique<Engine>(settings_, presets_);
    return true;
}

/// Creates the graphics stack and the main window.
bool App::initWindow(bool screenshotMode) {
    if (!device_.create()) {
        ::MessageBoxW(nullptr, L"Direct3D could not be initialised. HDR Hint needs a working graphics driver.",
                      L"HDR Hint", MB_ICONERROR | MB_OK);
        return false;
    }
    text_.init(device_.dwrite());
    ui::setSharedTextCache(&text_);

    // Theme from settings (or the screenshot flags).
    const std::wstring themeSetting = screenshotMode ? (args_.theme == 1 ? L"dark" : args_.theme == 2 ? L"light" : L"system") : settings_.theme;
    themes_.setMode(platform::iequals(themeSetting, L"dark") ? ui::ThemeMode::Dark
                    : platform::iequals(themeSetting, L"light") ? ui::ThemeMode::Light : ui::ThemeMode::System);
    themes_.setAccentMode(platform::iequals(settings_.accent, L"system") ? ui::AccentMode::System : ui::AccentMode::Blue);
    themes_.setReduceTransparency(settings_.reduceTransparency);

    icon_ = ::LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APP_ICON));

    ui::WindowSpec spec;
    spec.title = L"HDR Hint";
    spec.icon = icon_;
    spec.alwaysOnTop = settings_.floatingAlwaysOnTop && !screenshotMode;
    spec.startHidden = !screenshotMode && (args_.tray || settings_.startMinimized);
    if (args_.width > 0 && args_.height > 0) {
        spec.initialDips = ui::Rect{-1, -1, static_cast<float>(args_.width), static_cast<float>(args_.height)};
    }
    window_ = std::make_unique<ui::WindowHost>(device_, text_, themes_);
    if (!window_->create(spec)) {
        ::MessageBoxW(nullptr, L"The main window could not be created.", L"HDR Hint", MB_ICONERROR | MB_OK);
        return false;
    }
    if (!screenshotMode && !settings_.floatingPlacement.empty()) {
        window_->applyPlacement(settings_.floatingPlacement);
    }

    // Theme changes repaint the whole tree and refresh DWM attributes.
    themes_.addListener([this] {
        if (window_) {
            window_->root().themeChanged();
            window_->refreshBackdrop();
        }
    });
    return true;
}

/// Connects engine callbacks to the UI.
void App::wireEngine() {
    engine_->onToast = [this](const ToastRequest& t) { showToast(t); };
    engine_->onPanelMessage = [this](uint32_t conn, const std::string& kind, const std::string& line) {
        handlePanelMessage(conn, kind, line);
    };
    engine_->onPanelConnection = [this](uint32_t conn, bool connected) {
        if (!connected && dock_) { dock_->onPanelGone(conn); }
    };
}

/// Connects window callbacks.
void App::wireWindow() {
    window_->onCloseRequested = [this] {
        // X hides to the tray when enabled; otherwise it quits.
        if (settings_.minimizeToTray && tray_.added()) {
            window_->hide();
            if (!hiddenToTrayOnce_) {
                hiddenToTrayOnce_ = true;
                tray_.showBalloon(L"HDR Hint keeps watching", L"Exports are still processed. Double-click the tray icon to reopen.");
            }
        } else {
            requestQuit(false);
        }
    };
    window_->onDestroyedByOwner = [this] {
        // AME tore our HWND down (it owned us while docked). The window was recreated floating.
        HH_LOG_WARN(L"App", L"main window destroyed by its owner; recreated floating");
        if (dock_) { dock_->onWindowRecreated(); }
        themes_.setDocked(false, std::nullopt);
        if (shell_) { shell_->setDockedLayout(false); }
        tray_.onTaskbarCreated();
        if (engine_) { engine_->start(window_->hwnd(), ui::WM_HH_ENGINE_EVENTS); }
    };
    window_->onAppMessage = [this](UINT msg, WPARAM wp, LPARAM lp) -> bool {
        if (msg == ui::WM_HH_ENGINE_EVENTS) { if (engine_) { engine_->onEventMessage(); } return true; }
        if (msg == ui::WM_HH_DOCK_TICK) { if (dock_) { dock_->onDockTick(); } return true; }
        if (msg == ui::WM_HH_TRAY) { return tray_.handleMessage(wp, lp); }
        if (msg == ui::TrayIcon::taskbarCreatedMessage()) { tray_.onTaskbarCreated(); return true; }
        return false;
    };
    window_->onSystemSettingsChanged = [this] {
        themes_.refreshFromSystem();
        window_->root().timeline().setReducedMotion(ui::ThemeManager::readReducedMotion());
    };
    window_->onFilesDropped = [this](const std::vector<std::wstring>& files) {
        for (const auto& f : files) { if (engine_) { engine_->addManualFile(f); } }
        showWindow(true);
    };
    window_->onActivate = [this](bool active) { if (shell_) { shell_->setWindowActive(active); } };
    window_->onTick = [this] {
        if (engine_) { engine_->tick(); }
        if (dock_) { dock_->tick(); }
        // Remember the floating placement.
        if (window_ && window_->mode() == ui::WindowMode::Floating) {
            const std::wstring placement = window_->placementString();
            if (!placement.empty() && placement != settings_.floatingPlacement) {
                settings_.floatingPlacement = placement;
            }
        }
        // Leave with Media Encoder when the setting says so, however this
        // instance was started: the user asked for "quit when AME quits", not
        // "quit only if AME happened to launch me".
        //
        // AME must have been seen running at least once, otherwise an app
        // started before Media Encoder (or on a machine without it) would quit
        // itself five seconds in. A running mux always finishes first.
        if (settings_.quitWithAme && !quitting_) {
            if (ame::isAmeRunning()) {
                ameSeenRunning_ = true;
                ameMissingSinceMs_ = 0;
            } else if (ameSeenRunning_) {
                const uint64_t now = platform::nowMonotonicMs();
                if (ameMissingSinceMs_ == 0) { ameMissingSinceMs_ = now; }
                if (now - ameMissingSinceMs_ > 5000 && !(engine_ && engine_->muxRunning())) {
                    HH_LOG_INFO(L"App", L"Media Encoder exited; quitting (quit_with_ame is on)");
                    requestQuit(true);
                }
            }
        }
    };
    window_->onDpiChanged = [this] { if (window_) { window_->root().dpiChanged(); } };
}

/// Creates the docking controller and links it with the theme/shell.
void App::wireDock() {
    dock_ = std::make_unique<ame::DockController>(*window_);
    dock_->setSavedTarget(settings_.dockTarget);
    dock_->setEnabled(!platform::iequals(settings_.dockMode, L"floating"));
    dock_->onStateChanged = [this](ame::DockState s) { applyDockState(s); };
    dock_->onMessage = [this](const std::wstring& text) {
        ToastRequest t;
        t.tone = ToastRequest::Tone::Info;
        t.text = text;
        showToast(t);
    };
}

/// Docked <-> floating side effects for the theme and the shell layout.
void App::applyDockState(ame::DockState state) {
    const bool docked = state == ame::DockState::Docked;
    // Remember the picked panel so the next start glues back on by itself.
    if (dock_) {
        const std::wstring target = dock_->savedTarget();
        if (!target.empty() && target != settings_.dockTarget) {
            settings_.dockTarget = target;
            settings_.save(Settings::defaultPath());
        }
    }
    std::optional<ui::Color> panelBg;
    if (docked && dock_ && dock_->panelBackground()) {
        const COLORREF c = *dock_->panelBackground();
        panelBg = ui::Color(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
    }
    themes_.setDocked(docked, panelBg);
    if (shell_) { shell_->setDockedLayout(docked); }
    if (linkVm_ && linkVm_->onChanged) { linkVm_->onChanged(); }
}

/// Tray icon + its menu.
void App::wireTray() {
    if (!tray_.add(window_->hwnd(), icon_, L"HDR Hint", ui::WM_HH_TRAY)) {
        HH_LOG_WARN(L"App", L"tray icon could not be added");
    }
    enum : int { IdShow = 1, IdDock = 2, IdPause = 3, IdGuide = 4, IdInstall = 5, IdQuit = 6 };
    tray_.setMenuProvider([this]() {
        std::vector<ui::TrayIcon::MenuItem> items;
        items.push_back({IdShow, L"Show HDR Hint"});
        const bool docked = dock_ && dock_->docked();
        items.push_back({IdDock, docked ? L"Undock from Media Encoder" : L"Dock in Media Encoder", false, dock_ != nullptr});
        items.push_back({IdPause, L"Pause automation", !settings_.autoProcess});
        items.push_back({0, L""});
        items.push_back({IdGuide, L"Open export guide"});
        items.push_back({IdInstall, L"Install Media Encoder panel"});
        items.push_back({0, L""});
        items.push_back({IdQuit, L"Quit HDR Hint"});
        return items;
    });
    tray_.onLeftClick = [this] { showWindow(true); };
    tray_.onDoubleClick = [this] { showWindow(true); };
    tray_.onCommand = [this](int id) {
        switch (id) {
        case IdShow: showWindow(true); break;
        case IdDock: if (dock_) { dock_->docked() ? dock_->userUndock() : dock_->userDock(); } break;
        case IdPause: if (engine_) { engine_->setAutoProcess(!settings_.autoProcess); } break;
        case IdGuide: platform::openWithShell(platform::exeDirectory() + L"\\GUIDE.md"); break;
        case IdInstall: {
            auto r = ame::installPanel();
            ToastRequest t;
            t.tone = r ? ToastRequest::Tone::Success : ToastRequest::Tone::Error;
            t.text = r ? L"Panel installed. Restart Media Encoder, then open Window > Extensions > HDR Hint."
                       : L"Panel install failed: " + r.error().toString();
            showToast(t);
            break;
        }
        case IdQuit: requestQuit(false); break;
        default: break;
        }
    };
}

/// Routes non-engine panel messages (dock geometry, lifecycle, commands).
void App::handlePanelMessage(uint32_t conn, const std::string& kind, const std::string& line) {
    auto parsed = ipc::parseLine(line);
    if (!parsed) { return; }
    const ipc::json& msg = *parsed;

    if (kind == "hello") {
        const DWORD pid = static_cast<DWORD>(ipc::num(msg, "pid", 0.0));
        std::optional<COLORREF> bg;
        if (msg.contains("skin") && msg["skin"].is_object()) {
            const std::string hex = ipc::str(msg["skin"], "panelBg");
            if (hex.size() == 7 && hex[0] == '#') {
                const unsigned long v = std::strtoul(hex.c_str() + 1, nullptr, 16);
                bg = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
            }
        }
        if (dock_) { dock_->onPanelHello(conn, pid, bg); }
        return;
    }
    if (kind == "panelBounds") {
        if (dock_) {
            dock_->onPanelBounds(conn, ipc::num(msg, "x"), ipc::num(msg, "y"), ipc::num(msg, "w"), ipc::num(msg, "h"),
                                 ipc::num(msg, "dpr", 1.0), ipc::boolean(msg, "visible", true));
        }
        return;
    }
    if (kind == "command") {
        const std::string cmd = ipc::str(msg, "cmd");
        if (cmd == "show") { showWindow(true); }
        else if (cmd == "dock" && dock_) { dock_->userDock(); }
        else if (cmd == "undock" && dock_) { dock_->userUndock(); }
        return;
    }
    if (kind == "panelState") {
        if (ipc::str(msg, "state") == "closing" && dock_) { dock_->onPanelGone(conn); }
        return;
    }
    // Lifecycle events the engine forwards with an "ame:" prefix.
    if (kind == "ame:appBeforeQuit") { if (dock_) { dock_->onAmeBeforeQuit(); } return; }
    if (kind == "ame:extensionUnloaded") { if (dock_) { dock_->onPanelGone(conn); } return; }
    if (kind == "ame:workspaceChanged") { if (dock_) { dock_->onWorkspaceChanged(); } return; }
    if (kind == "ame:visibility") { if (dock_) { dock_->onPanelVisibility(ipc::boolean(msg, "visible", true)); } return; }
    if (kind == "ame:themeChanged") {
        const std::string hex = ipc::str(msg, "panelBg");
        if (hex.size() == 7 && hex[0] == '#' && dock_) {
            const unsigned long v = std::strtoul(hex.c_str() + 1, nullptr, 16);
            dock_->onPanelThemeChanged(RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF));
            if (dock_->docked()) { applyDockState(ame::DockState::Docked); }
        }
        return;
    }
}

/// A second instance forwarded its arguments.
void App::handleForwardedArgs(const std::vector<std::wstring>& forwarded) {
    bool show = forwarded.empty();
    for (size_t i = 0; i < forwarded.size(); ++i) {
        const std::wstring& a = forwarded[i];
        if (platform::iequals(a, L"--show") || platform::iequals(a, L"--from-panel")) { show = true; continue; }
        // Docking commands (tray, panel and scripts share these).
        if (platform::iequals(a, L"--dock")) { if (dock_) { dock_->userDock(); } continue; }
        if (platform::iequals(a, L"--undock")) { if (dock_) { dock_->userUndock(); } continue; }
        if (platform::iequals(a, L"--dock-at") && i + 1 < forwarded.size()) {
            // "--dock-at x,y": dock onto the AME panel under that screen point.
            const std::vector<std::wstring> xy = platform::split(forwarded[++i], L',');
            if (xy.size() == 2 && dock_) {
                const auto x = platform::parseInt(xy[0]);
                const auto y = platform::parseInt(xy[1]);
                if (x && y) { dock_->dockAtScreenPoint(POINT{static_cast<LONG>(*x), static_cast<LONG>(*y)}); }
            }
            continue;
        }
        if (platform::iequals(a, L"--snap") && i + 1 < forwarded.size()) {
            // "--snap <png>": dumps the live back buffer (docked or floating) so the
            // real UI can be reviewed without a screen grab of AME.
            const std::wstring out = forwarded[++i];
            std::vector<uint8_t> bgra;
            UINT w = 0, h = 0;
            const bool captured = window_ && window_->captureFrame(bgra, w, h);
            if (captured) {
                // A floating window is drawn over Mica, so its back buffer is
                // translucent (premultiplied). Composite over the theme's
                // opaque window colour so the PNG reads like the screen does.
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
            if (captured && savePng(out, bgra, w, h)) {
                HH_LOG_INFO(L"App", L"--snap wrote {} ({}x{})", out, w, h);
            } else {
                HH_LOG_WARN(L"App", L"--snap failed for {}", out);
            }
            continue;
        }
        if (platform::iequals(a, L"--install-panel")) {
            auto r = ame::installPanel();
            ToastRequest t;
            t.tone = r ? ToastRequest::Tone::Success : ToastRequest::Tone::Error;
            t.text = r ? L"Panel installed." : L"Panel install failed: " + r.error().toString();
            showToast(t);
            continue;
        }
        if (a.rfind(L"--", 0) == 0) { continue; }
        if (platform::isFile(a) && engine_) { engine_->addManualFile(a); show = true; }
    }
    if (show) { showWindow(true); }
}

/**
 * @brief Mirrors Settings > "Start with Windows" into the per-user Run key.
 *
 * The entry launches the exe with --tray so nothing pops up at logon; the
 * dock controller then attaches to Media Encoder the moment it appears.
 * Any failure is logged, never fatal - the app works the same without it.
 */
void App::applyStartWithWindows() {
    constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kRunValue = L"HdrHint";
    if (!settings_.startWithWindows) {
        if (auto r = platform::regDeleteValue(HKEY_CURRENT_USER, kRunKey, kRunValue); !r) {
            HH_LOG_WARN(L"App", L"start with Windows: could not remove Run entry: {}", r.error().toString());
        }
        return;
    }
    const std::wstring exe = platform::exePath();
    if (exe.empty()) {
        HH_LOG_WARN(L"App", L"start with Windows: exe path unknown; Run entry not written");
        return;
    }
    // Only touch the registry when the command line actually differs (the
    // exe may have moved since the last run).
    const std::wstring command = L"\"" + exe + L"\" --tray";
    const auto existing = platform::regReadString(HKEY_CURRENT_USER, kRunKey, kRunValue);
    if (existing && existing.value() == command) {
        return;
    }
    if (auto r = platform::regWriteString(HKEY_CURRENT_USER, kRunKey, kRunValue, command); !r) {
        HH_LOG_WARN(L"App", L"start with Windows: could not write Run entry: {}", r.error().toString());
    } else {
        HH_LOG_INFO(L"App", L"start with Windows: Run entry = {}", command);
    }
}

/// Shows the window (floating: restore + activate; docked: just make visible).
void App::showWindow(bool activate) {
    if (!window_) { return; }
    if (window_->mode() == ui::WindowMode::Docked) {
        window_->setDockVisible(true);
        return;
    }
    window_->show(activate);
    if (activate) { window_->bringToFront(); }
}

/// Toast in the window, or a tray balloon when the window is hidden.
void App::showToast(const ToastRequest& t) {
    ui::ToastSpec spec;
    spec.text = t.text;
    spec.tone = t.tone == ToastRequest::Tone::Success ? ui::ToastTone::Success
              : t.tone == ToastRequest::Tone::Warning ? ui::ToastTone::Warning
              : t.tone == ToastRequest::Tone::Error ? ui::ToastTone::Error : ui::ToastTone::Info;
    if (!t.actionLabel.empty() && t.jobId != 0) {
        spec.actionLabel = t.actionLabel;
        const JobId id = t.jobId;
        const bool recycle = platform::iequals(t.actionLabel, L"Recycle");
        spec.onAction = [this, id, recycle] {
            if (!engine_) { return; }
            if (recycle) { engine_->recycleJob(id); } else { engine_->revealJob(id, true); }
        };
        spec.durationSec = recycle ? 0.0 : 6.0;
    }
    if (shell_ && window_ && window_->visible()) {
        shell_->showToast(std::move(spec));
    } else if (tray_.added() && settings_.toastOnDone) {
        tray_.showBalloon(L"HDR Hint", t.text);
    }
}

/// Orderly shutdown. @p force skips the "mux running" question.
void App::requestQuit(bool force) {
    if (quitting_) { return; }
    if (!force && engine_ && engine_->muxRunning()) {
        const int answer = ::MessageBoxW(window_ ? window_->hwnd() : nullptr,
                                         L"mkvmerge is still writing a file.\n\nQuit now and cancel it?",
                                         L"HDR Hint", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) { return; }
    }
    quitting_ = true;
    if (engine_) { engine_->stop(false); }
    settings_.save(Settings::defaultPath());
    tray_.remove();
    if (window_) { window_->quit(0); }
}

/// Renders the mock UI and writes a PNG (used to review the design without AME).
int App::runScreenshot() {
    mockQueue_ = std::make_unique<ui::MockQueueViewModel>();
    mockSettings_ = std::make_unique<ui::MockSettingsViewModel>();
    mockLink_ = std::make_unique<ui::MockLinkViewModel>();
    mockGuide_ = std::make_unique<ui::MockGuideViewModel>();
    ui::mockApplyState(*mockQueue_, args_.scenario);

    auto shell = std::make_unique<ui::AppShell>(*mockQueue_, *mockSettings_, *mockLink_, *mockGuide_);
    shell_ = shell.get();
    window_->root().setContent(std::move(shell));
    shell_->setDockedLayout(args_.dockedLook);
    themes_.setDocked(args_.dockedLook, std::nullopt);
    // A PNG has no Mica behind it: paint the opaque window background so the
    // capture looks like the window does on screen.
    themes_.setReduceTransparency(true);
    window_->refreshBackdrop();
    shell_->setTab(static_cast<ui::AppTab>(std::clamp(args_.tab, 0, 2)), false);
    window_->show(false);

    // Let springs settle: pump the loop for ~700 ms rendering frames.
    const uint64_t start = platform::nowMonotonicMs();
    while (platform::nowMonotonicMs() - start < 700) {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
        window_->root().tickAnimations();
        window_->renderFrame();
        ::Sleep(16);
    }
    std::vector<uint8_t> bgra;
    UINT w = 0, h = 0;
    if (!window_->captureFrame(bgra, w, h)) {
        HH_LOG_ERROR(L"App", L"screenshot: frame capture failed");
        return 3;
    }
    if (!savePng(args_.screenshot, bgra, w, h)) {
        HH_LOG_ERROR(L"App", L"screenshot: PNG write failed for {}", args_.screenshot);
        return 4;
    }
    HH_LOG_INFO(L"App", L"screenshot written: {} ({}x{})", args_.screenshot, w, h);
    return 0;
}

int App::run() {
    if (!initCore()) { return 1; }

    // ---- headless one-shot ------------------------------------------------------
    if (!args_.processFile.empty()) {
        ConsoleWriter console;
        if (!args_.suffix.empty()) { settings_.suffix = args_.suffix; }
        int last = -1;
        auto result = engine_->processOneShot(args_.processFile, args_.preset, args_.lut, [&](float p) {
            const int percent = static_cast<int>(p * 100.0f + 0.5f);
            if (percent / 10 != last / 10) { last = percent; console.line(L"muxing " + std::to_wstring(percent) + L"%"); }
        });
        if (!result) {
            console.line(L"error: " + result.error().toString());
            return 4;
        }
        console.line(L"created: " + result.value());
        return 0;
    }

    // ---- panel install / uninstall ---------------------------------------------
    if (args_.installPanel || args_.uninstallPanel) {
        auto r = args_.installPanel ? ame::installPanel() : ame::uninstallPanel();
        const std::wstring text = r ? (args_.installPanel
                                           ? L"The HDR Hint panel is installed.\n\nRestart Adobe Media Encoder, then open Window > Extensions > HDR Hint and drag the tab above the Queue."
                                           : L"The HDR Hint panel was removed.")
                                    : L"Failed: " + r.error().toString();
        ::MessageBoxW(nullptr, text.c_str(), L"HDR Hint", r ? MB_ICONINFORMATION : MB_ICONERROR);
        return r ? 0 : 2;
    }

    const bool screenshotMode = !args_.screenshot.empty();

    // ---- single instance --------------------------------------------------------
    if (!screenshotMode) {
        if (!instance_.acquire()) {
            std::vector<std::wstring> forward = args_.all;
            if (forward.empty()) { forward.emplace_back(L"--show"); }
            platform::SingleInstance::forwardToPrimary(forward);
            return 0;
        }
    }

    if (!initWindow(screenshotMode)) { return 1; }
    if (screenshotMode) { return runScreenshot(); }

    // ---- real view-models + shell ----------------------------------------------
    wireEngine();
    wireWindow();
    wireDock();

    queueVm_ = std::make_unique<ui::AppQueueViewModel>(*engine_, window_->hwnd());
    settingsVm_ = std::make_unique<ui::AppSettingsViewModel>(*engine_, settings_, themes_, window_->hwnd());
    linkVm_ = std::make_unique<ui::AppLinkViewModel>(*engine_, *dock_);
    guideVm_ = std::make_unique<ui::AppGuideViewModel>(window_->hwnd());
    queueVm_->bind();
    linkVm_->bind();
    settingsVm_->onAppearanceChanged = [this] { window_->root().themeChanged(); };
    settingsVm_->onDockChanged = [this] {
        dock_->setEnabled(!platform::iequals(settings_.dockMode, L"floating"));
        if (dock_->enabled()) { dock_->userDock(); }
    };
    settingsVm_->onWindowBehaviourChanged = [this] {
        window_->setAlwaysOnTop(settings_.floatingAlwaysOnTop);
        applyStartWithWindows();
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
    shell_->onMaximize = [this] { window_->toggleMaximize(); shell_->setMaximized(!window_->minimized() && ::IsZoomed(window_->hwnd())); };
    shell_->onClose = [this] { if (window_->onCloseRequested) { window_->onCloseRequested(); } };
    window_->root().setContent(std::move(shell));
    window_->root().timeline().setReducedMotion(ui::ThemeManager::readReducedMotion());

    wireTray();
    instance_.registerReceiver([this](const std::vector<std::wstring>& forwarded) { handleForwardedArgs(forwarded); });

    // Start the engine (workers + IPC) now that the window exists for kicks.
    if (auto r = engine_->start(window_->hwnd(), ui::WM_HH_ENGINE_EVENTS); !r) {
        HH_LOG_ERROR(L"App", L"engine start: {}", r.error().toString());
        ToastRequest t;
        t.tone = ToastRequest::Tone::Error;
        t.text = L"Engine failed to start: " + r.error().toString();
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
            t.text = L"Install the Media Encoder panel from Settings to dock HDR Hint inside AME.";
            showToast(t);
        }
    }

    // Media Encoder started us: show the window unless the user asked for a
    // quiet tray start. It floats until the HDR Hint panel is opened, then
    // docks onto it; --tray on its own (a logon start) stays hidden.
    const bool ameLaunchedUs = args_.fromPanel && settings_.showOnAmeLaunch && !settings_.startMinimized;
    if ((!args_.tray && !settings_.startMinimized) || ameLaunchedUs) {
        showWindow(!args_.fromPanel);
    }

    // No tick override here: wireWindow() already installed the 1 Hz tick
    // (engine housekeeping, dock re-search, placement memory). Passing a
    // lambda - even an empty one - would replace it.
    const int code = window_->runLoop(nullptr);

    // Orderly teardown (in case quit came from WM_QUIT without requestQuit).
    if (!quitting_) {
        quitting_ = true;
        if (engine_) { engine_->stop(false); }
        settings_.save(Settings::defaultPath());
        tray_.remove();
    }
    shell_ = nullptr;
    dock_.reset();
    window_.reset();
    ui::setSharedTextCache(nullptr);
    text_.shutdown();
    device_.destroy();
    return code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    Args args = parseArgs();

    // Spawned by the CEP panel inside a kill-on-close job? Escape it first.
    if (!args.relaunched && platform::relaunchOutsideKillOnCloseJob(L"--relaunched")) {
        return 0;
    }

    platform::ScopedCoInit com;
    Logger::instance().open(platform::appLocalDataFolder() + L"\\logs", LogLevel::Info, 2048, 5);
    HH_LOG_INFO(L"App", L"HDR Hint 1.0.0 starting ({} args)", args.all.size());

    App app(hInstance, std::move(args));
    const int code = app.run();
    HH_LOG_INFO(L"App", L"exit {}", code);
    Logger::instance().close();
    return code;
}
