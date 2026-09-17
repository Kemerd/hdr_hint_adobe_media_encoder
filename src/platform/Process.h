// ---------------------------------------------------------------------------
// Process.h - child processes, command-line quoting and process queries.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Handle.h"
#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hh::platform {

/**
 * @brief Quotes one argument per the MSVC/CommandLineToArgvW rules.
 *
 * Arguments without spaces/tabs/quotes are returned unchanged; otherwise the
 * argument is wrapped in quotes and backslash runs before quotes are doubled.
 */
std::wstring quoteArgument(std::wstring_view arg);

/// exe + args joined into one CreateProcessW command line.
std::wstring buildCommandLine(std::wstring_view exe, const std::vector<std::wstring>& args);

/**
 * @brief Everything needed to launch a child.
 */
struct ProcessSpec {
    std::wstring exe;                     ///< full path of the executable
    std::vector<std::wstring> args;       ///< arguments (unquoted)
    std::wstring workingDirectory;        ///< empty = inherit
    bool captureOutput = true;            ///< pipe stdout+stderr back to us
    bool lowerPriority = false;           ///< BELOW_NORMAL_PRIORITY_CLASS
    bool killOnJobClose = true;           ///< child dies with us (job object)
    bool hideWindow = true;               ///< CREATE_NO_WINDOW
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

    /// Launches the child (suspended -> job object -> resumed).
    Result<void> start(const ProcessSpec& spec);

    /// Result of one readChunk() call.
    enum class ReadStatus { Data, Timeout, Cancelled, Exited, Error };

    /**
     * @brief Reads whatever output is available.
     * @param out          receives raw bytes (UTF-8 for mkvmerge)
     * @param waitMs       how long to wait for data
     * @param cancelEvent  optional event that aborts the wait
     */
    ReadStatus readChunk(std::string& out, DWORD waitMs, HANDLE cancelEvent);

    /// Waits for exit (INFINITE allowed). Returns false on timeout.
    bool waitForExit(DWORD waitMs);
    /// True once the child has exited.
    [[nodiscard]] bool exited() const;
    /// Exit code after exit (STILL_ACTIVE while running).
    [[nodiscard]] DWORD exitCode() const;
    /// Terminates the child (best effort) with the given exit code.
    void terminate(UINT exitCode = 0xC000013Au);
    /// Process id.
    [[nodiscard]] DWORD pid() const noexcept { return pid_; }
    /// Process handle (for waits).
    [[nodiscard]] HANDLE processHandle() const noexcept { return process_.get(); }
    /// The exact command line that was launched.
    [[nodiscard]] const std::wstring& commandLine() const noexcept { return commandLine_; }

private:
    UniqueHandle process_;
    UniqueHandle thread_;
    UniqueHandle job_;
    UniqueHandle readPipe_;
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

/// Launches a detached GUI process (ShellExecuteExW "open"), no capture.
Result<void> launchDetached(const std::wstring& exe, const std::vector<std::wstring>& args);

// ---- process queries -------------------------------------------------------

/// Image file name (e.g. "Adobe Media Encoder.exe") of a process, or empty.
std::wstring processImageName(DWORD pid);
/// Full image path of a process, or empty.
std::wstring processImagePath(DWORD pid);
/// Parent process id (0 when unknown).
DWORD parentProcessId(DWORD pid);
/// Command line of a process (same user; empty when unreadable).
std::wstring processCommandLine(DWORD pid);
/// All pids whose image name matches (case-insensitive).
std::vector<DWORD> findProcessesByImageName(std::wstring_view imageName);
/// True when a process with that pid is alive.
bool isProcessAlive(DWORD pid);
/// True when the other process runs elevated and we do not (or vice versa).
bool elevationDiffers(DWORD otherPid);

/**
 * @brief Job-object escape for exes spawned by CEP/Node.
 *
 * If the current process is inside a job that kills on close and breakaway is
 * permitted, relaunches itself with CREATE_BREAKAWAY_FROM_JOB and returns
 * true (the caller must exit). Returns false when nothing needs doing.
 */
bool relaunchOutsideKillOnCloseJob(const std::wstring& extraArg);

} // namespace hh::platform
