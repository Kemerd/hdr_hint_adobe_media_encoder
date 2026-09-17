// ---------------------------------------------------------------------------
// NamedPipe.cpp - one overlapped server-side pipe instance.
//
// The server thread multiplexes every instance on event(): it waits, calls
// onSignalled() on whichever instance fired, and that call advances the
// state machine one step:
//
//   Connecting --(client arrived)--> Connected --(beginRead)--> Reading
//   Reading    --(bytes landed)----> Connected --(beginRead)--> Reading ...
//   any        --(client gone)-----> DisconnectNamedPipe -> Connecting
//
// Reads that complete synchronously are harvested in a bounded loop rather
// than by recursion, so a chatty client can never blow the stack or starve
// the other instances.
// ---------------------------------------------------------------------------
#include "platform/NamedPipe.h"

#include "core/Logger.h"
#include "platform/Utf.h"

#include <sddl.h>

#include <algorithm>
#include <cstring>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"NamedPipe";

/// Kernel buffer sizes requested from CreateNamedPipeW (in and out).
constexpr DWORD kPipeBufferSize = 64u * 1024u;

/// Size of the user-mode read buffer; IPC lines are short, 16 KiB is plenty.
constexpr DWORD kReadBufferSize = 16u * 1024u;

/// Upper bound on synchronously completing reads harvested in one
/// onSignalled() call before control goes back to the server loop.
constexpr int kMaxSynchronousReads = 32;

/// How long a cancel-and-drain is willing to wait for the kernel to let go
/// of the OVERLAPPED. Pipe cancellations are immediate; the bound only
/// keeps a pathological case from hanging shutdown.
constexpr DWORD kDrainTimeoutMs = 5000;

/// Largest client SID string we are prepared to accept from the token.
constexpr DWORD kMaxTokenUserBytes = 4096;

/**
 * @brief Cancels the outstanding overlapped I/O (if any) and waits until the
 *        kernel has completed it, so the OVERLAPPED and buffer are free.
 * @param pipe      pipe handle (may be null)
 * @param ov        the OVERLAPPED that was passed to the pending call
 * @param ioPending in/out: whether an I/O is outstanding; cleared on return
 * @param id        instance id for the log lines
 */
void cancelAndDrain(HANDLE pipe, OVERLAPPED& ov, bool& ioPending, uint32_t id) {
    if (!ioPending || !pipe) {
        ioPending = false;
        return;
    }

    // ERROR_NOT_FOUND means the I/O already completed on its own; the drain
    // below still collects that completion.
    if (!::CancelIoEx(pipe, &ov)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_NOT_FOUND) {
            HH_LOG_DEBUG(kLog, L"instance {}: CancelIoEx failed: {} ({})", id, win32ErrorText(err), err);
        }
    }

    // Bounded wait for the completion, then a blocking harvest that returns
    // immediately because the event is already set.
    bool completed = true;
    if (ov.hEvent) {
        completed = (::WaitForSingleObject(ov.hEvent, kDrainTimeoutMs) == WAIT_OBJECT_0);
    }
    if (completed) {
        DWORD bytes = 0;
        if (!::GetOverlappedResult(pipe, &ov, &bytes, TRUE)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_OPERATION_ABORTED) {
                HH_LOG_DEBUG(kLog, L"instance {}: drain ended with {} ({})", id, win32ErrorText(err), err);
            }
        }
    } else {
        HH_LOG_ERROR(kLog, L"instance {}: cancelled I/O did not complete within {} ms", id, kDrainTimeoutMs);
    }
    ioPending = false;
}

/**
 * @brief True for the error codes that simply mean "the client went away".
 */
bool isClientGoneError(DWORD err) noexcept {
    return err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA;
}

} // namespace

// ---------------------------------------------------------------------------
// PipeSecurity
// ---------------------------------------------------------------------------

/**
 * @brief Builds "D:P(A;;GA;;;SY)(A;;GA;;;<user>)": a protected DACL that
 *        grants full access to SYSTEM and the current user and nothing to
 *        anyone else. On any failure attributes() returns nullptr and the
 *        server falls back to the default pipe security.
 */
PipeSecurity::PipeSecurity() {
    // Open our own token to learn the user SID.
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken) || !rawToken) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"OpenProcessToken failed: {} ({}); pipe uses default security", win32ErrorText(err), err);
        return;
    }
    UniqueHandle token(rawToken);

    // First call sizes the TOKEN_USER blob, second call fills it.
    DWORD needed = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed);
    if (needed == 0 || needed > kMaxTokenUserBytes) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"GetTokenInformation(TokenUser) size query failed: {} ({})", win32ErrorText(err), err);
        return;
    }
    std::vector<uint8_t> blob(needed, 0);
    if (!::GetTokenInformation(token.get(), TokenUser, blob.data(), needed, &needed)) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"GetTokenInformation(TokenUser) failed: {} ({})", win32ErrorText(err), err);
        return;
    }
    if (blob.size() < sizeof(TOKEN_USER)) {
        HH_LOG_WARN(kLog, L"TOKEN_USER blob too small ({} bytes)", blob.size());
        return;
    }

    // Validate the SID before converting it to text.
    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(blob.data());
    if (!tokenUser->User.Sid || !::IsValidSid(tokenUser->User.Sid)) {
        HH_LOG_WARN(kLog, L"token user SID is invalid; pipe uses default security");
        return;
    }
    LPWSTR sidText = nullptr;
    if (!::ConvertSidToStringSidW(tokenUser->User.Sid, &sidText) || !sidText) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"ConvertSidToStringSidW failed: {} ({})", win32ErrorText(err), err);
        return;
    }
    const std::wstring userSid(sidText);
    ::LocalFree(sidText);
    sidText = nullptr;

    // Build the descriptor from SDDL. "P" makes the DACL protected so no
    // inherited ACE can widen it later.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + userSid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ULONG descriptorBytes = 0;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                &descriptorBytes) ||
        !descriptor) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"ConvertStringSecurityDescriptorToSecurityDescriptorW failed: {} ({})",
                    win32ErrorText(err), err);
        return;
    }

    // Everything checked out: wire up the SECURITY_ATTRIBUTES.
    sd_ = descriptor;
    sa_.nLength = sizeof(sa_);
    sa_.lpSecurityDescriptor = sd_;
    sa_.bInheritHandle = FALSE;
    ok_ = true;
    HH_LOG_DEBUG(kLog, L"pipe DACL built for {}", userSid);
}

/**
 * @brief Frees the descriptor allocated by the SDDL conversion.
 */
PipeSecurity::~PipeSecurity() {
    if (sd_) {
        ::LocalFree(sd_);
        sd_ = nullptr;
    }
    ok_ = false;
}

// ---------------------------------------------------------------------------
// PipeInstance - construction / destruction
// ---------------------------------------------------------------------------

/**
 * @brief Constructs an idle instance; nothing is created until create().
 */
PipeInstance::PipeInstance() = default;

/**
 * @brief Cancels any outstanding I/O and closes the pipe.
 */
PipeInstance::~PipeInstance() {
    close();
}

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------

/**
 * @brief Creates one server-side pipe instance and starts listening.
 *
 * @param fullName      "\\\\.\\pipe\\HdrHint"
 * @param firstInstance adds FILE_FLAG_FIRST_PIPE_INSTANCE so a squatter
 *                      already owning the name makes creation fail
 * @param maxInstances  passed to CreateNamedPipeW (clamped to 1..255)
 * @param security      optional DACL (nullptr = default)
 */
Result<void> PipeInstance::create(std::wstring_view fullName, bool firstInstance, DWORD maxInstances,
                                  SECURITY_ATTRIBUTES* security) {
    if (fullName.empty()) {
        HH_LOG_ERROR(kLog, L"create(): empty pipe name");
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"PipeInstance::create: empty name");
    }
    // Re-creating a live instance must not leak its pending I/O.
    if (pipe_) {
        close();
    }

    // Remember the parameters so a later re-create can reuse them.
    name_ = std::wstring(fullName);
    maxInstances_ = std::clamp<DWORD>(maxInstances, 1u, PIPE_UNLIMITED_INSTANCES);
    security_ = security;

    // Manual-reset event shared by connect and read completions.
    event_ = makeEvent(true, false);
    if (!event_) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"CreateEventW failed: {} ({})", win32ErrorText(err), err);
        return Error::fromWin32(err, L"PipeInstance: create event");
    }
    buffer_.assign(kReadBufferSize, 0);

    // Byte-mode duplex pipe, overlapped, local clients only.
    const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0u);
    const DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    HANDLE raw = ::CreateNamedPipeW(name_.c_str(), openMode, pipeMode, maxInstances_, kPipeBufferSize, kPipeBufferSize,
                                    0, security_);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"CreateNamedPipeW({}) failed: {} ({})", name_, win32ErrorText(err), err);
        event_.reset();
        return Error::fromWin32(err, L"CreateNamedPipeW " + name_);
    }
    pipe_.reset(raw);
    state_ = State::Idle;
    ioPending_ = false;

    // Start listening straight away. beginConnect() re-posts the Win32 error
    // so it can be reported here.
    if (!beginConnect()) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"ConnectNamedPipe({}) failed: {} ({})", name_, win32ErrorText(err), err);
        close();
        return Error::fromWin32(err, L"ConnectNamedPipe " + name_);
    }
    HH_LOG_DEBUG(kLog, L"instance {} created on {} (first={}, max={})", id_, name_, firstInstance, maxInstances_);
    return {};
}

// ---------------------------------------------------------------------------
// state machine
// ---------------------------------------------------------------------------

/**
 * @brief Issues an overlapped ConnectNamedPipe.
 *
 * ERROR_IO_PENDING leaves the instance Connecting with an I/O outstanding.
 * ERROR_PIPE_CONNECTED means a client raced us and is already there: the
 * event is set by hand so the server loop wakes and starts reading.
 *
 * @return false on a hard failure (state becomes Closed; GetLastError() is
 *         re-posted with the failing code for the caller's diagnostics)
 */
bool PipeInstance::beginConnect() {
    if (!pipe_ || !event_) {
        state_ = State::Closed;
        ::SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    if (ioPending_) {
        // A connect or read is still outstanding; never issue two at once.
        return true;
    }

    // Fresh OVERLAPPED for this connect.
    overlapped_ = OVERLAPPED{};
    overlapped_.hEvent = event_.get();

    if (::ConnectNamedPipe(pipe_.get(), &overlapped_)) {
        // Overlapped connects are documented to always return FALSE, but if
        // one ever returns TRUE the client is connected: proceed as such.
        state_ = State::Connected;
        ::SetEvent(event_.get());
        return true;
    }

    const DWORD err = ::GetLastError();
    switch (err) {
    case ERROR_IO_PENDING:
        // Normal path: wait for a client on event().
        state_ = State::Connecting;
        ioPending_ = true;
        return true;
    case ERROR_PIPE_CONNECTED:
        // A client connected between CreateNamedPipe and now. Wake the
        // server so it moves on to reading.
        state_ = State::Connected;
        ::SetEvent(event_.get());
        return true;
    default:
        HH_LOG_WARN(kLog, L"instance {}: ConnectNamedPipe failed: {} ({})", id_, win32ErrorText(err), err);
        state_ = State::Closed;
        ::SetLastError(err);
        return false;
    }
}

/**
 * @brief Issues one overlapped ReadFile into buffer_.
 *
 * A synchronous completion is not harvested here: the I/O manager still
 * signals the event and GetOverlappedResult() in onSignalled() collects the
 * bytes, which keeps the harvesting logic in one place and recursion-free.
 *
 * @return false when the client is gone (the instance has already been
 *         reset to Connecting via disconnectAndReconnect())
 */
bool PipeInstance::beginRead() {
    if (!pipe_ || !event_) {
        state_ = State::Closed;
        return false;
    }
    if (ioPending_) {
        // Never stack a second read on the one already outstanding.
        return true;
    }
    if (buffer_.size() < kReadBufferSize) {
        buffer_.assign(kReadBufferSize, 0);
    }

    // Reset first: the event must be clear before the kernel can set it.
    ::ResetEvent(event_.get());
    overlapped_ = OVERLAPPED{};
    overlapped_.hEvent = event_.get();
    state_ = State::Reading;
    ioPending_ = true;

    DWORD bytesNow = 0;
    if (::ReadFile(pipe_.get(), buffer_.data(), static_cast<DWORD>(buffer_.size()), &bytesNow, &overlapped_)) {
        // Completed synchronously; the event is signalled and the bytes are
        // waiting in GetOverlappedResult().
        return true;
    }

    const DWORD err = ::GetLastError();
    if (err == ERROR_IO_PENDING || err == ERROR_MORE_DATA) {
        // Pending (normal) or, in message mode, a partial read - both leave
        // a completion to harvest.
        return true;
    }

    // Anything else means the client hung up (or the handle is dead).
    ioPending_ = false;
    if (isClientGoneError(err)) {
        HH_LOG_INFO(kLog, L"instance {}: client disconnected ({})", id_, err);
    } else {
        HH_LOG_WARN(kLog, L"instance {}: ReadFile failed: {} ({})", id_, win32ErrorText(err), err);
    }
    disconnectAndReconnect();
    return false;
}

/**
 * @brief Advances the state machine after event() fired.
 *
 * Connecting: harvests the connect and falls into reading.
 * Connected/Reading: harvests completed reads in a bounded loop, re-issuing
 * a read after each one, until a read is genuinely pending.
 *
 * @param received appended with every byte read
 * @return false when the client disconnected (the instance has been reset
 *         and is listening for the next client) or the instance is unusable
 */
bool PipeInstance::onSignalled(std::string& received) {
    if (!pipe_ || !event_) {
        return false;
    }
    if (state_ == State::Idle || state_ == State::Closed) {
        // Nothing is in flight; a signal here is stale.
        return false;
    }

    // ---- connect phase --------------------------------------------------
    if (state_ == State::Connecting) {
        DWORD bytes = 0;
        if (!::GetOverlappedResult(pipe_.get(), &overlapped_, &bytes, FALSE)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_INCOMPLETE) {
                // Spurious wake-up; keep waiting.
                return true;
            }
            ioPending_ = false;
            if (err != ERROR_PIPE_CONNECTED) {
                // The listen itself failed (e.g. the pipe was torn down).
                HH_LOG_WARN(kLog, L"instance {}: connect failed: {} ({})", id_, win32ErrorText(err), err);
                disconnectAndReconnect();
                return false;
            }
        }
        // A client is attached: move on to reading.
        ioPending_ = false;
        state_ = State::Connected;
        ::ResetEvent(event_.get());
        HH_LOG_INFO(kLog, L"instance {}: client connected", id_);
    }

    if (state_ != State::Connected && state_ != State::Reading) {
        return false;
    }

    // ---- read phase -----------------------------------------------------
    // Harvest completed reads until one is genuinely pending. The loop is
    // bounded so a client blasting data cannot monopolise the server thread.
    for (int spins = 0; spins < kMaxSynchronousReads; ++spins) {
        if (state_ == State::Connected) {
            // No read outstanding yet: issue one. A false return means the
            // client is gone and the instance has already been reset.
            if (!beginRead()) {
                return false;
            }
        }

        // Poll the outstanding read without blocking.
        DWORD bytes = 0;
        if (!::GetOverlappedResult(pipe_.get(), &overlapped_, &bytes, FALSE)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_INCOMPLETE) {
                // Genuinely pending: the server waits on event() again.
                return true;
            }
            if (err != ERROR_MORE_DATA) {
                // Client hung up or the pipe broke.
                ioPending_ = false;
                if (isClientGoneError(err)) {
                    HH_LOG_INFO(kLog, L"instance {}: client disconnected ({})", id_, err);
                } else {
                    HH_LOG_WARN(kLog, L"instance {}: read failed: {} ({})", id_, win32ErrorText(err), err);
                }
                disconnectAndReconnect();
                return false;
            }
            // ERROR_MORE_DATA only happens in message mode; `bytes` still
            // holds what fit, so treat it like a normal completion.
        }

        // The read completed: append the bytes (bounded by the buffer) and
        // get ready to issue the next one.
        ioPending_ = false;
        if (bytes > 0) {
            const size_t count = std::min<size_t>(static_cast<size_t>(bytes), buffer_.size());
            received.append(buffer_.data(), count);
        }
        ::ResetEvent(event_.get());
        state_ = State::Connected;
    }

    // Spin cap reached with no read outstanding. Leave the event set so the
    // server loop comes straight back to this instance after serving others.
    ::SetEvent(event_.get());
    return true;
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

/**
 * @brief Writes the whole buffer synchronously with a private OVERLAPPED.
 *
 * The pipe handle is overlapped, so the write must be overlapped too; a
 * dedicated event lets it proceed while a read is outstanding. On timeout
 * the write is cancelled and drained before the OVERLAPPED goes out of scope.
 */
bool PipeInstance::write(std::string_view bytes, DWORD timeoutMs) {
    if (!pipe_) {
        HH_LOG_DEBUG(kLog, L"instance {}: write on closed pipe", id_);
        return false;
    }
    if (state_ != State::Connected && state_ != State::Reading) {
        HH_LOG_DEBUG(kLog, L"instance {}: write while not connected", id_);
        return false;
    }
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > static_cast<size_t>(MAXDWORD)) {
        HH_LOG_WARN(kLog, L"instance {}: write of {} bytes is too large", id_, bytes.size());
        return false;
    }

    // Private event + OVERLAPPED so this write never collides with the read.
    UniqueHandle done = makeEvent(true, false);
    if (!done) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"instance {}: CreateEventW for write failed: {} ({})", id_, win32ErrorText(err), err);
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = done.get();

    const DWORD toWrite = static_cast<DWORD>(bytes.size());
    DWORD written = 0;
    if (!::WriteFile(pipe_.get(), bytes.data(), toWrite, &written, &ov)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_IO_PENDING) {
            if (isClientGoneError(err)) {
                HH_LOG_INFO(kLog, L"instance {}: write to disconnected client ({})", id_, err);
            } else {
                HH_LOG_WARN(kLog, L"instance {}: WriteFile failed: {} ({})", id_, win32ErrorText(err), err);
            }
            return false;
        }

        // Pending: wait up to the timeout for the kernel to take the bytes.
        const DWORD wait = ::WaitForSingleObject(done.get(), timeoutMs);
        if (wait != WAIT_OBJECT_0) {
            // Stalled client. Cancel, then drain so `ov` is no longer in use
            // when this frame unwinds.
            HH_LOG_WARN(kLog, L"instance {}: write of {} bytes timed out after {} ms", id_, toWrite, timeoutMs);
            ::CancelIoEx(pipe_.get(), &ov);
            DWORD ignored = 0;
            ::GetOverlappedResult(pipe_.get(), &ov, &ignored, TRUE);
            return false;
        }
    }

    // Collect the final byte count (works for both sync and async completion).
    if (!::GetOverlappedResult(pipe_.get(), &ov, &written, FALSE)) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"instance {}: write completion failed: {} ({})", id_, win32ErrorText(err), err);
        return false;
    }
    if (written != toWrite) {
        HH_LOG_WARN(kLog, L"instance {}: short write ({} of {} bytes)", id_, written, toWrite);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// disconnect / close
// ---------------------------------------------------------------------------

/**
 * @brief Drops the current client and listens for the next one.
 *
 * Any outstanding I/O is cancelled and drained first so the OVERLAPPED can
 * be reused safely by the new ConnectNamedPipe.
 */
void PipeInstance::disconnectAndReconnect() {
    if (!pipe_) {
        state_ = State::Closed;
        ioPending_ = false;
        return;
    }

    // Make sure nothing is in flight before touching the pipe state.
    cancelAndDrain(pipe_.get(), overlapped_, ioPending_, id_);

    // ERROR_PIPE_NOT_CONNECTED is routine (client already gone); anything
    // else is worth a debug line but not fatal.
    if (!::DisconnectNamedPipe(pipe_.get())) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_PIPE_NOT_CONNECTED) {
            HH_LOG_DEBUG(kLog, L"instance {}: DisconnectNamedPipe failed: {} ({})", id_, win32ErrorText(err), err);
        }
    }
    if (event_) {
        ::ResetEvent(event_.get());
    }
    state_ = State::Idle;

    // Listen again. On failure state_ is Closed; the server decides whether
    // to re-create the instance.
    if (!beginConnect()) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"instance {}: could not re-listen: {} ({})", id_, win32ErrorText(err), err);
    }
}

/**
 * @brief Cancels I/O, drains it and closes the pipe handle.
 *
 * The event is kept (reset, unsignalled) so a server loop that still has it
 * in a wait array never waits on a dead handle; it is released by the
 * destructor or replaced by the next create().
 */
void PipeInstance::close() {
    if (pipe_) {
        // Cancel and wait for the outstanding I/O before the handle goes.
        cancelAndDrain(pipe_.get(), overlapped_, ioPending_, id_);
        pipe_.reset();
        HH_LOG_DEBUG(kLog, L"instance {} closed", id_);
    }

    // The handle is gone, so no I/O can touch the buffer any more.
    ioPending_ = false;
    state_ = State::Closed;
    if (event_) {
        ::ResetEvent(event_.get());
    }
    buffer_.clear();
    buffer_.shrink_to_fit();
    overlapped_ = OVERLAPPED{};
}

} // namespace hh::platform
