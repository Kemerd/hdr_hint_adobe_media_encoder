// ---------------------------------------------------------------------------
// DockController.h - keeps the HdrHint window glued to the CEP panel in AME.
// ---------------------------------------------------------------------------
#pragma once

#include "ame/DockControl.h"
#include "ame/PanelWindowLocator.h"
#include "platform/Win.h"
#include "ui/window/WindowHost.h"

#include <functional>
#include <optional>
#include <string>

namespace hh::ame {

/**
 * @brief Dock/undock state machine + WinEvent tracking.
 *
 * All methods run on the UI thread. WinEvent callbacks only post
 * WM_HH_DOCK_TICK to the main window; the window forwards to onDockTick().
 */
class DockController final : public IDockControl {
public:
    explicit DockController(ui::WindowHost& window);
    ~DockController();
    DockController(const DockController&) = delete;
    DockController& operator=(const DockController&) = delete;

    /// Master switch (Settings > Dock inside Media Encoder).
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] DockState state() const noexcept { return state_; }
    [[nodiscard]] bool docked() const noexcept override { return state_ == DockState::Docked; }
    [[nodiscard]] bool supported() const noexcept override { return true; }

    // ---- panel bridge inputs ---------------------------------------------
    /// "hello" from the panel: renderer pid + skin colour.
    void onPanelHello(uint32_t connectionId, DWORD rendererPid, std::optional<COLORREF> panelBackground);
    /// "panelBounds" (CSS px + dpr) - fallback geometry and visibility hint.
    void onPanelBounds(uint32_t connectionId, double x, double y, double w, double h, double dpr, bool visible);
    void onPanelVisibility(bool visible);
    void onPanelThemeChanged(COLORREF panelBackground);
    /// Panel closed / pipe dropped / ExtensionUnloaded.
    void onPanelGone(uint32_t connectionId);
    /// AME is quitting: un-own immediately.
    void onAmeBeforeQuit();
    /// Workspace changed: re-locate the panel windows.
    void onWorkspaceChanged();
    /// User pressed Dock / Undock (panel or app).
    void userDock() override;
    void userUndock() override;
    [[nodiscard]] bool userUndocked() const noexcept { return userUndocked_; }

    // ---- pick-a-panel docking (no CEP needed) --------------------------------
    /**
     * @brief Pick mode: the next left click inside Media Encoder chooses the
     *        panel HDR Hint covers. Esc (or a click elsewhere) cancels.
     */
    void beginPick();
    void cancelPick(const std::wstring& reason);
    [[nodiscard]] bool picking() const noexcept { return state_ == DockState::Picking; }
    /// Docks onto any window that belongs to Media Encoder (a panel under the cursor).
    bool dockToWindow(HWND target);
    /// Docks onto the AME panel under a screen point (scripting / tests).
    bool dockAtScreenPoint(POINT pt);
    /// Where the last pick landed, "l,t,w,h" relative to AME's client area (empty = none).
    [[nodiscard]] std::wstring savedTarget() const { return savedTarget_; }
    void setSavedTarget(const std::wstring& signature) { savedTarget_ = signature; }
    /// Re-docks onto the saved panel when AME is running and the panel can be found.
    bool redockSaved();
    /// Default target, in order: the Window > Extensions > HDR Hint frame, then the saved panel.
    bool dockToDefaultTarget();

    // ---- window inputs -------------------------------------------------------
    /// WM_HH_DOCK_TICK from the WinEvent hooks (coalesced).
    void onDockTick();
    /// 1 Hz housekeeping (re-search, occlusion re-check).
    void tick();
    /// The main HWND was destroyed by its owner and recreated in floating mode.
    void onWindowRecreated();

    /// Panel background as a UI colour (from hello/theme events).
    [[nodiscard]] std::optional<COLORREF> panelBackground() const noexcept { return panelBackground_; }
    std::function<void(const std::wstring&)> onMessage;   ///< toasts ("Docked inside Media Encoder")

private:
    static void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD thread, DWORD time);
    static void CALLBACK pickTimerProc(HWND, UINT, UINT_PTR, DWORD);
    void installHooks();
    void removeHooks();
    void tryDock();
    void applyGeometry(bool force);
    void setState(DockState s);
    [[nodiscard]] bool occluded(const RECT& rect) const;
    [[nodiscard]] std::optional<RECT> fallbackRect() const;
    void pollPick();
    [[nodiscard]] HWND resolvePickTarget(POINT pt) const;
    [[nodiscard]] std::wstring signatureFor(HWND target, HWND root) const;
    void stopPickTimer();

    ui::WindowHost& window_;
    bool enabled_ = true;
    bool userUndocked_ = false;
    DockState state_ = DockState::Undocked;
    uint32_t connectionId_ = 0;
    DWORD rendererPid_ = 0;
    std::optional<PanelWindows> panel_;
    std::optional<COLORREF> panelBackground_;
    struct { double x = 0, y = 0, w = 0, h = 0, dpr = 1; bool visible = true; bool valid = false; } jsBounds_;
    RECT lastRect_{};
    bool lastVisible_ = false;
    HWINEVENTHOOK hooks_[3]{};
    uint64_t lastSearchMs_ = 0;
    bool elevationMismatch_ = false;

    // pick-a-panel state
    std::wstring savedTarget_;        ///< "l,t,w,h" relative to the AME client area
    UINT_PTR pickTimer_ = 0;
    uint64_t pickStartedMs_ = 0;
    bool pickButtonWasDown_ = false;
    bool pickedDock_ = false;         ///< the current dock came from a pick, not from the CEP panel
    uint64_t lastRedockMs_ = 0;
};

} // namespace hh::ame
