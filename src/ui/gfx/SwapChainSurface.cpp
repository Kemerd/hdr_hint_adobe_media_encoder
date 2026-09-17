// ---------------------------------------------------------------------------
// SwapChainSurface.cpp - composition swap chain + DComp visual for one HWND.
// ---------------------------------------------------------------------------
#include "ui/gfx/SwapChainSurface.h"

#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Surface";

// The swap chain is created and resized with the same flag set; DXGI rejects
// a ResizeBuffers whose flags differ from creation.
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

/**
 * @brief Premultiplied BGRA at 96 dpi: the target's DPI is set per frame via SetDpi.
 */
D2D1_BITMAP_PROPERTIES1 bitmapProps(D2D1_BITMAP_OPTIONS options) {
    return D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                   96.0f, 96.0f);
}

} // namespace

/**
 * @brief Releases everything on destruction.
 */
SwapChainSurface::~SwapChainSurface() {
    unbind();
}

/**
 * @brief Creates the swap chain, D2D context, target bitmap and DComp tree.
 */
bool SwapChainSurface::bind(HWND hwnd, GraphicsDevice& device, UINT pxWidth, UINT pxHeight) {
    // A rebind on an already bound surface starts clean.
    unbind();

    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        HH_LOG_ERROR(kLog, L"bind: invalid window handle");
        return false;
    }
    if (!device.valid() || device.dxgiFactory() == nullptr || device.dxgi() == nullptr ||
        device.d2dDevice() == nullptr || device.dcomp() == nullptr) {
        HH_LOG_ERROR(kLog, L"bind: graphics device is not ready");
        return false;
    }
    device_ = &device;
    hwnd_ = hwnd;
    width_ = std::max(1u, pxWidth);
    height_ = std::max(1u, pxHeight);

    // ---- swap chain ----------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;                 // the only value composition allows
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;      // transparent pixels show the DWM backdrop
    sd.Flags = kSwapChainFlags;

    HRESULT hr = device.dxgiFactory()->CreateSwapChainForComposition(device.dxgi(), &sd, nullptr, swapChain_.GetAddressOf());
    if (FAILED(hr) || !swapChain_) {
        HH_LOG_ERROR(kLog, L"CreateSwapChainForComposition failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }

    // Latency must be set on the swap chain (not the DXGI device) for a
    // waitable swap chain, and before the waitable handle is fetched.
    ComPtr<IDXGISwapChain2> swapChain2;
    hr = swapChain_.As(&swapChain2);
    if (FAILED(hr) || !swapChain2) {
        HH_LOG_ERROR(kLog, L"IDXGISwapChain2 unavailable (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }
    hr = swapChain2->SetMaximumFrameLatency(1);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"SetMaximumFrameLatency failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
    }
    waitable_ = swapChain2->GetFrameLatencyWaitableObject();
    if (waitable_ == nullptr) {
        HH_LOG_WARN(kLog, L"GetFrameLatencyWaitableObject returned null; the pacer will fall back to timers");
    }

    // ---- Direct2D context ----------------------------------------------------
    ComPtr<ID2D1DeviceContext> baseContext;
    hr = device.d2dDevice()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, baseContext.GetAddressOf());
    if (FAILED(hr) || !baseContext) {
        HH_LOG_ERROR(kLog, L"CreateDeviceContext failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }
    hr = baseContext.As(&context_);
    if (FAILED(hr) || !context_) {
        HH_LOG_ERROR(kLog, L"ID2D1DeviceContext1 unavailable (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }
    if (!createTargetBitmap()) {
        unbind();
        return false;
    }

    // ---- DirectComposition ---------------------------------------------------
    IDCompositionDesktopDevice* dcomp = device.dcomp();
    hr = dcomp->CreateTargetForHwnd(hwnd_, TRUE, dcompTarget_.GetAddressOf());
    if (FAILED(hr) || !dcompTarget_) {
        HH_LOG_ERROR(kLog, L"CreateTargetForHwnd failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }
    hr = dcomp->CreateVisual(visual_.GetAddressOf());
    if (FAILED(hr) || !visual_) {
        HH_LOG_ERROR(kLog, L"IDCompositionDesktopDevice::CreateVisual failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }
    hr = visual_->SetContent(swapChain_.Get());
    if (FAILED(hr)) {
        HH_LOG_ERROR(kLog, L"IDCompositionVisual::SetContent failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }
    hr = dcompTarget_->SetRoot(visual_.Get());
    if (FAILED(hr)) {
        HH_LOG_ERROR(kLog, L"IDCompositionTarget::SetRoot failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }

    // One commit wires the tree up; presents update the visual on their own.
    hr = dcomp->Commit();
    if (FAILED(hr)) {
        HH_LOG_ERROR(kLog, L"IDCompositionDevice::Commit failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        unbind();
        return false;
    }

    HH_LOG_DEBUG(kLog, L"bound to hwnd 0x{:X} at {}x{} px", reinterpret_cast<uintptr_t>(hwnd_), width_, height_);
    return true;
}

/**
 * @brief Releases everything in reverse order of creation.
 */
void SwapChainSurface::unbind() {
    // Never leave the context inside BeginDraw; EndDraw's result is moot here.
    if (drawing_ && context_) {
        context_->EndDraw();
        drawing_ = false;
    }

    // Detach the visual tree before the swap chain goes away.
    if (dcompTarget_) {
        dcompTarget_->SetRoot(nullptr);
    }
    if (visual_) {
        visual_->SetContent(nullptr);
    }
    visual_.Reset();
    dcompTarget_.Reset();

    // The target bitmap references the back buffer; drop it before the chain.
    if (context_) {
        context_->SetTarget(nullptr);
    }
    target_.Reset();
    context_.Reset();

    // The waitable is a real kernel handle owned by us.
    if (waitable_ != nullptr) {
        ::CloseHandle(waitable_);
        waitable_ = nullptr;
    }
    swapChain_.Reset();

    hwnd_ = nullptr;
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    drawing_ = false;
}

/**
 * @brief Wraps back buffer 0 in a D2D target bitmap.
 */
bool SwapChainSurface::createTargetBitmap() {
    if (!swapChain_ || !context_) {
        HH_LOG_ERROR(kLog, L"createTargetBitmap: surface is not bound");
        return false;
    }
    ComPtr<IDXGISurface> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr) || !backBuffer) {
        HH_LOG_ERROR(kLog, L"IDXGISwapChain::GetBuffer failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    const D2D1_BITMAP_PROPERTIES1 props = bitmapProps(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    target_.Reset();
    hr = context_->CreateBitmapFromDxgiSurface(backBuffer.Get(), &props, target_.GetAddressOf());
    if (FAILED(hr) || !target_) {
        HH_LOG_ERROR(kLog, L"CreateBitmapFromDxgiSurface failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    return true;
}

/**
 * @brief Resizes the buffers (no-op when the size is unchanged).
 */
bool SwapChainSurface::resize(UINT pxWidth, UINT pxHeight) {
    if (!bound() || !context_) {
        HH_LOG_WARN(kLog, L"resize: surface is not bound");
        return false;
    }
    const UINT w = std::max(1u, pxWidth);
    const UINT h = std::max(1u, pxHeight);
    if (w == width_ && h == height_) {
        return true;
    }
    if (drawing_) {
        HH_LOG_WARN(kLog, L"resize: called between beginFrame/endFrame; ignored");
        return false;
    }

    // Every reference to the old back buffer must be gone before ResizeBuffers.
    context_->SetTarget(nullptr);
    target_.Reset();

    HRESULT hr = swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (FAILED(hr)) {
        HH_LOG_ERROR(kLog, L"ResizeBuffers({}x{}) failed (hr=0x{:08X})", w, h, static_cast<uint32_t>(hr));
        if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && device_ != nullptr) {
            device_->markLost();
        }
        return false;
    }
    width_ = w;
    height_ = h;
    return createTargetBitmap();
}

/**
 * @brief Points the context at the back buffer and opens the frame.
 */
ID2D1DeviceContext1* SwapChainSurface::beginFrame(float dpi, const Color& clear) {
    if (!context_ || !target_) {
        HH_LOG_WARN(kLog, L"beginFrame: no target");
        return nullptr;
    }
    if (drawing_) {
        HH_LOG_WARN(kLog, L"beginFrame: frame already open");
        return context_.Get();
    }
    if (device_ != nullptr && device_->isLost()) {
        return nullptr;
    }

    // DPI drives the dip -> pixel mapping for every draw call this frame.
    const float effectiveDpi = (dpi > 0.0f) ? dpi : 96.0f;
    context_->SetTarget(target_.Get());
    context_->SetDpi(effectiveDpi, effectiveDpi);
    context_->SetUnitMode(D2D1_UNIT_MODE_DIPS);

    // Grayscale AA: ClearType fringes look wrong over a translucent backdrop.
    context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context_->SetTransform(D2D1::Matrix3x2F::Identity());
    context_->BeginDraw();
    context_->Clear(clear.toD2D());
    drawing_ = true;
    return context_.Get();
}

/**
 * @brief Closes the frame and presents; device-lost codes mark the device.
 */
HRESULT SwapChainSurface::endFrame() {
    if (!context_ || !swapChain_) {
        return E_NOT_VALID_STATE;
    }
    if (!drawing_) {
        HH_LOG_WARN(kLog, L"endFrame: no frame is open");
        return E_NOT_VALID_STATE;
    }
    drawing_ = false;

    HRESULT hr = context_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        HH_LOG_WARN(kLog, L"EndDraw reported D2DERR_RECREATE_TARGET");
        if (device_ != nullptr) {
            device_->markLost();
        }
        return hr;
    }
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"EndDraw failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return hr;
    }

    // Present(1, 0): vsync-paced, no tearing; the waitable handle throttles us.
    hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HH_LOG_WARN(kLog, L"Present reported device loss (hr=0x{:08X})", static_cast<uint32_t>(hr));
        if (device_ != nullptr) {
            device_->markLost();
        }
    } else if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"Present failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
    }
    return hr;
}

/**
 * @brief Copies the back buffer into a drawable bitmap (theme cross-fades).
 */
ComPtr<ID2D1Bitmap1> SwapChainSurface::snapshot() {
    if (!context_ || !target_) {
        HH_LOG_WARN(kLog, L"snapshot: no target");
        return nullptr;
    }
    if (drawing_) {
        HH_LOG_WARN(kLog, L"snapshot: cannot copy while a frame is open");
        return nullptr;
    }
    const D2D1_SIZE_U size = target_->GetPixelSize();
    if (size.width == 0 || size.height == 0) {
        return nullptr;
    }

    // A plain GPU bitmap: drawable, same format so the copy is a straight blit.
    const D2D1_BITMAP_PROPERTIES1 props = bitmapProps(D2D1_BITMAP_OPTIONS_NONE);
    ComPtr<ID2D1Bitmap1> copy;
    HRESULT hr = context_->CreateBitmap(size, nullptr, 0, &props, copy.GetAddressOf());
    if (FAILED(hr) || !copy) {
        HH_LOG_WARN(kLog, L"snapshot: CreateBitmap failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return nullptr;
    }
    hr = copy->CopyFromBitmap(nullptr, target_.Get(), nullptr);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"snapshot: CopyFromBitmap failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return nullptr;
    }
    return copy;
}

/**
 * @brief Reads the back buffer to CPU memory as tightly packed BGRA rows.
 */
bool SwapChainSurface::readback(std::vector<uint8_t>& bgra, UINT& width, UINT& height) {
    bgra.clear();
    width = 0;
    height = 0;
    if (!context_ || !target_) {
        HH_LOG_WARN(kLog, L"readback: no target");
        return false;
    }
    if (drawing_) {
        HH_LOG_WARN(kLog, L"readback: cannot copy while a frame is open");
        return false;
    }
    const D2D1_SIZE_U size = target_->GetPixelSize();
    if (size.width == 0 || size.height == 0) {
        return false;
    }

    // Staging bitmap the CPU can map.
    const D2D1_BITMAP_PROPERTIES1 props = bitmapProps(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    ComPtr<ID2D1Bitmap1> staging;
    HRESULT hr = context_->CreateBitmap(size, nullptr, 0, &props, staging.GetAddressOf());
    if (FAILED(hr) || !staging) {
        HH_LOG_WARN(kLog, L"readback: CreateBitmap failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    hr = staging->CopyFromBitmap(nullptr, target_.Get(), nullptr);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"readback: CopyFromBitmap failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }

    // Map, then copy row by row because the driver pitch may be padded.
    D2D1_MAPPED_RECT mapped = {};
    hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr) || mapped.bits == nullptr) {
        HH_LOG_WARN(kLog, L"readback: Map failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(size.width) * 4u;
    bgra.resize(rowBytes * static_cast<size_t>(size.height));
    for (UINT y = 0; y < size.height; ++y) {
        const uint8_t* src = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
        std::memcpy(bgra.data() + static_cast<size_t>(y) * rowBytes, src, rowBytes);
    }
    hr = staging->Unmap();
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"readback: Unmap failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
    }
    width = size.width;
    height = size.height;
    return true;
}

} // namespace hh::ui
