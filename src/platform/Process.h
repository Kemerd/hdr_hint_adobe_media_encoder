// ---------------------------------------------------------------------------
// Process.h - child processes, command-line quoting and process queries.
//
// Windows: CreateProcessW + job objects + Toolhelp / NtQueryInformationProcess.
// macOS:   posix_spawn + a kqueue EVFILT_PROC exit watch + libproc / sysctl.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Event.h"
#include "platform/Win.h"

#if defined(_WIN32)
#include "platform/Handle.h"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace hh::platform {

/// Exit code reported by ChildProcess::exitCode() while the child still runs.
inline constexpr DWORD kStillActiveExitCode = 259;   // == STILL_ACTIVE

/**
 * @brief Quotes one argument for this platform's shell.
 *
 * Windows: the MSVC/CommandLineToArgvW rules. Arguments without
 * spaces/tabs/quotes are returned unchanged; otherwise the argument is
 * wrapped in quotes and backslash runs before quotes are doubled.
 * POSIX: plain words pass through; anything else is single-quoted for
 * sh/zsh, with every embedded single quote closed, escaped and reopened.
 */
std::wstring quoteArgument(std::wstring_view arg);

/// exe + args joined into one command line (for display and "copy command").
std::wstring buildCommandLine(std::wstring_view exe, const std::vector<std::wstring>& args);

/**
 * @brief Finds an executable the way the shell would.
 *
 * Windows: SearchPathW (application folder, system folders, PATH).
 * macOS: PATH plus the Homebrew / MacPorts prefixes - apps started from the
 * Finder get a minimal PATH that never includes /opt/homebrew/bin.
 * @return the full path, or empty when not found
 */
std::wstring searchPath(std::wstring_view fileName);

/**
 * @brief Everything needed to launch a child.
 */
struct ProcessSpec {
    std::wstring exe;                     ///< full path of the executable
    std::vector<std::wstring> args;       ///< arguments (unquoted)
    std::wstring workingDirectory;        ///< empty = inherit
    bool captureOutput = true;            ///< pipe stdout+stderr back to us
    bool lowerPriority = false;           ///< BELOW_NORMAL_PRIORITY_CLASS / nice +10
    bool killOnJobClose = true;           ///< child dies with us (job object; POSIX: killed when the ChildProcess goes away)
    bool hideWindow = true;               ///< CREATE_NO_WINDOW (no meaning on POSIX)
};

/**
 * @brief A running child process with an optional captured output pipe.
 *
 * Output is read with a wait so a stalled child can be abandoned: readChunk()
 * returns when data arrives, the process exits, the cancel event fires, or
 * the timeout elapses.
 */
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Launches the child (Windows: suspended -> job object -> resumed).
    Result<void> start(const ProcessSpec& spec);

    /// Result of one readChunk() call.
    enum class ReadStatus { Data, Timeout, Cancelled, Exited, Error };

    /**
     * @brief Reads whatever output is available.
     * @param out          receives raw bytes (UTF-8 for mkvmerge)
     * @param waitMs       how long to wait for data
     * @param cancelEvent  optional event handle that aborts the wait (kInvalidWaitHandle = none)
     */
    ReadStatus readChunk(std::string& out, DWORD waitMs, WaitHandle cancelEvent);

    /// Waits for exit (INFINITE allowed). Returns false on timeout.
    bool waitForExit(DWORD waitMs);
    /// True once the child has exited.
    [[nodiscard]] bool exited() const;
    /// Exit code after exit (kStillActiveExitCode while running).
    [[nodiscard]] DWORD exitCode() const;
    /// Terminates the child (best effort) with the given exit code.
    void terminate(UINT exitCode = 0xC000013Au);
    /// Process id.
    [[nodiscard]] DWORD pid() const noexcept { return pid_; }
#if defined(_WIN32)
    /// Process handle (for waits).
    [[nodiscard]] HANDLE processHandle() const noexcept { return process_.get(); }
#endif
    /// The exact command line that was launched.
    [[nodiscard]] const std::wstring& commandLine() const noexcept { return commandLine_; }

private:
#if defined(_WIN32)
    UniqueHandle process_;
    UniqueHandle thread_;
    UniqueHandle job_;
    UniqueHandle readPipe_;
#else
    /// Collects the child's status once it has exited. True once reaped.
    bool reap(bool block) const;
    /// Moves whatever sits in the pipe into @p out without blocking.
    bool drainAvailable(std::string& out);

    int readFd_ = -1;                     ///< our end of the stdout+stderr pipe
    int exitQueue_ = -1;                  ///< kqueue with EVFILT_PROC/NOTE_EXIT (readable once the child exits)
    bool killOnClose_ = true;             ///< terminate a still-running child in the destructor
    mutable bool reaped_ = false;         ///< waitpid() collected the status
    mutable int status_ = 0;              ///< raw waitpid() status
    DWORD forcedExitCode_ = 0;            ///< code requested by terminate()
    bool terminated_ = false;             ///< terminate() was called
#endif
    std::wstring commandLine_;
    DWORD pid_ = 0;
};

/**
 * @brief Output of runCapture().
 */
struct CaptureResult {
    DWORD exitCode = 0;
    std::string output;      ///< stdout + stderr, raw bytes (UTF-8 for our tools)
    bool timedOut = false;
};

/// Runs a process to completion and captures its output (killed on timeout).
Result<CaptureResult> runCapture(const std::wstring& exe, const std::vector<std::wstring>& args,
                                 DWORD timeoutMs, bool lowerPriority = false);

/// Launches a detached GUI process (ShellExecuteExW "open"; macOS: LaunchServices for a .app), no capture.
Result<void> launchDetached(const std::wstring& exe, const std::vector<std::wstring>& args);

// ---- process queries -------------------------------------------------------

/// Image file name (e.g. "Adobe Media Encoder.exe"; macOS "Adobe Media Encoder 2026") of a process, or empty.
std::wstring processImageName(DWORD pid);
/// Full image path of a process, or empty.
std::wstring processImagePath(DWORD pid);
/// Parent process id (0 when unknown).
DWORD parentProcessId(DWORD pid);
/// Command line of a process (same user; empty when unreadable).
std::wstring processCommandLine(DWORD pid);
/// All pids whose image name matches (case-insensitive).
std::vector<DWORD> findProcessesByImageName(std::wstring_view imageName);
/// All pids whose image name starts with @p prefix (case-insensitive) - AME's name carries its year on macOS.
std::vector<DWORD> findProcessesByImagePrefix(std::wstring_view prefix);
/// True when a process with that pid is alive.
bool isProcessAlive(DWORD pid);
/// True when the other process runs elevated and we do not (or vice versa). POSIX: root vs non-root.
bool elevationDiffers(DWORD otherPid);

#if defined(_WIN32)
/**
 * @brief Job-object escape for exes spawned by CEP/Node.
 *
 * If the current process is inside a job that kills on close and breakaway is
 * permitted, relaunches itself with CREATE_BREAKAWAY_FROM_JOB and returns
 * true (the caller must exit). Returns false when nothing needs doing.
 */
bool relaunchOutsideKillOnCloseJob(const std::wstring& extraArg);
#endif

} // namespace hh::platform
