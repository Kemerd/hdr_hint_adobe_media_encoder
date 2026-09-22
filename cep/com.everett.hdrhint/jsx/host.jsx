/* ---------------------------------------------------------------------------
 * HDR Hint bridge - runs inside Adobe Media Encoder's ExtendScript engine.
 *
 * Loaded twice on purpose: once by the manifest's <ScriptPath> when the panel
 * starts, and again by panel.js through $.evalFile() in case ScriptPath was
 * not honoured. The engine outlives the panel, so a re-evaluation must NOT
 * re-register listeners (they would fire twice); instead it re-announces the
 * existing bridge so a freshly opened panel learns what is registered.
 *
 * Everything here is ES3: var only, no arrow functions, no trailing commas,
 * no JSON object (json2.js may or may not be loaded before us).
 * ------------------------------------------------------------------------- */
(function () {

    // ---- re-evaluation guard ------------------------------------------------
    // A previous evaluation in this AME session already installed the bridge.
    // Re-announce (so the panel sees bridgeReady) and stop.
    var existing = null;
    try { existing = $.global.__hdrHintBridge; } catch (eGuard) { existing = null; }
    if (existing && typeof existing.version === "number" && existing.version >= 1) {
        try {
            if (typeof existing.announce === "function") { existing.announce(); }
        } catch (eAnnounce) { /* nothing sensible to do at load time */ }
        return;
    }

    var BRIDGE_VERSION = 1;
    var EVENT_TYPE = "com.everett.hdrhint.ame";
    var registered = [];      // "target.eventName" for every listener that took
    var errors = [];          // human-readable failures, surfaced in the panel

    // ---- tiny JSON encoder ----------------------------------------------------
    // Character-loop escaper: ExtendScript's regex replace is slow and has
    // quirks with control characters, and we must not depend on json2.js.
    function esc(value) {
        var s = String(value);
        var out = "";
        var i, c, code;
        for (i = 0; i < s.length; i++) {
            c = s.charAt(i);
            code = s.charCodeAt(i);
            if (c === '"') { out += '\\"'; }
            else if (c === "\\") { out += "\\\\"; }
            else if (c === "\n") { out += "\\n"; }
            else if (c === "\r") { out += "\\r"; }
            else if (c === "\t") { out += "\\t"; }
            else if (code < 32) {
                // Remaining control characters as \u00XX.
                var hex = code.toString(16);
                while (hex.length < 4) { hex = "0" + hex; }
                out += "\\u" + hex;
            }
            else { out += c; }
        }
        return out;
    }

    // Recursive encoder for the small payloads we send: primitives, arrays,
    // plain objects. Depth-capped so a cyclic host object can never hang AME.
    function stringify(value, depth) {
        var d = (typeof depth === "number") ? depth : 0;
        if (d > 4) { return '"[depth]"'; }
        if (value === undefined || value === null) { return "null"; }
        var t = typeof value;
        if (t === "number") {
            // NaN / Infinity are not valid JSON; send null instead.
            if (isNaN(value) || value === Infinity || value === -Infinity) { return "null"; }
            return String(value);
        }
        if (t === "boolean") { return value ? "true" : "false"; }
        if (t === "string") { return '"' + esc(value) + '"'; }
        if (t === "function") { return "null"; }
        if (value instanceof Array) {
            var items = [];
            var i;
            for (i = 0; i < value.length; i++) { items.push(stringify(value[i], d + 1)); }
            return "[" + items.join(",") + "]";
        }
        if (t === "object") {
            var parts = [];
            var k;
            for (k in value) {
                if (!value.hasOwnProperty(k)) { continue; }
                if (typeof value[k] === "function") { continue; }
                parts.push('"' + esc(k) + '":' + stringify(value[k], d + 1));
            }
            return "{" + parts.join(",") + "}";
        }
        // Host objects and anything exotic: stringify defensively.
        try { return '"' + esc(String(value)) + '"'; } catch (eStr) { return "null"; }
    }

    // ---- CSXS event plumbing --------------------------------------------------
    // PlugPlugExternalObject is what lets ExtendScript raise CSXS events that
    // CEP panels receive with CSInterface.addEventListener().
    var plug = null;
    function ensurePlug() {
        if (plug) { return true; }
        try {
            plug = new ExternalObject("lib:PlugPlugExternalObject");
        } catch (ePlug) {
            plug = null;
            errors.push("PlugPlugExternalObject: " + ePlug);
        }
        return !!plug;
    }
    ensurePlug();

    var seq = 0;
    function dispatch(type, fields) {
        if (!ensurePlug()) { return false; }
        try {
            var payload = fields || {};
            payload.type = type;
            payload.seq = ++seq;
            payload.t = new Date().getTime();
            var ev = new CSXSEvent();
            ev.type = EVENT_TYPE;
            ev.data = stringify(payload, 0);
            ev.dispatch();
            return true;
        } catch (eDispatch) {
            errors.push("dispatch " + type + ": " + eDispatch);
            return false;
        }
    }

    // ---- safe field readers ---------------------------------------------------
    // AME's event objects are host objects; reading a missing property can
    // throw on some builds, so every read goes through these.
    function fieldStr(obj, name) {
        try {
            if (obj && obj[name] !== undefined && obj[name] !== null) { return String(obj[name]); }
        } catch (eField) { /* fall through */ }
        return "";
    }
    function fieldNum(obj, name, fallback) {
        try {
            if (obj && obj[name] !== undefined && obj[name] !== null) {
                var n = Number(obj[name]);
                if (!isNaN(n)) { return n; }
            }
        } catch (eField) { /* fall through */ }
        return fallback;
    }

    // ---- listener registration ------------------------------------------------
    // getTarget is a function so a missing DOM object (older AME) becomes an
    // entry in errors[] instead of an exception at load time.
    function safeAdd(getTarget, label, name, fn, capture) {
        try {
            var target = getTarget();
            if (target && typeof target.addEventListener === "function") {
                target.addEventListener(name, fn, !!capture);
                registered.push(label + "." + name);
                return true;
            }
            errors.push(label + "." + name + ": target has no addEventListener");
        } catch (eAdd) {
            errors.push(label + "." + name + ": " + eAdd);
        }
        return false;
    }

    function hostApp() {
        if (typeof app === "undefined" || !app) { throw "app is undefined"; }
        return app;
    }
    function encoderHost() { return hostApp().getEncoderHost(); }
    function exporter() { return hostApp().getExporter(); }

    // ---- encoder host events --------------------------------------------------
    // Item finished (success or failure; 'result' vocabulary is opaque to us).
    function onItemEncodeComplete(eventObj) {
        dispatch("encodeComplete", {
            result: fieldStr(eventObj, "result"),
            sourceFilePath: fieldStr(eventObj, "sourceFilePath"),
            outputFilePath: fieldStr(eventObj, "outputFilePath")
        });
    }

    // Item started; the app pairs later batchItemStatus events with this path.
    function onItemEncodingStarted(eventObj) {
        dispatch("encodingStarted", {
            sourceFilePath: fieldStr(eventObj, "sourceFilePath"),
            outputFilePath: fieldStr(eventObj, "outputFilePath")
        });
    }

    // Progress fires many times per second; CSXS events are not free, so we
    // throttle to roughly one per second.
    var lastProgressAt = 0;
    function onEncodingItemProgressUpdate(eventObj) {
        var now = new Date().getTime();
        if (now - lastProgressAt < 1000) { return; }
        lastProgressAt = now;
        dispatch("progress", {
            progress: fieldNum(eventObj, "progress", -1),
            sourceFilePath: fieldStr(eventObj, "sourceFilePath"),
            outputFilePath: fieldStr(eventObj, "outputFilePath")
        });
    }

    // Queue state: invalid / paused / running / stopped / stopping.
    function onBatchEncoderStatusChanged(eventObj) {
        dispatch("queueStatus", {
            batchEncoderStatus: fieldStr(eventObj, "batchEncoderStatus")
        });
    }

    // ---- exporter events ------------------------------------------------------
    // Per-item status by (groupIndex, itemIndex); status is the documented
    // enum (0 Waiting, 1 Done, 2 Failed, 3 Skipped, 4 Encoding, 5 Paused,
    // 6 Stopped, 7 Any, 8 AutoStart, 9 Done Warning, 10 Watch Folder Waiting).
    function onBatchItemStatusChanged(eventObj) {
        dispatch("batchItemStatus", {
            groupIndex: fieldNum(eventObj, "groupIndex", -1),
            itemIndex: fieldNum(eventObj, "itemIndex", -1),
            status: fieldNum(eventObj, "status", -1)
        });
    }

    // Exporter-level error: forward whatever text the host attached.
    function onExporterError(eventObj) {
        var message = fieldStr(eventObj, "message");
        if (!message) { message = fieldStr(eventObj, "error"); }
        if (!message && eventObj) {
            try { message = String(eventObj); } catch (eMsg) { message = ""; }
        }
        dispatch("error", { message: message });
    }

    // ---- app-level capture-phase event ---------------------------------------
    // AME's own mapping script listens for this on 'app' with capture=true.
    function onEncodeFinished(eventObj) {
        dispatch("encodeFinished", {
            result: fieldStr(eventObj, "result"),
            outputFilePath: fieldStr(eventObj, "outputFilePath")
        });
    }

    // Register everything. Each call is independently guarded so one missing
    // event name never prevents the others from working.
    safeAdd(encoderHost, "encoderHost", "onItemEncodeComplete", onItemEncodeComplete, false);
    safeAdd(encoderHost, "encoderHost", "onItemEncodingStarted", onItemEncodingStarted, false);
    safeAdd(encoderHost, "encoderHost", "onEncodingItemProgressUpdate", onEncodingItemProgressUpdate, false);
    safeAdd(encoderHost, "encoderHost", "onBatchEncoderStatusChanged", onBatchEncoderStatusChanged, false);
    safeAdd(exporter, "exporter", "onBatchItemStatusChanged", onBatchItemStatusChanged, false);
    safeAdd(exporter, "exporter", "onError", onExporterError, false);
    safeAdd(hostApp, "app", "onEncodeFinished", onEncodeFinished, true);

    // ---- announce + launch helpers -------------------------------------------
    function ameVersion() {
        try { return String(hostApp().version); } catch (eVer) { return ""; }
    }

    // Re-dispatches bridgeReady from the stored state. Called at install and
    // again on every re-evaluation (panel reopened, DevTools reload).
    function announce() {
        ensurePlug();
        return dispatch("bridgeReady", {
            bridgeVersion: BRIDGE_VERSION,
            registered: registered,
            errors: errors,
            ameVersion: ameVersion()
        });
    }

    // macOS: Launch Services cannot pass the app an argument, so leave the
    // "launch-request" marker it consumes to know Media Encoder started it.
    function markLaunchRequest() {
        var file = null;
        try {
            var base = Folder.userData ? String(Folder.userData.fsName) : "";
            if (!base) { return; }
            var folder = new Folder(base + "/HdrHint");
            if (!folder.exists) { folder.create(); }
            file = new File(folder.fsName + "/launch-request");
            file.encoding = "UTF-8";
            if (file.open("w")) {
                file.write(String(new Date().getTime()));
                file.close();
            }
        } catch (eMark) {
            try { if (file) { file.close(); } } catch (eClose) {}
        }
    }

    // Launch fallback: File.execute() is a ShellExecute from AME's own process
    // (Launch Services on macOS), outside CEF's process tree (and any job
    // object CEF may have put the panel's Node in). Path may use forward or
    // back slashes; on macOS it is usually an .app bundle (a folder).
    function launch(path) {
        try {
            if (!path) { return false; }
            var isMac = String(Folder.fs) === "Macintosh";
            var f = new File(String(path));
            if (!f.exists && !(isMac && new Folder(String(path)).exists)) { return false; }
            if (isMac) { markLaunchRequest(); }
            return f.execute();
        } catch (eLaunch) {
            return false;
        }
    }

    $.global.__hdrHintBridge = {
        version: BRIDGE_VERSION,
        registered: registered,
        errors: errors,
        announce: announce,
        launch: launch
    };

    announce();
})();
