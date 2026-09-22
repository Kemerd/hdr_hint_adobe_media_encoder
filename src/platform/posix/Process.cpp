// ---------------------------------------------------------------------------
// posix/Process.cpp - child processes and process queries on macOS.
//
// Children
//   posix_spawn with:
//     * one pipe for stdout + stderr (the child's copy is dup2'd onto 1 and 2),
//     * stdin from /dev/null,
//     * POSIX_SPAWN_CLOEXEC_DEFAULT so *no* other descriptor leaks into
//       mkvmerge (our event pipes, sockets, log file...),
//     * SIGPIPE restored to its default so a child writing into a closed
//       pipe dies the normal way even though this process ignores SIGPIPE.
//   Exit is observed through a kqueue with EVFILT_PROC / NOTE_EXIT. A kqueue
//   descriptor is itself pollable, so readChunk() waits on output, exit and
//   the caller's cancel event in one waitAny() - the same shape as the
//   Windows version's WaitForMultipleObjects on process + cancel.
//
// Queries
//   libproc (proc_pidpath, proc_pidinfo, proc_listallpids) and sysctl
//   KERN_PROCARGS2, which is how ps(1) and Activity Monitor do it.
// ---------------------------------------------------------------------------
#include "platform/Process.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace hh::platform {

using posix::closeQuietly;
using posix::fromNative;
using posix::toNative;

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"Process";

/// Read size per chunk from the output pipe (kept off-heap, so modest).
constexpr size_t kPipeBufferSize = 16u * 1024u;

/// Slice used by readChunk() so a missed kqueue wake-up costs at most this much.
constexpr DWORD kPollSliceMs = 50;

/// Hard cap on what runCapture() keeps in memory from a runaway child.
constexpr size_t kMaxCapturedBytes = 32u * 1024u * 1024u;

/// How long terminate() gives SIGTERM before escalating to SIGKILL.
constexpr uint64_t kTerminateGraceMs = 750;

/**
 * @brief The environment block handed to every child.
 *
 * _NSGetEnviron() is the supported way to reach it from code that may end
 * up in a shared library; a plain "extern char** environ" is not.
 */
char** childEnvironment() noexcept {
#if defined(__APPLE__)
    return *::_NSGetEnviron();
#else
    extern char** environ;
    return environ;
#endif
}

/**
 * @brief Marks a descriptor non-blocking and close-on-exec.
 */
bool makeNonBlockingCloexec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    const int fdFlags = ::fcntl(fd, F_GETFD, 0);
    return fdFlags >= 0 && ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) >= 0;
}

/**
 * @brief Creates a kqueue that becomes readable when @p pid exits.
 * @return the kqueue descriptor, or -1 (ESRCH: the child already exited)
 */
int watchExit(pid_t pid) noexcept {
    const int kq = ::kqueue();
    if (kq < 0) {
        return -1;
    }
    ::fcntl(kq, F_SETFD, FD_CLOEXEC);
    struct kevent change {};
    EV_SET(&change, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (::kevent(kq, &change, 1, nullptr, 0, nullptr) < 0) {
        ::close(kq);
        return -1;
    }
    return kq;
}

/// Characters that never need quoting for sh/zsh.
bool isShellSafe(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' ||
           c == L'-' || c == L'.' || c == L'/' || c == L',' || c == L':' || c == L'=' || c == L'+' || c == L'@' ||
           c == L'%';
}

/// argv as UTF-8 strings plus the NULL-terminated pointer array posix_spawn wants.
struct NativeArgv {
    std::vector<std::string> storage;
    std::vector<char*> pointers;

    void build(const std::wstring& exe, const std::vector<std::wstring>& args) {
        storage.clear();
        storage.reserve(args.size() + 1);
        storage.push_back(toNative(exe));
        for (const std::wstring& a : args) {
            storage.push_back(toUtf8(a));
        }
        pointers.clear();
        for (std::string& s : storage) {
            pointers.push_back(s.data());
        }
        pointers.push_back(nullptr);
    }
};

#if defined(__APPLE__)
/// Every pid on the system (best effort; the list may be stale by the time it is used).
std::vector<pid_t> allPids() {
    const int estimate = ::proc_listallpids(nullptr, 0);
    if (estimate <= 0) {
        return {};
    }
    std::vector<pid_t> pids(static_cast<size_t>(estimate) + 64);
    const int n = ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (n <= 0) {
        return {};
    }
    pids.resize(std::min(static_cast<size_t>(n), pids.size()));
    return pids;
}

/// proc_bsdinfo for a pid (parent, uid, name). False when the process is gone or foreign.
bool bsdInfo(pid_t pid, proc_bsdinfo& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    return ::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &out, sizeof(out)) == static_cast<int>(sizeof(out));
}
#endif

} // namespace

// ===========================================================================
// Quoting
// ===========================================================================

/**
 * @brief Single-quotes for sh unless the argument is made of safe characters.
 */
std::wstring quoteArgument(std::wstring_view arg) {
    if (!arg.empty() && std::all_of(arg.begin(), arg.end(), isShellSafe)) {
        return std::wstring(arg);
    }
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'\'');
    for (const wchar_t c : arg) {
        if (c == L'\'') {
            out.append(L"'\\''");   // close, escaped quote, reopen
        } else {
            out.push_back(c);
        }
    }
    out.push_back(L'\'');
    return out;
}

std::wstring buildCommandLine(std::wstring_view exe, const std::vector<std::wstring>& args) {
    std::wstring line = quoteArgument(exe);
    for (const std::wstring& a : args) {
        line.push_back(L' ');
        line.append(quoteArgument(a));
    }
    return line;
}

/**
 * @brief PATH lookup plus the package-manager prefixes Finder apps never see.
 */
std::wstring searchPath(std::wstring_view fileName) {
    if (fileName.empty() || fileName.find(L'\0') != std::wstring_view::npos) {
        return {};
    }
    // A name with a slash is a path already.
    if (fileName.find(L'/') != std::wstring_view::npos) {
        const std::string native = toNative(fileName);
        return (::access(native.c_str(), X_OK) == 0) ? fullPath(fileName) : std::wstring();
    }

    std::vector<std::string> dirs;
    if (const char* path = ::getenv("PATH"); path != nullptr) {
        std::string list(path);
        size_t start = 0;
        while (start <= list.size()) {
            const size_t colon = list.find(':', start);
            const size_t end = (colon == std::string::npos) ? list.size() : colon;
            if (end > start) {
                dirs.push_back(list.substr(start, end - start));
            }
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
    }
    // Homebrew (Apple silicon, then Intel), MacPorts, then the system folders.
    for (const char* extra : {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin", "/bin"}) {
        if (std::find(dirs.begin(), dirs.end(), extra) == dirs.end()) {
            dirs.emplace_back(extra);
        }
    }

    const std::string name = toNative(fileName);
    for (const std::string& dir : dirs) {
        const std::string candidate = dir + "/" + name;
        struct stat st {};
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(candidate.c_str(), X_OK) == 0) {
            return fromNative(candidate);
        }
    }
    return {};
}

// ===========================================================================
// ChildProcess
// ===========================================================================

/**
 * @brief Kills a child that must not outlive us, then releases everything.
 */
ChildProcess::~ChildProcess() {
    if (pid_ != 0 && !reap(false) && killOnClose_) {
        terminate();
        waitForExit(2000);
    }
    closeQuietly(readFd_);
    closeQuietly(exitQueue_);
}

/**
 * @brief posix_spawn with a captured stdout+stderr pipe.
 */
Result<void> ChildProcess::start(const ProcessSpec& spec) {
    if (pid_ != 0) {
        return Error::text(L"ChildProcess::start: already started");
    }
    if (spec.exe.empty()) {
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"ChildProcess::start: empty exe");
    }
    commandLine_ = buildCommandLine(spec.exe, spec.args);
    killOnClose_ = spec.killOnJobClose;

    // ---- output pipe --------------------------------------------------------
    int pipeFds[2] = {-1, -1};
    if (spec.captureOutput) {
        if (::pipe(pipeFds) != 0) {
            return Error::fromErrno(errno, L"pipe");
        }
        // Our read end must not leak into the child; the write end is dup2'd.
        makeNonBlockingCloexec(pipeFds[0]);
        ::fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
    }

    // ---- file actions: stdin </dev/null, stdout/stderr -> pipe --------------
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (spec.captureOutput) {
        ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
    } else {
        ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    std::string workingDir;
    if (!spec.workingDirectory.empty()) {
        workingDir = toNative(spec.workingDirectory);
#if defined(__APPLE__)
        if (__builtin_available(macOS 10.15, *)) {
            ::posix_spawn_file_actions_addchdir_np(&actions, workingDir.c_str());
        }
#endif
    }

    // ---- attributes: no leaked descriptors, default signals -------------------
    posix_spawnattr_t attrs;
    ::posix_spawnattr_init(&attrs);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    ::posix_spawnattr_setflags(&attrs, flags);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    ::posix_spawnattr_setsigdefault(&attrs, &defaults);
    sigset_t noMask;
    sigemptyset(&noMask);
    ::posix_spawnattr_setsigmask(&attrs, &noMask);

    // ---- spawn ---------------------------------------------------------------
    NativeArgv argv;
    argv.build(spec.exe, spec.args);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, argv.storage.front().c_str(), &actions, &attrs, argv.pointers.data(), childEnvironment());
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attrs);
    if (spec.captureOutput) {
        closeQuietly(pipeFds[1]);   // only the child writes now; EOF arrives when it exits
    }
    if (rc != 0) {
        closeQuietly(pipeFds[0]);
        return Error::fromErrno(rc, L"posix_spawn " + commandLine_);
    }
    pid_ = static_cast<DWORD>(pid);
    readFd_ = pipeFds[0];

    // ---- priority + exit watch ---------------------------------------------------
    if (spec.lowerPriority) {
        // BELOW_NORMAL_PRIORITY_CLASS is roughly nice +10.
        ::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 10);
    }
    exitQueue_ = watchExit(pid);   // -1 when the child already exited; reap() covers that
    HH_LOG_DEBUG(kLog, L"started pid {}: {}", pid_, commandLine_);
    return {};
}

/**
 * @brief Collects the exit status with waitpid (once).
 */
bool ChildProcess::reap(bool block) const {
    if (reaped_) {
        return true;
    }
    if (pid_ == 0) {
        return false;
    }
    int status = 0;
    pid_t r = 0;
    do {
        r = ::waitpid(static_cast<pid_t>(pid_), &status, block ? 0 : WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == static_cast<pid_t>(pid_)) {
        reaped_ = true;
        status_ = status;
        return true;
    }
    if (r < 0 && errno == ECHILD) {
        // Someone else reaped it (should not happen); treat as exited, code unknown.
        reaped_ = true;
        status_ = 0;
        return true;
    }
    return false;
}

/**
 * @brief Non-blocking read of everything buffered in the pipe.
 * @return true when bytes were appended
 */
bool ChildProcess::drainAvailable(std::string& out) {
    if (readFd_ < 0) {
        return false;
    }
    bool any = false;
    char buffer[kPipeBufferSize];
    for (;;) {
        const ssize_t n = ::read(readFd_, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, static_cast<size_t>(n));
            any = true;
            if (out.size() > kMaxCapturedBytes * 2) {
                break;   // leave the rest for the next call rather than balloon
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0) {
            closeQuietly(readFd_);   // EOF: the child (and its children) closed the pipe
        }
        break;   // EAGAIN or an error: nothing more right now
    }
    return any;
}

/**
 * @brief Output, exit or cancel - whichever comes first (see the header).
 */
ChildProcess::ReadStatus ChildProcess::readChunk(std::string& out, DWORD waitMs, WaitHandle cancelEvent) {
    if (pid_ == 0) {
        HH_LOG_DEBUG(kLog, L"readChunk() without a running process");
        return ReadStatus::Error;
    }
    // Fast path: bytes already sitting in the pipe.
    if (drainAvailable(out)) {
        return ReadStatus::Data;
    }

    const bool infinite = (waitMs == INFINITE);
    const uint64_t deadline = infinite ? 0 : nowMonotonicMs() + waitMs;
    for (;;) {
        // Exited? Hand back anything still buffered first, exactly like Windows.
        if (reap(false)) {
            return drainAvailable(out) ? ReadStatus::Data : ReadStatus::Exited;
        }

        // Work out how long this slice may last.
        DWORD slice = kPollSliceMs;
        if (!infinite) {
            const uint64_t now = nowMonotonicMs();
            const uint64_t remaining = (now >= deadline) ? 0 : (deadline - now);
            slice = static_cast<DWORD>(std::min<uint64_t>(remaining, kPollSliceMs));
        }

        // Wait on output, exit and cancel together.
        WaitHandle set[3];
        size_t count = 0;
        int outputIndex = -1;
        int exitIndex = -1;
        int cancelIndex = -1;
        if (readFd_ >= 0) { outputIndex = static_cast<int>(count); set[count++] = readFd_; }
        if (exitQueue_ >= 0) { exitIndex = static_cast<int>(count); set[count++] = exitQueue_; }
        if (cancelEvent != kInvalidWaitHandle) { cancelIndex = static_cast<int>(count); set[count++] = cancelEvent; }

        DWORD r = kWaitTimeout;
        if (count > 0) {
            r = waitAny(set, count, slice);
        } else if (slice > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        }
        if (r == kWaitFailed) {
            HH_LOG_WARN(kLog, L"wait on pid {} failed: {}", pid_, Error::fromErrno(errno, L"poll").toString());
            return ReadStatus::Error;
        }
        if (cancelIndex >= 0 && r == static_cast<DWORD>(cancelIndex)) {
            return ReadStatus::Cancelled;
        }
        if (outputIndex >= 0 && r == static_cast<DWORD>(outputIndex)) {
            if (drainAvailable(out)) {
                return ReadStatus::Data;
            }
            continue;   // EOF: the descriptor is closed now; keep waiting for the exit
        }
        if (exitIndex >= 0 && r == static_cast<DWORD>(exitIndex)) {
            closeQuietly(exitQueue_);   // one-shot: the exit has been observed
            reap(true);
            return drainAvailable(out) ? ReadStatus::Data : ReadStatus::Exited;
        }
        if (!infinite && nowMonotonicMs() >= deadline) {
            return ReadStatus::Timeout;
        }
    }
}

bool ChildProcess::waitForExit(DWORD waitMs) {
    if (pid_ == 0) {
        return true;
    }
    if (reap(false)) {
        return true;
    }
    if (waitMs == INFINITE) {
        return reap(true);
    }
    // Sleep on the exit kqueue when there is one; poll otherwise.
    const uint64_t deadline = nowMonotonicMs() + waitMs;
    for (;;) {
        if (reap(false)) {
            return true;
        }
        const uint64_t now = nowMonotonicMs();
        if (now >= deadline) {
            return false;
        }
        const DWORD slice = static_cast<DWORD>(std::min<uint64_t>(deadline - now, kPollSliceMs));
        if (exitQueue_ >= 0) {
            const WaitHandle h = exitQueue_;
            if (waitAny(&h, 1, slice) == 0) {
                closeQuietly(exitQueue_);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        }
    }
}

bool ChildProcess::exited() const {
    return pid_ != 0 && reap(false);
}

/**
 * @brief Win32-style exit code: the status for a normal exit, the code
 *        terminate() asked for when we killed it, 128+signal otherwise.
 */
DWORD ChildProcess::exitCode() const {
    if (pid_ == 0 || !reap(false)) {
        return kStillActiveExitCode;
    }
    if (WIFEXITED(status_)) {
        return static_cast<DWORD>(WEXITSTATUS(status_));
    }
    if (WIFSIGNALED(status_)) {
        if (terminated_) {
            return forcedExitCode_;
        }
        return 128u + static_cast<DWORD>(WTERMSIG(status_));
    }
    return kStillActiveExitCode;
}

/**
 * @brief SIGTERM, a short grace period, then SIGKILL.
 */
void ChildProcess::terminate(UINT exitCode) {
    if (pid_ == 0 || reap(false)) {
        return;
    }
    terminated_ = true;
    forcedExitCode_ = static_cast<DWORD>(exitCode);
    const pid_t pid = static_cast<pid_t>(pid_);
    ::kill(pid, SIGTERM);
    const uint64_t deadline = nowMonotonicMs() + kTerminateGraceMs;
    while (nowMonotonicMs() < deadline) {
        if (reap(false)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(pid, SIGKILL);
}

// ===========================================================================
// One-shot helpers
// ===========================================================================

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
    const uint64_t startedAt = nowMonotonicMs();

    // Pump output until the child exits or the deadline passes.
    for (;;) {
        // Deadline check first so a chatty child cannot postpone it.
        const uint64_t elapsed = nowMonotonicMs() - startedAt;
        if (!infinite && elapsed >= timeoutMs) {
            HH_LOG_WARN(kLog, L"pid {} exceeded {} ms; terminating: {}", child.pid(), timeoutMs, child.commandLine());
            child.terminate();
            child.waitForExit(5000);
            result.timedOut = true;
            // Collect whatever it managed to print before the kill.
            std::string tail;
            child.readChunk(tail, 0, kInvalidWaitHandle);
            if (!tail.empty() && result.output.size() < kMaxCapturedBytes) {
                result.output += tail;
            }
            break;
        }

        const DWORD wait = infinite ? 250u : static_cast<DWORD>(std::min<uint64_t>(timeoutMs - elapsed, 250u));
        std::string chunk;
        const ChildProcess::ReadStatus status = child.readChunk(chunk, wait, kInvalidWaitHandle);

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
            if (child.exited()) {
                break;
            }
            HH_LOG_ERROR(kLog, L"output read failed for pid {}; terminating", child.pid());
            child.terminate();
            child.waitForExit(5000);
            return Error::fromWin32(ERROR_NOT_READY, L"runCapture: reading child output");
        }
        // Data / Timeout: keep pumping.
    }

    result.exitCode = child.exitCode();
    HH_LOG_DEBUG(kLog, L"pid {} finished with exit code {} ({} bytes of output, timedOut={})", child.pid(),
                 result.exitCode, result.output.size(), result.timedOut);
    return result;
}

/**
 * @brief Starts a program we never wait for.
 *
 * A .app bundle goes through LaunchServices ("open -a"), which is what the
 * Finder does and what keeps the app out of our process group. Anything
 * else is spawned directly and reaped by a detached thread so it never
 * lingers as a zombie.
 */
Result<void> launchDetached(const std::wstring& exe, const std::vector<std::wstring>& args) {
    if (exe.empty()) {
        return Error::fromWin32(ERROR_INVALID_PARAMETER, L"launchDetached: empty exe");
    }
    std::wstring program = exe;
    std::vector<std::wstring> argv = args;
    std::wstring trimmed = exe;
    while (trimmed.size() > 1 && trimmed.back() == L'/') {
        trimmed.pop_back();
    }
    if (iendsWith(trimmed, L".app")) {
        program = L"/usr/bin/open";
        argv.clear();
        argv.push_back(L"-a");
        argv.push_back(trimmed);
        if (!args.empty()) {
            argv.push_back(L"--args");
            argv.insert(argv.end(), args.begin(), args.end());
        }
    }

    NativeArgv native;
    native.build(program, argv);
    posix_spawnattr_t attrs;
    ::posix_spawnattr_init(&attrs);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    ::posix_spawnattr_setflags(&attrs, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
#else
    ::posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
#endif
    ::posix_spawnattr_setpgroup(&attrs, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, native.storage.front().c_str(), nullptr, &attrs, native.pointers.data(), childEnvironment());
    ::posix_spawnattr_destroy(&attrs);
    if (rc != 0) {
        return Error::fromErrno(rc, L"posix_spawn " + program);
    }
    // Reap it whenever it ends; the thread holds nothing else.
    try {
        std::thread([pid] {
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }).detach();
    } catch (...) {
        // Out of threads: the child still runs; at worst it lingers as a zombie until we exit.
    }
    return {};
}

// ===========================================================================
// Process queries
// ===========================================================================

std::wstring processImagePath(DWORD pid) {
#if defined(__APPLE__)
    if (pid == 0) {
        return {};
    }
    char buffer[PROC_PIDPATHINFO_MAXSIZE] = {};
    const int n = ::proc_pidpath(static_cast<int>(pid), buffer, sizeof(buffer));
    if (n <= 0) {
        return {};
    }
    return fromNative(std::string(buffer, static_cast<size_t>(n)));
#else
    static_cast<void>(pid);
    return {};
#endif
}

std::wstring processImageName(DWORD pid) {
    const std::wstring path = processImagePath(pid);
    if (!path.empty()) {
        const size_t slash = path.find_last_of(L'/');
        return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    }
#if defined(__APPLE__)
    // No path (another user's process): the short name still identifies it.
    char name[2 * MAXCOMLEN + 1] = {};
    if (pid != 0 && ::proc_name(static_cast<int>(pid), name, sizeof(name)) > 0) {
        return fromNative(name);
    }
#endif
    return {};
}

DWORD parentProcessId(DWORD pid) {
#if defined(__APPLE__)
    proc_bsdinfo info {};
    if (pid != 0 && bsdInfo(static_cast<pid_t>(pid), info)) {
        return static_cast<DWORD>(info.pbi_ppid);
    }
#else
    static_cast<void>(pid);
#endif
    return 0;
}

/**
 * @brief argv of another process via KERN_PROCARGS2 (same user only).
 *
 * Layout: int argc, the exec path, NUL padding, then argc NUL-terminated
 * strings (followed by the environment, which is ignored).
 */
std::wstring processCommandLine(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    int argMax = 0;
    size_t size = sizeof(argMax);
    int mibMax[2] = {CTL_KERN, KERN_ARGMAX};
    if (::sysctl(mibMax, 2, &argMax, &size, nullptr, 0) != 0 || argMax <= 0) {
        return {};
    }
    std::vector<char> buffer(static_cast<size_t>(argMax));
    size = buffer.size();
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    if (::sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) {
        return {};
    }
    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof(argc));
    size_t pos = sizeof(argc);
    // Skip the exec path and its padding.
    while (pos < size && buffer[pos] != '\0') { ++pos; }
    while (pos < size && buffer[pos] == '\0') { ++pos; }

    std::wstring line;
    for (int i = 0; i < argc && pos < size; ++i) {
        const size_t start = pos;
        while (pos < size && buffer[pos] != '\0') { ++pos; }
        if (!line.empty()) { line.push_back(L' '); }
        line.append(quoteArgument(toWide(std::string_view(buffer.data() + start, pos - start))));
        ++pos;
    }
    return line;
}

std::vector<DWORD> findProcessesByImageName(std::wstring_view imageName) {
    std::vector<DWORD> pids;
#if defined(__APPLE__)
    if (imageName.empty()) {
        return pids;
    }
    // Windows callers pass "name.exe"; macOS executables carry no extension.
    std::wstring_view wanted = imageName;
    if (iendsWith(wanted, L".exe")) {
        wanted.remove_suffix(4);
    }
    for (const pid_t pid : allPids()) {
        if (pid <= 0) { continue; }
        const std::wstring name = processImageName(static_cast<DWORD>(pid));
        if (!name.empty() && iequals(name, wanted)) {
            pids.push_back(static_cast<DWORD>(pid));
        }
    }
#else
    static_cast<void>(imageName);
#endif
    return pids;
}

std::vector<DWORD> findProcessesByImagePrefix(std::wstring_view prefix) {
    std::vector<DWORD> pids;
#if defined(__APPLE__)
    if (prefix.empty()) {
        return pids;
    }
    for (const pid_t pid : allPids()) {
        if (pid <= 0) { continue; }
        const std::wstring name = processImageName(static_cast<DWORD>(pid));
        if (!name.empty() && istartsWith(name, prefix)) {
            pids.push_back(static_cast<DWORD>(pid));
        }
    }
#else
    static_cast<void>(prefix);
#endif
    return pids;
}

bool isProcessAlive(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    // Signal 0 checks existence; EPERM means it exists but belongs to someone else.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

bool elevationDiffers(DWORD otherPid) {
#if defined(__APPLE__)
    proc_bsdinfo info {};
    if (otherPid == 0 || !bsdInfo(static_cast<pid_t>(otherPid), info)) {
        return false;
    }
    const bool otherRoot = (info.pbi_uid == 0);
    const bool selfRoot = (::geteuid() == 0);
    return otherRoot != selfRoot;
#else
    static_cast<void>(otherPid);
    return false;
#endif
}

} // namespace hh::platform
