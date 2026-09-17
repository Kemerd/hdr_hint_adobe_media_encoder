// ---------------------------------------------------------------------------
// GraphicsDevice.cpp - D3D11 / DXGI / Direct2D / DirectWrite / DComp stack.
// ---------------------------------------------------------------------------
#include "ui/gfx/GraphicsDevice.h"

#include "core/Logger.h"

#include <d2d1_2.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>

#include <iterator>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Gfx";

// Feature levels in preference order. 9_3 keeps ancient VMs alive; Direct2D
// only needs 9_1 but the swap chain path wants BGRA + flip model.
constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,
};

/**
 * @brief True when HH_FORCE_WARP=1 is set in the environment (testing aid).
 */
bool warpForcedByEnvironment() {
    wchar_t buffer[8] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"HH_FORCE_WARP", buffer, static_cast<DWORD>(std::size(buffer)));
    // A missing variable returns 0; anything longer than our buffer is not "1".
    if (n == 0 || n >= std::size(buffer)) {
        return false;
    }
    return buffer[0] == L'1' && buffer[1] == L'\0';
}

/**
 * @brief Creates a D3D11 device of the requested driver type with the given flags.
 */
HRESULT createD3D(D3D_DRIVER_TYPE driverType, UINT flags, ComPtr<ID3D11Device>& device, D3D_FEATURE_LEVEL& level) {
    device.Reset();
    level = D3D_FEATURE_LEVEL_9_1;
    return ::D3D11CreateDevice(nullptr, driverType, nullptr, flags, kFeatureLevels,
                               static_cast<UINT>(std::size(kFeatureLevels)), D3D11_SDK_VERSION,
                               device.GetAddressOf(), &level, nullptr);
}

/**
 * @brief Tries hardware/WARP with (in debug builds) and without the debug layer.
 */
HRESULT createD3DWithFallbacks(D3D_DRIVER_TYPE driverType, ComPtr<ID3D11Device>& device, D3D_FEATURE_LEVEL& level) {
    const UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    // The debug layer only exists when the Graphics Tools optional feature is
    // installed; when it is missing the call fails with SDK_COMPONENT_MISSING
    // and we silently retry without it.
    HRESULT hr = createD3D(driverType, baseFlags | D3D11_CREATE_DEVICE_DEBUG, device, level);
    if (SUCCEEDED(hr)) {
        return hr;
    }
    HH_LOG_DEBUG(kLog, L"D3D11 debug layer unavailable (hr=0x{:08X}); retrying without it", static_cast<uint32_t>(hr));
#endif
    return createD3D(driverType, baseFlags, device, level);
}

} // namespace

/**
 * @brief Releases every device object.
 */
GraphicsDevice::~GraphicsDevice() {
    destroy();
}

/**
 * @brief Builds the whole stack; false when even WARP could not be created.
 */
bool GraphicsDevice::create() {
    // Start from a clean slate so a failed half-creation never leaks objects.
    destroy();

    // ---- D3D11 ---------------------------------------------------------------
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    HRESULT hr = E_FAIL;
    warp_ = warpForcedByEnvironment();
    if (warp_) {
        HH_LOG_INFO(kLog, L"HH_FORCE_WARP=1: using the WARP software rasterizer");
    } else {
        hr = createD3DWithFallbacks(D3D_DRIVER_TYPE_HARDWARE, d3d_, level);
        if (FAILED(hr)) {
            HH_LOG_WARN(kLog, L"hardware D3D11 device failed (hr=0x{:08X}); falling back to WARP", static_cast<uint32_t>(hr));
            warp_ = true;
        }
    }
    if (warp_) {
        hr = createD3DWithFallbacks(D3D_DRIVER_TYPE_WARP, d3d_, level);
        if (FAILED(hr)) {
            HH_LOG_ERROR(kLog, L"WARP D3D11 device failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
            destroy();
            return false;
        }
    }
    if (!d3d_) {
        HH_LOG_ERROR(kLog, L"D3D11CreateDevice succeeded without a device");
        destroy();
        return false;
    }

    // ---- DXGI ----------------------------------------------------------------
    hr = d3d_.As(&dxgi_);
    if (FAILED(hr) || !dxgi_) {
        HH_LOG_ERROR(kLog, L"IDXGIDevice1 unavailable (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }

    // The factory that created the adapter is the one that must create our
    // composition swap chains.
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr) || !adapter) {
        HH_LOG_ERROR(kLog, L"IDXGIDevice1::GetAdapter failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }
    hr = adapter->GetParent(IID_PPV_ARGS(dxgiFactory_.GetAddressOf()));
    if (FAILED(hr) || !dxgiFactory_) {
        HH_LOG_ERROR(kLog, L"IDXGIFactory2 unavailable (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }

    // ---- Direct2D ------------------------------------------------------------
    D2D1_FACTORY_OPTIONS options = {};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory2), &options,
                             reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
#if defined(_DEBUG)
    // The D2D debug layer also depends on the optional SDK component.
    if (FAILED(hr)) {
        options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory2), &options,
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    }
#endif
    if (FAILED(hr) || !d2dFactory_) {
        HH_LOG_ERROR(kLog, L"D2D1CreateFactory failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }
    hr = d2dFactory_->CreateDevice(dxgi_.Get(), d2dDevice_.GetAddressOf());
    if (FAILED(hr) || !d2dDevice_) {
        HH_LOG_ERROR(kLog, L"ID2D1Factory2::CreateDevice failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }

    // ---- DirectWrite ---------------------------------------------------------
    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                               reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(hr) || !dwrite_) {
        HH_LOG_ERROR(kLog, L"DWriteCreateFactory failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }

    // ---- DirectComposition ---------------------------------------------------
    hr = ::DCompositionCreateDevice2(dxgi_.Get(), __uuidof(IDCompositionDesktopDevice),
                                     reinterpret_cast<void**>(dcomp_.GetAddressOf()));
    if (FAILED(hr) || !dcomp_) {
        HH_LOG_ERROR(kLog, L"DCompositionCreateDevice2 failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        destroy();
        return false;
    }

    lost_ = false;
    HH_LOG_INFO(kLog, L"graphics device ready: {} feature level 0x{:X} generation {}",
                warp_ ? L"WARP" : L"hardware", static_cast<uint32_t>(level), generation_);
    return true;
}

/**
 * @brief Releases everything in reverse creation order.
 */
void GraphicsDevice::destroy() {
    dcomp_.Reset();
    dwrite_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    dxgiFactory_.Reset();
    dxgi_.Reset();
    d3d_.Reset();
}

/**
 * @brief Device-lost recovery: rebuild and bump the generation so caches drop stale objects.
 */
bool GraphicsDevice::recreate() {
    HH_LOG_WARN(kLog, L"recreating the graphics device (generation {} -> {})", generation_, generation_ + 1);
    destroy();
    const bool ok = create();
    ++generation_;
    lost_ = false;
    if (!ok) {
        HH_LOG_ERROR(kLog, L"graphics device recreation failed");
    }
    return ok;
}

} // namespace hh::ui
