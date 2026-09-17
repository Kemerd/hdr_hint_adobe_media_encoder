// ---------------------------------------------------------------------------
// SingleInstance.cpp - one HdrHint per user session, argv forwarding.
//
// The primary instance owns a session-local named mutex and a message-only
// window with a fixed class name. A second launch fails to own the mutex,
// finds that window and hands over its arguments as a small UTF-8 JSON
// document ({"argv": [...]}) through WM_COPYDATA, then exits.
//
// The payload is tagged with dwData = 0x4844 ('HD') so a stray WM_COPYDATA
// from some other program is ignored, and it is parsed with exceptions off.
// ---------------------------------------------------------------------------
#include "platform/SingleInstance.h"

#include "core/Logger.h"
#include "platform/Utf.h"

#include <nlohmann/json.hpp>

#include <exception>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"SingleInstance";

/// Session-local mutex: one primary per logon session.
constexpr const wchar_t* kMutexName = L"Local\\HdrHint.SingleInstance.{6F1E0D0C-4B4B-4C1A-9B0C-1B3A2C5D7E9F}";

/// Window class of the message-only receiver; secondaries look it up by name.
constexpr const wchar_t* kReceiverClass = L"HdrHint.MessageTarget";

/// Window title (purely cosmetic for message-only windows).
constexpr const wchar_t* kReceiverTitle = L"HdrHint";

/// dwData tag identifying our WM_COPYDATA payloads ('H' 'D').
constexpr ULONG_PTR kCopyDataTag = 0x4844;

/// Largest payload we will parse; argv never comes close to this.
constexpr DWORD kMaxPayloadBytes = 1u * 1024u * 1024u;

/// How many times, and how often, a secondary looks for the primary's window.
constexpr int kFindRetries = 20;
constexpr DWORD kFindRetryDelayMs = 100;

/// SendMessageTimeout budget for the hand-over.
constexpr UINT kForwardTimeoutMs = 5000;

/**
 * @brief Decodes a {"argv": [...]} payload into wide strings.
 * @return false when the bytes are not our document
 */
bool decodeArgs(const char* data, DWORD bytes, std::vector<std::wstring>& out) {
    out.clear();
    if (!data || bytes == 0) {
        return false;
    }
    // Copy first: the sender's buffer is only valid for the duration of the
    // message, and the parser wants a contiguous string anyway.
    const std::string text(data, static_cast<size_t>(bytes));
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("argv")) {
        return false;
    }
    const nlohmann::json& argv = doc["argv"];
    if (!argv.is_array()) {
        return false;
    }
    // Only strings are arguments; anything else is silently skipped.
    out.reserve(argv.size());
    for (const nlohmann::json& element : argv) {
        if (element.is_string()) {
            out.push_back(toWide(element.get_ref<const std::string&>()));
        }
    }
    return true;
}

/**
 * @brief Encodes argv as the UTF-8 JSON document the receiver expects.
 *        Empty when serialisation fails (never throws).
 */
std::string encodeArgs(const std::vector<std::wstring>& args) {
    try {
        nlohmann::json doc = nlohmann::json::object();
        nlohmann::json list = nlohmann::json::array();
        for (const std::wstring& arg : args) {
            list.push_back(toUtf8(arg));
        }
        doc["argv"] = std::move(list);
        // The replace handler guarantees dump() cannot throw on odd bytes.
        return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception& e) {
        HH_LOG_ERROR(kLog, L"failed to encode argv: {}", toWide(e.what()));
        return {};
    } catch (...) {
        HH_LOG_ERROR(kLog, L"failed to encode argv: unknown exception");
        return {};
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

/**
 * @brief Destroys the receiver window, unregisters its class and releases
 *        the mutex. Must run on the thread that called registerReceiver().
 */
SingleInstance::~SingleInstance() {
    if (receiver_) {
        // Detach first so a message racing the destroy never reaches a
        // half-destroyed object.
        ::SetWindowLongPtrW(receiver_, GWLP_USERDATA, 0);
        if (!::DestroyWindow(receiver_)) {
            const DWORD err = ::GetLastError();
            HH_LOG_DEBUG(kLog, L"DestroyWindow failed: {} ({})", win32ErrorText(err), err);
        }
        receiver_ = nullptr;
        if (!::UnregisterClassW(kReceiverClass, ::GetModuleHandleW(nullptr))) {
            const DWORD err = ::GetLastError();
            HH_LOG_DEBUG(kLog, L"UnregisterClassW failed: {} ({})", win32ErrorText(err), err);
        }
    }
    onArgs_ = nullptr;

    // Give the mutex back explicitly before the handle closes so the next
    // launch sees a clean (not abandoned) object.
    if (mutex_ && primary_) {
        ::ReleaseMutex(mutex_.get());
    }
    mutex_.reset();
    primary_ = false;
}

// ---------------------------------------------------------------------------
// acquire
// ---------------------------------------------------------------------------

/**
 * @brief Tries to become the primary instance.
 *
 * ERROR_ALREADY_EXISTS (and ERROR_ACCESS_DENIED, which also means the object
 * is there) make this a secondary. Any other failure is logged and treated
 * as primary so the app still starts.
 */
bool SingleInstance::acquire() {
    if (mutex_) {
        return primary_;
    }

    HANDLE raw = ::CreateMutexW(nullptr, TRUE, kMutexName);
    const DWORD err = ::GetLastError();
    if (!raw) {
        if (err == ERROR_ACCESS_DENIED) {
            HH_LOG_INFO(kLog, L"instance mutex exists (access denied); this is a secondary");
            primary_ = false;
            return false;
        }
        HH_LOG_ERROR(kLog, L"CreateMutexW failed: {} ({}); proceeding as primary", win32ErrorText(err), err);
        primary_ = true;
        return true;
    }
    mutex_.reset(raw);

    if (err == ERROR_ALREADY_EXISTS) {
        // Someone else owns it. Drop our handle so we never hold it open.
        HH_LOG_INFO(kLog, L"another HdrHint owns the instance mutex; this is a secondary");
        mutex_.reset();
        primary_ = false;
        return false;
    }

    primary_ = true;
    HH_LOG_DEBUG(kLog, L"acquired instance mutex; this is the primary");
    return true;
}

// ---------------------------------------------------------------------------
// receiver window
// ---------------------------------------------------------------------------

/**
 * @brief Creates the message-only window that receives forwarded argv.
 *
 * The window is created on the calling thread, which is therefore the
 * thread @p onArgs runs on. UIPI is relaxed for WM_COPYDATA so a
 * non-elevated secondary can still reach an elevated primary.
 */
bool SingleInstance::registerReceiver(std::function<void(const std::vector<std::wstring>&)> onArgs) {
    if (receiver_) {
        // Already registered: just swap the callback.
        onArgs_ = std::move(onArgs);
        return true;
    }
    if (!onArgs) {
        HH_LOG_WARN(kLog, L"registerReceiver() called without a callback; forwarded argv will be dropped");
    }
    onArgs_ = std::move(onArgs);

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!instance) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"GetModuleHandleW failed: {} ({})", win32ErrorText(err), err);
        return false;
    }

    // Register the class (idempotent within the process).
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &SingleInstance::receiverProc;
    cls.hInstance = instance;
    cls.lpszClassName = kReceiverClass;
    if (!::RegisterClassExW(&cls)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            HH_LOG_ERROR(kLog, L"RegisterClassExW failed: {} ({})", win32ErrorText(err), err);
            return false;
        }
    }

    // Message-only window: no painting, no taskbar, just a message target.
    // `this` travels through lpParam so WM_NCCREATE can attach it at once.
    HWND hwnd = ::CreateWindowExW(0, kReceiverClass, kReceiverTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                                  this);
    if (!hwnd) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"CreateWindowExW(message-only) failed: {} ({})", win32ErrorText(err), err);
        return false;
    }
    receiver_ = hwnd;

    // Belt and braces: make sure the back-pointer is set even if WM_NCCREATE
    // was not delivered with our lpParam for some reason.
    ::SetLastError(0);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Allow WM_COPYDATA from lower-integrity senders.
    if (!::ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr)) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"ChangeWindowMessageFilterEx(WM_COPYDATA) failed: {} ({})", win32ErrorText(err), err);
    }

    HH_LOG_DEBUG(kLog, L"receiver window created");
    return true;
}

/**
 * @brief Window procedure of the receiver: handles WM_COPYDATA only.
 */
LRESULT CALLBACK SingleInstance::receiverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Attach the owner as early as possible.
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        if (create && create->lpCreateParams) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    if (message != WM_COPYDATA) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // ---- WM_COPYDATA ------------------------------------------------------
    // Validate everything before trusting a single byte of the payload.
    auto* self = reinterpret_cast<SingleInstance*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
    if (!self || !copy) {
        return FALSE;
    }
    if (copy->dwData != kCopyDataTag) {
        HH_LOG_DEBUG(kLog, L"ignoring WM_COPYDATA with tag {:#x}", static_cast<uint64_t>(copy->dwData));
        return FALSE;
    }
    if (!copy->lpData || copy->cbData == 0 || copy->cbData > kMaxPayloadBytes) {
        HH_LOG_WARN(kLog, L"ignoring WM_COPYDATA with {} bytes", copy->cbData);
        return FALSE;
    }

    // Nothing may throw out of a window procedure.
    try {
        std::vector<std::wstring> args;
        if (!decodeArgs(static_cast<const char*>(copy->lpData), copy->cbData, args)) {
            HH_LOG_WARN(kLog, L"ignoring WM_COPYDATA with a payload that is not {{\"argv\":[...]}}");
            return FALSE;
        }
        HH_LOG_INFO(kLog, L"received {} forwarded argument(s)", args.size());
        if (self->onArgs_) {
            self->onArgs_(args);
        }
        return TRUE;
    } catch (const std::exception& e) {
        HH_LOG_ERROR(kLog, L"exception while handling forwarded argv: {}", toWide(e.what()));
        return FALSE;
    } catch (...) {
        HH_LOG_ERROR(kLog, L"unknown exception while handling forwarded argv");
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// forwardToPrimary
// ---------------------------------------------------------------------------

/**
 * @brief Finds the primary's receiver window and sends it argv.
 *
 * The primary may still be starting up, so the lookup retries for about two
 * seconds. AllowSetForegroundWindow lets the primary bring itself to the
 * front in response.
 */
bool SingleInstance::forwardToPrimary(const std::vector<std::wstring>& args) {
    // Find the receiver, retrying while the primary finishes starting.
    HWND target = nullptr;
    for (int attempt = 0; attempt < kFindRetries && !target; ++attempt) {
        target = ::FindWindowExW(HWND_MESSAGE, nullptr, kReceiverClass, nullptr);
        if (!target) {
            ::Sleep(kFindRetryDelayMs);
        }
    }
    if (!target) {
        HH_LOG_WARN(kLog, L"no primary receiver window found after {} attempts", kFindRetries);
        return false;
    }

    // Let the primary steal focus when it handles the arguments.
    DWORD targetPid = 0;
    ::GetWindowThreadProcessId(target, &targetPid);
    if (targetPid != 0) {
        if (!::AllowSetForegroundWindow(targetPid)) {
            const DWORD err = ::GetLastError();
            HH_LOG_DEBUG(kLog, L"AllowSetForegroundWindow({}) failed: {} ({})", targetPid, win32ErrorText(err), err);
        }
    }

    // Serialise and send.
    std::string payload = encodeArgs(args);
    if (payload.empty()) {
        return false;
    }
    if (payload.size() > kMaxPayloadBytes) {
        HH_LOG_ERROR(kLog, L"argv payload of {} bytes is too large to forward", payload.size());
        return false;
    }

    COPYDATASTRUCT copy{};
    copy.dwData = kCopyDataTag;
    copy.cbData = static_cast<DWORD>(payload.size());
    copy.lpData = payload.data();

    DWORD_PTR result = 0;
    const LRESULT sent = ::SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy),
                                               SMTO_ABORTIFHUNG | SMTO_BLOCK, kForwardTimeoutMs, &result);
    if (!sent) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"SendMessageTimeoutW(WM_COPYDATA) failed: {} ({})", win32ErrorText(err), err);
        return false;
    }
    if (result == 0) {
        HH_LOG_WARN(kLog, L"primary rejected the forwarded argv");
        return false;
    }
    HH_LOG_INFO(kLog, L"forwarded {} argument(s) to the primary (pid {})", args.size(), targetPid);
    return true;
}

} // namespace hh::platform
