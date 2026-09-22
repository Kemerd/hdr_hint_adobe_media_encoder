/**
 * HdrHintLauncher.jsx - starts HDR Hint when Adobe Media Encoder starts.
 *
 * Media Encoder evaluates every .jsx in its Scripts\Startup folder once, in
 * file-name order, while the application comes up. This script finds the
 * HdrHint executable (HdrHint.app on macOS) and launches it, so the companion
 * window is there for the whole session without anything being registered
 * with the operating system.
 *
 * It is deliberately defensive: Media Encoder shares one ExtendScript engine
 * between all startup scripts, so an uncaught error here could stop the
 * scripts that come after it. Everything is wrapped, nothing throws out, and
 * the script never blocks application start.
 *
 * Lookup order for the executable:
 *   1. launcher.json ("exePath", written by the app on every run)
 *        Windows  %APPDATA%\HdrHint\launcher.json
 *        macOS    ~/Library/Application Support/HdrHint/launcher.json
 *   2. HdrHintExePath.txt next to this script (written by the installer)
 *
 * macOS: File.execute() opens the .app through Launch Services and cannot
 * pass arguments, so the script first touches
 * ~/Library/Application Support/HdrHint/launch-request; the app consumes
 * that marker to know Media Encoder started it (quiet start, leaves with AME).
 *
 * The registry is not read: ExtendScript has no registry API, and shelling
 * out to reg.exe at startup would be slower and more fragile than the two
 * files the app already maintains.
 */
(function hdrHintLauncher() {
    // A second evaluation (script reload, AME re-running startup) must not
    // start a second copy. The app itself is single-instance, but skipping
    // early keeps the log clean.
    if ($.global.__hdrHintLauncherRan) {
        return;
    }
    $.global.__hdrHintLauncherRan = true;

    /** True on macOS (ExtendScript reports the file system as "Macintosh"). */
    var isMac = (String(Folder.fs) === "Macintosh");
    /** Path separator of this platform. */
    var SEP = isMac ? "/" : "\\";

    /** Reads a whole UTF-8 file, or "" when it cannot be read. */
    function readText(path) {
        var file = null;
        try {
            file = new File(path);
            if (!file.exists) {
                return "";
            }
            file.encoding = "UTF-8";
            if (!file.open("r")) {
                return "";
            }
            var text = file.read();
            file.close();
            return String(text || "");
        } catch (eRead) {
            // A locked or unreadable file is not worth reporting at startup.
            try { if (file) { file.close(); } } catch (eClose) {}
            return "";
        }
    }

    /**
     * Pulls "exePath" out of launcher.json without a JSON parser.
     *
     * json2.js is not loaded in the startup engine and eval() on a file we
     * did not write would be reckless, so this reads the one string value
     * with a regular expression and unescapes the backslashes.
     */
    function exePathFromJson(text) {
        if (!text) {
            return "";
        }
        var match = /"exePath"\s*:\s*"((?:[^"\\]|\\.)*)"/.exec(text);
        if (!match || !match[1]) {
            return "";
        }
        // JSON escapes every backslash in a Windows path (and may escape
        // the slashes of a macOS one); undo that.
        return String(match[1]).replace(/\\\\/g, "\\").replace(/\\"/g, "\"").replace(/\\\//g, "/");
    }

    /** Full path of the folder this script lives in (empty when unknown). */
    function scriptFolder() {
        try {
            var self = new File($.fileName);
            if (self && self.parent) {
                return String(self.parent.fsName);
            }
        } catch (eSelf) {}
        return "";
    }

    /** First existing executable from the two known locations. */
    function findExe() {
        var candidates = [];

        // 1. launcher.json, refreshed by the app on every run.
        try {
            var roaming = Folder.userData ? String(Folder.userData.fsName) : "";
            if (roaming) {
                candidates.push(exePathFromJson(readText(roaming + SEP + "HdrHint" + SEP + "launcher.json")));
            }
        } catch (eRoaming) {}

        // 2. The path stamped next to this script at install time.
        var folder = scriptFolder();
        if (folder) {
            var stamped = readText(folder + SEP + "HdrHintExePath.txt");
            // Strip a BOM and any trailing newline the writer may have left.
            candidates.push(String(stamped).replace(/^﻿/, "").replace(/[\r\n]+$/, ""));
        }

        for (var i = 0; i < candidates.length; ++i) {
            var candidate = candidates[i];
            if (!candidate) {
                continue;
            }
            try {
                var exe = new File(candidate);
                if (exe.exists) {
                    return exe;
                }
                // An .app bundle is a folder; File.execute() still opens it.
                if (isMac && new Folder(candidate).exists) {
                    return exe;
                }
            } catch (eCandidate) {}
        }
        return null;
    }

    /**
     * macOS: leaves the "launch-request" marker the app consumes on start
     * (Launch Services cannot pass it an argument). Best effort.
     */
    function markLaunchRequest() {
        var file = null;
        try {
            var base = Folder.userData ? String(Folder.userData.fsName) : "";
            if (!base) {
                return;
            }
            var folder = new Folder(base + SEP + "HdrHint");
            if (!folder.exists) {
                folder.create();
            }
            file = new File(folder.fsName + SEP + "launch-request");
            file.encoding = "UTF-8";
            if (file.open("w")) {
                file.write(String(new Date().getTime()));
                file.close();
            }
        } catch (eMark) {
            try { if (file) { file.close(); } } catch (eClose) {}
        }
    }

    try {
        var exe = findExe();
        if (!exe) {
            return;   // not installed: stay silent, AME keeps starting
        }
        if (isMac) {
            markLaunchRequest();
        }
        // File.execute() is a ShellExecute from Media Encoder's own process.
        // The app is single-instance: a second launch just forwards its
        // arguments to the copy that is already running, so this is safe
        // even when HDR Hint was started by hand beforehand.
        exe.execute();
    } catch (eLaunch) {
        // Never let a launch problem interfere with Media Encoder's startup.
    }
})();
