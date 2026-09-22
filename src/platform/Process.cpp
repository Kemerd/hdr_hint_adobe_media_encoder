// ---------------------------------------------------------------------------
// Process.cpp - child processes, command-line quoting and process queries.
//
// Three groups live here:
//   * quoting/launching: quoteArgument, buildCommandLine, ChildProcess,
//     runCapture, launchDetached
//   * queries on other processes: image name/path, parent pid, command
//     line, pid lookup, liveness, elevation comparison
//   * relaunchOutsideKillOnCloseJob: the escape hatch for being spawned by
//     CEP/Node inside a kill-on-close job
//
// Every handle is owned by a UniqueHandle; every Win32 return value is
// checked; every cross-process read is bounds-checked and falls back to
// "unknown" rather than guessing.
// ---------------------------------------------------------------------------
#include "platform/Process.h"

#include "core/Logger.h"
#include "platform/Utf.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"Process";

/// Size of the anonymous stdout/stderr pipe and of a single read.
constexpr DWORD kPipeBufferSize = 64u * 1024u;

/// Slice used by readChunk() so newly arrived output is noticed promptly
/// even though an anonymous pipe has no waitable "data ready" handle.
constexpr DWORD kPollSliceMs = 50;

/// Upper bound on captured output kept in memory by runCapture(). mkvmerge
/// prints progress lines, never megabytes; the cap only stops a runaway
/// child from exhausting memory.
constexpr size_t kMaxCapturedBytes = 32u * 1024u * 1024u;

/// Longest command line we will copy out of another process (bytes).
constexpr size_t kMaxRemoteCommandLineBytes = 64u * 1024u;

/// Longest image path we ask QueryFullProcessImageNameW for (chars).
constexpr DWORD kMaxImagePathChars = 32768;

/// Sentinel exit code reported when GetExitCodeProcess itself fails.
constexpr DWORD kExitCodeUnknown = 0xFFFFFFFFu;

/// Signature of ntdll!NtQueryInformationProcess, resolved at run time.
using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

/**
 * @brief RAII owner of a PROC_THREAD_ATTRIBUTE_LIST.
 */
struct AttributeList {
    std::vector<uint8_t> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    AttributeList() = default;
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    ~AttributeList() {
        if (list) {
            ::DeleteProcThreadAttributeList(list);
            list = nullptr;
        }
    }

    /**
     * @brief Allocates and initialises a list with room for @p count attributes.
     * @return false with GetLastError() set on failure
     */
    bool init(DWORD count) {
        // First call only reports the required size.
        SIZE_T bytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, count, 0, &bytes);
        if (bytes == 0) {
            return false;
        }
        storage.assign(bytes, 0);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!::InitializeProcThreadAttributeList(list, count, 0, &bytes)) {
            list = nullptr;
            return false;
        }
        return true;
    }
};

/**
 * @brief Walks a Toolhelp process snapshot, calling @p visit for each entry
 *        until it returns false.
 * @return false when the snapshot could not be taken
 */
bool forEachProcess(const std::function<bool(const PROCESSENTRY32W&)>& visit) {
    if (!visit) {
        return false;
    }
    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"CreateToolhelp32Snapshot failed: {} ({})", win32ErrorText(err), err);
        return false;
    }

    // Process32FirstW/NextW require dwSize to be set on every call.
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) {
        return false;
    }
    do {
        if (!visit(entry)) {
            break;
        }
        entry.dwSize = sizeof(entry);
    } while (::Process32NextW(snapshot.get(), &entry));
    return true;
}

/**
 * @brief Peeks the byte count waiting in an anonymous pipe.
 * @param pipe   read end
 * @param broken set when the write side is gone (ERROR_BROKEN_PIPE)
 * @return bytes available (0 on error)
 */
DWORD peekAvailable(HANDLE pipe, bool& broken) {
    broken = false;
    if (!pipe) {
        broken = true;
        return 0;
    }
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE) {
            broken = true;
        } else {
            HH_LOG_DEBUG(kLog, L"PeekNamedPipe failed: {} ({})", win32ErrorText(err), err);
        }
        return 0;
    }
    return available;
}

/**
 * @brief Reads up to @p available bytes (capped at one pipe buffer) into @p out.
 *
 * Only called when PeekNamedPipe reported data, so ReadFile cannot block.
 * @param broken set when the write side is gone
 * @return true when at least one byte was appended
 */
bool readAvailable(HANDLE pipe, DWORD available, std::string& out, bool& broken) {
    broken = false;
    if (!pipe || available == 0) {
        return false;
    }
    const DWORD toRead = std::min<DWORD>(available, kPipeBufferSize);
    std::string chunk(static_cast<size_t>(toRead), '\0');
    DWORD read = 0;
    if (!::ReadFile(pipe, chunk.data(), toRead, &read, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE) {
            broken = true;
        } else {
            HH_LOG_WARN(kLog, L"ReadFile on child pipe failed: {} ({})", win32ErrorText(err), err);
        }
        return false;
    }
    if (read == 0) {
        return false;
    }
    out.append(chunk.data(), std::min<size_t>(static_cast<size_t>(read), chunk.size()));
    return true;
}

/**
 * @brief Drains everything still buffered in the pipe (used once the child
 *        has exited). Bounded so a grandchild still holding the write end
 *        cannot keep us here forever.
 * @return true when any bytes were appended
 */
bool drainPipe(HANDLE pipe, std::string& out) {
    bool any = false;
    for (int guard = 0; guard < 4096; ++guard) {
        bool broken = false;
        const DWORD available = peekAvailable(pipe, broken);
        if (broken || available == 0) {
            break;
        }
        if (!readAvailable(pipe, available, out, broken)) {
            break;
        }
        any = true;
    }
    return any;
}

/**
 * @brief Full path of the running executable via GetModuleFileNameW,
 *        growing the buffer until it fits.
 */
std::wstring currentExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            const DWORD err = ::GetLastError();
            HH_LOG_WARN(kLog, L"GetModuleFileNameW failed: {} ({})", win32ErrorText(err), err);
            return {};
        }
        // A truncated result fills the buffer completely; grow and retry.
        if (static_cast<size_t>(len) < buffer.size() - 1) {
            return std::wstring(buffer.data(), len);
        }
        buffer.assign(buffer.size() * 2, L'\0');
    }
    return {};
}

/**
 * @brief Reads TokenElevation for a process handle.
 * @return false when the token could not be queried
 */
bool queryElevation(HANDLE process, bool& elevated) {
    elevated = false;
    if (!process) {
        return false;
    }
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &rawToken) || !rawToken) {
        return false;
    }
    UniqueHandle token(rawToken);
    TOKEN_ELEVATION info{};
    DWORD returned = 0;
    if (!::GetTokenInformation(token.get(), TokenElevation, &info, sizeof(info), &returned)) {
        return false;
    }
    elevated = (info.TokenIsElevated != 0);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Command-line quoting
// ---------------------------------------------------------------------------

/**
 * @brief Quotes one argument so CommandLineToArgvW / the CRT reproduce it.
 *
 * Rules (from "Parsing C command-line arguments"):
 *   * n backslashes followed by a quote -> 2n+1 backslashes + quote
 *   * n backslashes at the end (before the closing quote) -> 2n backslashes
 *   * backslashes not followed by a quote are literal
 *   * an empty argument becomes ""
 */
std::wstring quoteArgument(std::wstring_view arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    // Nothing that needs quoting: return verbatim.
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }

    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');

    size_t i = 0;
    while (i < arg.size()) {
        // Count the run of backslashes starting here.
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }

        if (i >= arg.size()) {
            // Trailing run before the closing quote: double it.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            // Run before a quote: double it and escape the quote.
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            // Run before an ordinary character: literal.
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
        ++i;
    }

    out.push_back(L'"');
    return out;
}

/**
 * @brief exe + args joined into one CreateProcessW command line.
 */
std::wstring buildCommandLine(std::wstring_view exe, const std::vector<std::wstring>& args) {
    std::wstring line = quoteArgument(exe);
    for (const std::wstring& arg : args) {
        line.push_back(L' ');
        line += quoteArgument(arg);
    }
    return line;
}

// ---------------------------------------------------------------------------
// ChildProcess
// ---------------------------------------------------------------------------

/**
 * @brief Releases the handles. Members are destroyed in reverse order, so
 *        the pipe closes first and then the job: with killOnJobClose the job
 *        close terminates a child that is still running, which is exactly
 *        the "child dies with us" guarantee.
 */
ChildProcess::~ChildProcess() = default;

/**
 * @brief Launches the child suspended, attaches the job, resumes it.
 *
 * Handle inheritance is restricted with PROC_THREAD_ATTRIBUTE_HANDLE_LIST
 * to exactly the stdio handles the child needs, so no stray handle from
 * another thread leaks into it (and keeps the pipe open after it exits).
 */
Result<void> ChildProcess::start(const ProcessSpec& spec) {
    if (spec.exe.empty()) {
        HH_LOG_ERROR(kLog, L"start(): empty executable path");
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"ChildProcess::start: empty exe");
    }
    if (process_) {
        HH_LOG_ERROR(kLog, L"start(): process already running (pid {})", pid_);
        return Error::fromWin32(ERROR_ALREADY_EXISTS, L"ChildProcess::start: already started");
    }
    commandLine_ = buildCommandLine(spec.exe, spec.args);

    // Inheritable security attributes for the child-side handles.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    // ---- stdout/stderr ----------------------------------------------------
    // With capture: an anonymous pipe whose read end stays private to us.
    // Without: a NUL sink so the child can write without ever blocking.
    UniqueHandle readEnd;
    UniqueHandle childOut;
    if (spec.captureOutput) {
        HANDLE rawRead = nullptr;
        HANDLE rawWrite = nullptr;
        if (!::CreatePipe(&rawRead, &rawWrite, &inheritable, kPipeBufferSize)) {
            const DWORD err = ::GetLastError();
            HH_LOG_ERROR(kLog, L"CreatePipe failed: {} ({})", win32ErrorText(err), err);
            return Error::fromWin32(err, L"ChildProcess: CreatePipe");
        }
        readEnd.reset(rawRead);
        childOut.reset(rawWrite);
        if (!::SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0)) {
            const DWORD err = ::GetLastError();
            HH_LOG_ERROR(kLog, L"SetHandleInformation on pipe read end failed: {} ({})", win32ErrorText(err), err);
            return Error::fromWin32(err, L"ChildProcess: SetHandleInformation");
        }
    } else {
        childOut.reset(::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                     OPEN_EXISTING, 0, nullptr));
        if (!childOut) {
            const DWORD err = ::GetLastError();
            HH_LOG_ERROR(kLog, L"CreateFileW(NUL, write) failed: {} ({})", win32ErrorText(err), err);
            return Error::fromWin32(err, L"ChildProcess: open NUL for output");
        }
    }

    // ---- stdin ------------------------------------------------------------
    // Always NUL: a child that tries to read input gets EOF instead of hanging.
    UniqueHandle childIn(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                       OPEN_EXISTING, 0, nullptr));
    if (!childIn) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"CreateFileW(NUL, read) failed: {} ({})", win32ErrorText(err), err);
        return Error::fromWin32(err, L"ChildProcess: open NUL for input");
    }

    // ---- inheritance list -------------------------------------------------
    AttributeList attributes;
    if (!attributes.init(1)) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"InitializeProcThreadAttributeList failed: {} ({})", win32ErrorText(err), err);
        return Error::fromWin32(err, L"ChildProcess: attribute list");
    }
    HANDLE inheritList[2] = {childOut.get(), childIn.get()};
    if (!::UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
                                     sizeof(inheritList), nullptr, nullptr)) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"UpdateProcThreadAttribute(HANDLE_LIST) failed: {} ({})", win32ErrorText(err), err);
        return Error::fromWin32(err, L"ChildProcess: handle list");
    }

    // ---- startup info -----------------------------------------------------
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.list;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childIn.get();
    startup.StartupInfo.hStdOutput = childOut.get();
    startup.StartupInfo.hStdError = childOut.get();
    if (spec.hideWindow) {
        startup.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
    }

    // Suspended so the job can be attached before the child runs a single
    // instruction (and before it could spawn anything of its own).
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    if (spec.hideWindow) {
        flags |= CREATE_NO_WINDOW;
    }
    if (spec.lowerPriority) {
        flags |= BELOW_NORMAL_PRIORITY_CLASS;
    }

    // CreateProcessW may modify the command line in place: give it a copy.
    std::vector<wchar_t> mutableLine(commandLine_.begin(), commandLine_.end());
    mutableLine.push_back(L'\0');
    const wchar_t* workingDir = spec.workingDirectory.empty() ? nullptr : spec.workingDirectory.c_str();

    PROCESS_INFORMATION info{};
    if (!::CreateProcessW(spec.exe.c_str(), mutableLine.data(), nullptr, nullptr, TRUE, flags, nullptr, workingDir,
                          &startup.StartupInfo, &info)) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"CreateProcessW({}) failed: {} ({})", spec.exe, win32ErrorText(err), err);
        return Error::fromWin32(err, L"CreateProcessW " + spec.exe);
    }
    process_.reset(info.hProcess);
    thread_.reset(info.hThread);
    pid_ = info.dwProcessId;

    // The child owns its copies now; ours must go so the pipe breaks when
    // the child exits (otherwise reads would never see EOF).
    childOut.reset();
    childIn.reset();

    // ---- job object -------------------------------------------------------
    if (spec.killOnJobClose) {
        UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
        if (!job) {
            const DWORD err = ::GetLastError();
            HH_LOG_WARN(kLog, L"CreateJobObjectW failed for pid {}: {} ({}); child will not be tied to us", pid_,
                        win32ErrorText(err), err);
        } else {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                const DWORD err = ::GetLastError();
                HH_LOG_WARN(kLog, L"SetInformationJobObject failed for pid {}: {} ({})", pid_, win32ErrorText(err), err);
            } else if (!::AssignProcessToJobObject(job.get(), process_.get())) {
                const DWORD err = ::GetLastError();
                HH_LOG_WARN(kLog, L"AssignProcessToJobObject failed for pid {}: {} ({}); child will not be tied to us",
                            pid_, win32ErrorText(err), err);
            } else {
                job_ = std::move(job);
            }
        }
    }

    // ---- go ---------------------------------------------------------------
    if (::ResumeThread(thread_.get()) == static_cast<DWORD>(-1)) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"ResumeThread failed for pid {}: {} ({})", pid_, win32ErrorText(err), err);
        ::TerminateProcess(process_.get(), 1);
        process_.reset();
        thread_.reset();
        job_.reset();
        pid_ = 0;
        return Error::fromWin32(err, L"ChildProcess: ResumeThread");
    }
    readPipe_ = std::move(readEnd);
    HH_LOG_DEBUG(kLog, L"started pid {}: {}", pid_, commandLine_);
    return {};
}

/**
 * @brief Reads whatever output is available, waiting up to @p waitMs.
 *
 * The anonymous pipe has no "data ready" event, so the wait is sliced:
 * between slices the pipe is peeked, which keeps latency low without
 * spinning. Once the process handle signals, the pipe is drained and the
 * leftover bytes are returned before Exited.
 */
ChildProcess::ReadStatus ChildProcess::readChunk(std::string& out, DWORD waitMs, HANDLE cancelEvent) {
    if (!process_) {
        HH_LOG_DEBUG(kLog, L"readChunk() without a running process");
        return ReadStatus::Error;
    }
    const size_t before = out.size();

    // Fast path: bytes already sitting in the pipe.
    if (readPipe_) {
        bool broken = false;
        const DWORD available = peekAvailable(readPipe_.get(), broken);
        if (broken) {
            // The write side is gone: the child is exiting (or has exited).
            // No more reads are possible; fall through to the process wait.
            readPipe_.reset();
        } else if (available > 0) {
            if (readAvailable(readPipe_.get(), available, out, broken)) {
                return ReadStatus::Data;
            }
            if (broken) {
                readPipe_.reset();
            }
        }
    }

    // Slow path: wait for exit / cancel in slices, peeking between them.
    const bool infinite = (waitMs == INFINITE);
    const uint64_t deadline = infinite ? 0 : ::GetTickCount64() + waitMs;
    HANDLE waitHandles[2] = {process_.get(), cancelEvent};
    const DWORD handleCount = cancelEvent ? 2u : 1u;

    for (;;) {
        // Work out how long this slice may last.
        DWORD slice = kPollSliceMs;
        if (!infinite) {
            const uint64_t now = ::GetTickCount64();
            const uint64_t remaining = (now >= deadline) ? 0 : (deadline - now);
            slice = static_cast<DWORD>(std::min<uint64_t>(remaining, kPollSliceMs));
        }

        const DWORD wait = ::WaitForMultipleObjects(handleCount, waitHandles, FALSE, slice);
        if (wait == WAIT_OBJECT_0) {
            // Process exited: hand back anything still buffered first.
            if (readPipe_) {
                drainPipe(readPipe_.get(), out);
            }
            return (out.size() > before) ? ReadStatus::Data : ReadStatus::Exited;
        }
        if (handleCount == 2 && wait == WAIT_OBJECT_0 + 1) {
            return ReadStatus::Cancelled;
        }
        if (wait != WAIT_TIMEOUT) {
            const DWORD err = ::GetLastError();
            HH_LOG_WARN(kLog, L"WaitForMultipleObjects on pid {} failed: {} ({})", pid_, win32ErrorText(err), err);
            return ReadStatus::Error;
        }

        // Slice elapsed: did output arrive meanwhile?
        if (readPipe_) {
            bool broken = false;
            const DWORD available = peekAvailable(readPipe_.get(), broken);
            if (broken) {
                readPipe_.reset();
            } else if (available > 0) {
                if (readAvailable(readPipe_.get(), available, out, broken)) {
                    return ReadStatus::Data;
                }
                if (broken) {
                    readPipe_.reset();
                }
            }
        }
        if (!infinite && ::GetTickCount64() >= deadline) {
            return ReadStatus::Timeout;
        }
    }
}

/**
 * @brief Waits for the child to exit.
 */
bool ChildProcess::waitForExit(DWORD waitMs) {
    if (!process_) {
        return false;
    }
    return ::WaitForSingleObject(process_.get(), waitMs) == WAIT_OBJECT_0;
}

/**
 * @brief True once the process handle is signalled.
 */
bool ChildProcess::exited() const {
    if (!process_) {
        return false;
    }
    return ::WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;
}

/**
 * @brief GetExitCodeProcess (STILL_ACTIVE while running, 0xFFFFFFFF when
 *        the query itself fails or nothing was started).
 */
DWORD ChildProcess::exitCode() const {
    if (!process_) {
        return kExitCodeUnknown;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(process_.get(), &code)) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"GetExitCodeProcess for pid {} failed: {} ({})", pid_, win32ErrorText(err), err);
        return kExitCodeUnknown;
    }
    return code;
}

/**
 * @brief TerminateProcess, best effort; a no-op once the child has exited.
 */
void ChildProcess::terminate(UINT exitCode) {
    if (!process_) {
        return;
    }
    if (exited()) {
        return;
    }
    if (!::TerminateProcess(process_.get(), exitCode)) {
        const DWORD err = ::GetLastError();
        // ERROR_ACCESS_DENIED here usually means it exited between the check and the call.
        HH_LOG_WARN(kLog, L"TerminateProcess(pid {}) failed: {} ({})", pid_, win32ErrorText(err), err);
        return;
    }
    HH_LOG_INFO(kLog, L"terminated pid {} with exit code {:#x}", pid_, exitCode);
}

// ---------------------------------------------------------------------------
// runCapture / launchDetached
// ---------------------------------------------------------------------------

/**
 * @brief Runs a process to completion and captures its output.
 *
 * The child is killed when @p timeoutMs elapses (timedOut = true); whatever
 * it printed until then is still returned so the caller can log it.
 */
Result<CaptureResult> runCapture(const std::wstring& exe, const std::vector<std::wstring>& args, DWORD timeoutMs,
                                 bool lowerPriority) {
    if (exe.empty()) {
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"runCapture: empty exe");
    }

    ProcessSpec spec;
    spec.exe = exe;
    spec.args = args;
    spec.captureOutput = true;
    spec.lowerPriority = lowerPriority;
    spec.killOnJobClose = true;
    spec.hideWindow = true;

    ChildProcess child;
    if (Result<void> started = child.start(spec); !started) {
        return started.error();
    }

    CaptureResult result;
    bool truncatedLogged = false;
    const bool infinite = (timeoutMs == INFINITE);
    const uint64_t startedAt = ::GetTickCount64();

    // Pump output until the child exits or the deadline passes.
    for (;;) {
        // Deadline check first so a chatty child cannot postpone it.
        const uint64_t elapsed = ::GetTickCount64() - startedAt;
        if (!infinite && elapsed >= timeoutMs) {
            HH_LOG_WARN(kLog, L"pid {} exceeded {} ms; terminating: {}", child.pid(), timeoutMs, child.commandLine());
            child.terminate();
            child.waitForExit(5000);
            result.timedOut = true;
            // Collect whatever it managed to print before the kill.
            std::string tail;
            child.readChunk(tail, 0, nullptr);
            if (!tail.empty() && result.output.size() < kMaxCapturedBytes) {
                result.output += tail;
            }
            break;
        }

        const DWORD wait = infinite ? 250u : static_cast<DWORD>(std::min<uint64_t>(timeoutMs - elapsed, 250u));
        std::string chunk;
        const ChildProcess::ReadStatus status = child.readChunk(chunk, wait, nullptr);

        // Append with a hard cap so a runaway child cannot exhaust memory.
        if (!chunk.empty()) {
            if (result.output.size() + chunk.size() <= kMaxCapturedBytes) {
                result.output += chunk;
            } else if (!truncatedLogged) {
                truncatedLogged = true;
                HH_LOG_WARN(kLog, L"pid {} output exceeded {} bytes; further output dropped", child.pid(),
                            kMaxCapturedBytes);
            }
        }

        if (status == ChildProcess::ReadStatus::Exited) {
            break;
        }
        if (status == ChildProcess::ReadStatus::Error) {
            // A read error with a live child is unrecoverable: stop it.
            if (child.exited()) {
                break;
            }
            HH_LOG_ERROR(kLog, L"output read failed for pid {}; terminating", child.pid());
            child.terminate();
            child.waitForExit(5000);
            return Error::fromWin32(ERROR_READ_FAULT, L"runCapture: reading child output");
        }
        // Data / Timeout / Cancelled(no event given): keep pumping.
    }

    result.exitCode = child.exitCode();
    HH_LOG_DEBUG(kLog, L"pid {} finished with exit code {} ({} bytes of output, timedOut={})", child.pid(),
                 result.exitCode, result.output.size(), result.timedOut);
    return result;
}

/**
 * @brief Launches a detached GUI process through the shell (no capture, no
 *        job): the new process outlives us.
 */
Result<void> launchDetached(const std::wstring& exe, const std::vector<std::wstring>& args) {
    if (exe.empty()) {
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"launchDetached: empty exe");
    }

    // Parameters are the quoted arguments joined by spaces.
    std::wstring parameters;
    for (const std::wstring& arg : args) {
        if (!parameters.empty()) {
            parameters.push_back(L' ');
        }
        parameters += quoteArgument(arg);
    }

    // ShellExecuteEx wants COM initialised on the calling thread.
    ScopedCoInit com;
    if (!com.ok()) {
        HH_LOG_WARN(kLog, L"CoInitializeEx failed before ShellExecuteEx: {:#010x}", static_cast<uint32_t>(com.hr()));
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&info)) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"ShellExecuteExW({}) failed: {} ({})", exe, win32ErrorText(err), err);
        return Error::fromWin32(err, L"ShellExecuteExW " + exe);
    }
    // We asked for the handle only to confirm a process was created.
    if (info.hProcess) {
        ::CloseHandle(info.hProcess);
        info.hProcess = nullptr;
    }
    HH_LOG_INFO(kLog, L"launched {} {}", exe, parameters);
    return {};
}

// ---------------------------------------------------------------------------
// Process queries
// ---------------------------------------------------------------------------

/**
 * @brief Full image path via QueryFullProcessImageNameW (works across
 *        elevation boundaries thanks to PROCESS_QUERY_LIMITED_INFORMATION).
 */
std::wstring processImagePath(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return {};
    }
    std::vector<wchar_t> buffer(kMaxImagePathChars, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!::QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &length)) {
        return {};
    }
    if (length == 0 || static_cast<size_t>(length) >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

/**
 * @brief Image file name only. Falls back to the Toolhelp snapshot when the
 *        process cannot be opened (protected or foreign-session processes).
 */
std::wstring processImageName(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    const std::wstring path = processImagePath(pid);
    if (!path.empty()) {
        const size_t slash = path.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    }

    // Toolhelp still lists the exe name for processes we cannot open.
    std::wstring name;
    forEachProcess([&](const PROCESSENTRY32W& entry) {
        if (entry.th32ProcessID == pid) {
            name.assign(entry.szExeFile, ::wcsnlen(entry.szExeFile, MAX_PATH));
            return false;
        }
        return true;
    });
    return name;
}

/**
 * @brief Parent process id from the Toolhelp snapshot (0 when unknown).
 */
DWORD parentProcessId(DWORD pid) {
    if (pid == 0) {
        return 0;
    }
    DWORD parent = 0;
    forEachProcess([&](const PROCESSENTRY32W& entry) {
        if (entry.th32ProcessID == pid) {
            parent = entry.th32ParentProcessID;
            return false;
        }
        return true;
    });
    return parent;
}

/**
 * @brief Command line of another process, read from its PEB.
 *
 * PEB -> ProcessParameters -> CommandLine (UNICODE_STRING). Every hop is a
 * bounds-checked ReadProcessMemory; any failure yields an empty string.
 */
std::wstring processCommandLine(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    UniqueHandle process(::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!process) {
        return {};
    }

    // Resolve NtQueryInformationProcess dynamically (documented as subject to change).
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return {};
    }
    const auto query =
        reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!query) {
        return {};
    }

    // Locate the PEB.
    PROCESS_BASIC_INFORMATION basic{};
    ULONG returned = 0;
    const NTSTATUS status = query(process.get(), ProcessBasicInformation, &basic, sizeof(basic), &returned);
    if (status < 0 || returned < sizeof(basic) || !basic.PebBaseAddress) {
        return {};
    }

    // PEB -> ProcessParameters.
    PEB peb{};
    SIZE_T read = 0;
    if (!::ReadProcessMemory(process.get(), basic.PebBaseAddress, &peb, sizeof(peb), &read) || read != sizeof(peb)) {
        return {};
    }
    if (!peb.ProcessParameters) {
        return {};
    }

    // ProcessParameters -> CommandLine.
    RTL_USER_PROCESS_PARAMETERS parameters{};
    if (!::ReadProcessMemory(process.get(), peb.ProcessParameters, &parameters, sizeof(parameters), &read) ||
        read != sizeof(parameters)) {
        return {};
    }
    const UNICODE_STRING& line = parameters.CommandLine;
    if (!line.Buffer || line.Length == 0) {
        return {};
    }

    // Copy the text (Length is in bytes); cap and round down to whole chars.
    size_t bytes = std::min<size_t>(static_cast<size_t>(line.Length), kMaxRemoteCommandLineBytes);
    bytes -= bytes % sizeof(wchar_t);
    if (bytes == 0) {
        return {};
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (!::ReadProcessMemory(process.get(), line.Buffer, text.data(), bytes, &read) || read != bytes) {
        return {};
    }
    // Trim at an embedded terminator if one slipped in.
    const size_t nul = text.find(L'\0');
    if (nul != std::wstring::npos) {
        text.resize(nul);
    }
    return text;
}

/**
 * @brief All pids whose image name matches (case-insensitive).
 */
std::vector<DWORD> findProcessesByImageName(std::wstring_view imageName) {
    std::vector<DWORD> pids;
    if (imageName.empty()) {
        return pids;
    }
    forEachProcess([&](const PROCESSENTRY32W& entry) {
        const std::wstring_view exe(entry.szExeFile, ::wcsnlen(entry.szExeFile, MAX_PATH));
        if (iequals(exe, imageName)) {
            pids.push_back(entry.th32ProcessID);
        }
        return true;
    });
    return pids;
}

/**
 * @brief All pids whose image name starts with @p prefix (case-insensitive).
 */
std::vector<DWORD> findProcessesByImagePrefix(std::wstring_view prefix) {
    std::vector<DWORD> pids;
    if (prefix.empty()) {
        return pids;
    }
    forEachProcess([&](const PROCESSENTRY32W& entry) {
        const std::wstring_view exe(entry.szExeFile, ::wcsnlen(entry.szExeFile, MAX_PATH));
        if (istartsWith(exe, prefix)) {
            pids.push_back(entry.th32ProcessID);
        }
        return true;
    });
    return pids;
}

/**
 * @brief SearchPathW over the default search order; the buffer grows once.
 */
std::wstring searchPath(std::wstring_view fileName) {
    if (fileName.empty() || fileName.find(L'\0') != std::wstring_view::npos) {
        return {};
    }
    const std::wstring name(fileName);
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD needed = ::SearchPathW(nullptr, name.c_str(), nullptr, static_cast<DWORD>(buffer.size()),
                                           buffer.data(), nullptr);
        if (needed == 0) {
            return {};
        }
        if (needed < buffer.size()) {
            return std::wstring(buffer.data(), needed);
        }
        buffer.assign(static_cast<size_t>(needed) + 1u, L'\0');
    }
    return {};
}

/**
 * @brief True when a process with that pid is alive.
 *
 * Opens the process and checks it has not signalled; when it cannot be
 * opened (protected process) the Toolhelp snapshot decides.
 */
bool isProcessAlive(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    UniqueHandle process(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (process) {
        return ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
    }
    const DWORD err = ::GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        // ERROR_INVALID_PARAMETER: no such process.
        return false;
    }

    // Exists but we may not open it: confirm via the snapshot.
    bool found = false;
    forEachProcess([&](const PROCESSENTRY32W& entry) {
        if (entry.th32ProcessID == pid) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

/**
 * @brief True when exactly one of {us, other} runs elevated. False whenever
 *        either token cannot be queried (no false alarms).
 */
bool elevationDiffers(DWORD otherPid) {
    if (otherPid == 0) {
        return false;
    }
    bool mine = false;
    if (!queryElevation(::GetCurrentProcess(), mine)) {
        return false;
    }
    UniqueHandle other(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, otherPid));
    if (!other) {
        return false;
    }
    bool theirs = false;
    if (!queryElevation(other.get(), theirs)) {
        return false;
    }
    return mine != theirs;
}

// ---------------------------------------------------------------------------
// Job-object escape
// ---------------------------------------------------------------------------

/**
 * @brief Relaunches ourselves outside a kill-on-close job when breakaway is
 *        allowed, so closing the CEP panel does not take HdrHint down.
 * @return true when a new instance was started (the caller must exit)
 */
bool relaunchOutsideKillOnCloseJob(const std::wstring& extraArg) {
    // Are we in a job at all?
    BOOL inJob = FALSE;
    if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &inJob)) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"IsProcessInJob failed: {} ({})", win32ErrorText(err), err);
        return false;
    }
    if (!inJob) {
        return false;
    }

    // Inspect the job's limits (nullptr = the job of the calling process).
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    DWORD returned = 0;
    if (!::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), &returned)) {
        const DWORD err = ::GetLastError();
        HH_LOG_WARN(kLog, L"QueryInformationJobObject failed: {} ({})", win32ErrorText(err), err);
        return false;
    }
    const DWORD limitFlags = limits.BasicLimitInformation.LimitFlags;
    if ((limitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) == 0) {
        HH_LOG_DEBUG(kLog, L"in a job without KILL_ON_JOB_CLOSE (flags {:#x}); staying put", limitFlags);
        return false;
    }
    const bool breakawayOk = (limitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) != 0;
    const bool silentBreakawayOk = (limitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) != 0;
    if (!breakawayOk && !silentBreakawayOk) {
        HH_LOG_WARN(kLog, L"in a kill-on-close job that forbids breakaway (flags {:#x}); cannot relaunch", limitFlags);
        return false;
    }

    // Guard against relaunching in a loop: if the marker is already on our
    // command line this is the relaunched copy (nested jobs can do that).
    const wchar_t* currentLine = ::GetCommandLineW();
    std::wstring commandLine = currentLine ? currentLine : L"";
    if (!extraArg.empty() && commandLine.find(extraArg) != std::wstring::npos) {
        HH_LOG_WARN(kLog, L"relaunch marker already present on the command line; not relaunching again");
        return false;
    }
    if (commandLine.empty()) {
        commandLine = quoteArgument(currentExecutablePath());
    }
    if (!extraArg.empty()) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(extraArg);
    }

    const std::wstring self = currentExecutablePath();
    if (self.empty()) {
        return false;
    }

    // CREATE_BREAKAWAY_FROM_JOB needs BREAKAWAY_OK; with only
    // SILENT_BREAKAWAY_OK the child leaves the job on its own.
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (breakawayOk) {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!::CreateProcessW(self.c_str(), mutableLine.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup,
                          &info)) {
        const DWORD err = ::GetLastError();
        HH_LOG_ERROR(kLog, L"relaunch outside job failed: {} ({})", win32ErrorText(err), err);
        return false;
    }
    if (info.hThread) {
        ::CloseHandle(info.hThread);
    }
    if (info.hProcess) {
        ::CloseHandle(info.hProcess);
    }
    HH_LOG_INFO(kLog, L"relaunched outside the kill-on-close job as pid {}", info.dwProcessId);
    return true;
}

} // namespace hh::platform
