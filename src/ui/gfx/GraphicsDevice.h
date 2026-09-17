// ---------------------------------------------------------------------------
// GraphicsDevice.h - the process-wide D3D11 / D2D / DirectWrite / DComp stack.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Handle.h"
#include "platform/Win.h"

#include <d2d1_2.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>

#include <cstdint>

namespace hh::ui {

using platform::ComPtr;

/**
 * @brief Owns the device objects; everything else borrows raw pointers.
 *
 * Device loss: markLost() is called by whoever observes DXGI_ERROR_DEVICE_*
 * or D2DERR_RECREATE_TARGET; the window then calls recreate() and rebinds
 * its surface. generation() lets caches know their resources are stale.
 */
class GraphicsDevice {
public:
    GraphicsDevice() = default;
    ~GraphicsDevice();
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    /// Creates D3D11 (hardware, WARP fallback; env HH_FORCE_WARP=1 forces WARP), D2D, DWrite, DComp.
    bool create();
    /// Releases everything.
    void destroy();
    /// Releases and re-creates; increments generation().
    bool recreate();

    [[nodiscard]] bool isLost() const noexcept { return lost_; }
    void markLost() noexcept { lost_ = true; }
    [[nodiscard]] uint32_t generation() const noexcept { return generation_; }
    [[nodiscard]] bool valid() const noexcept { return d2dDevice_ != nullptr && !lost_; }
    [[nodiscard]] bool isWarp() const noexcept { return warp_; }

    [[nodiscard]] ID3D11Device* d3d() const noexcept { return d3d_.Get(); }
    [[nodiscard]] IDXGIDevice1* dxgi() const noexcept { return dxgi_.Get(); }
    [[nodiscard]] IDXGIFactory2* dxgiFactory() const noexcept { return dxgiFactory_.Get(); }
    [[nodiscard]] ID2D1Factory2* d2dFactory() const noexcept { return d2dFactory_.Get(); }
    [[nodiscard]] ID2D1Device1* d2dDevice() const noexcept { return d2dDevice_.Get(); }
    [[nodiscard]] IDWriteFactory3* dwrite() const noexcept { return dwrite_.Get(); }
    [[nodiscard]] IDCompositionDesktopDevice* dcomp() const noexcept { return dcomp_.Get(); }

private:
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice1> dxgi_;
    ComPtr<IDXGIFactory2> dxgiFactory_;
    ComPtr<ID2D1Factory2> d2dFactory_;
    ComPtr<ID2D1Device1> d2dDevice_;
    ComPtr<IDWriteFactory3> dwrite_;
    ComPtr<IDCompositionDesktopDevice> dcomp_;
    bool lost_ = false;
    bool warp_ = false;
    uint32_t generation_ = 1;
};

} // namespace hh::ui
