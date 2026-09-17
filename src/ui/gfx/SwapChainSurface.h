// ---------------------------------------------------------------------------
// SwapChainSurface.h - a composition swap chain bound to one HWND.
//
// The window must be created with WS_EX_NOREDIRECTIONBITMAP so transparent
// pixels show the DWM backdrop (Mica) instead of black.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"
#include "ui/gfx/GraphicsDevice.h"

#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi1_3.h>

namespace hh::ui {

class SwapChainSurface {
public:
    SwapChainSurface() = default;
    ~SwapChainSurface();
    SwapChainSurface(const SwapChainSurface&) = delete;
    SwapChainSurface& operator=(const SwapChainSurface&) = delete;

    /// Creates the swap chain (flip-sequential, premultiplied alpha, waitable) + DComp target/visual.
    bool bind(HWND hwnd, GraphicsDevice& device, UINT pxWidth, UINT pxHeight);
    /// Releases everything (window recreation / device loss).
    void unbind();
    [[nodiscard]] bool bound() const noexcept { return swapChain_ != nullptr; }

    /// Resizes the buffers when the size actually changed.
    bool resize(UINT pxWidth, UINT pxHeight);
    [[nodiscard]] UINT pxWidth() const noexcept { return width_; }
    [[nodiscard]] UINT pxHeight() const noexcept { return height_; }

    /**
     * @brief Sets the target bitmap, DPI and starts drawing.
     * @param clear  colour the frame starts from (transparent for Mica)
     * @return the device context to draw with, or nullptr when unusable
     */
    ID2D1DeviceContext1* beginFrame(float dpi, const Color& clear);
    /// EndDraw + Present(1,0). Returns the HRESULT (device-lost codes are reported, not hidden).
    HRESULT endFrame();

    /// Frame-latency waitable object (signalled when a present slot is free).
    [[nodiscard]] HANDLE frameLatencyWaitable() const noexcept { return waitable_; }
    /// The D2D context (valid between bind and unbind).
    [[nodiscard]] ID2D1DeviceContext1* context() const noexcept { return context_.Get(); }
    /// Copies the current back buffer into a new bitmap (theme cross-fades). May return null.
    ComPtr<ID2D1Bitmap1> snapshot();
    /// Reads the back buffer back to CPU memory as BGRA (for --screenshot). Row stride = width*4.
    bool readback(std::vector<uint8_t>& bgra, UINT& width, UINT& height);

private:
    bool createTargetBitmap();

    GraphicsDevice* device_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1DeviceContext1> context_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual2> visual_;
    HANDLE waitable_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    bool drawing_ = false;
};

} // namespace hh::ui
