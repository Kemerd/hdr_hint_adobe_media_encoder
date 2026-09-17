/* ---------------------------------------------------------------------------
 * HDR Hint CEP panel - panel.js
 *
 * Runs inside AME's CEPHtmlEngine (CEF 99 / Node 17, mixed context). It has
 * four jobs:
 *
 *   1. Pipe client   - NDJSON over \\.\pipe\HdrHint to the native app, with
 *                      reconnect backoff, a small outbound ring buffer and a
 *                      5-second ping.
 *   2. Launcher      - when the pipe does not exist, find HdrHint.exe and
 *                      start it (Node spawn, ExtendScript File.execute as the
 *                      fallback when CEF's job object kills the child).
 *   3. Event bridge  - forwards ExtendScript CSXS events (host.jsx) and CEP
 *                      lifecycle events to the app as {kind:"ame"} messages.
 *   4. Bounds        - reports the panel's screen rectangle so the app can
 *                      dock its window over us before it finds our HWND.
 *
 * Every CSInterface / Node call is wrapped: a missing API must degrade to a
 * diagnostics line, never to a blank panel. No network access of any kind.
 * ------------------------------------------------------------------------- */
(function () {
    'use strict';

    // ---- constants ------------------------------------------------------------
    const PANEL_VERSION = '1.0.0';
    const DEFAULT_PIPE = '\\\\.\\pipe\\HdrHint';
    const PIPE_PREFIX = '\\\\.\\pipe\\';
    const EVENT_TYPE = 'com.everett.hdrhint.ame';
    const CSXS_EVENTS = {
        beforeQuit: 'com.adobe.csxs.events.ApplicationBeforeQuit',
        extensionUnloaded: 'com.adobe.csxs.events.ExtensionUnloaded',
        workspaceChanged: 'com.adobe.csxs.events.WorkspaceChanged',
        visibilityChanged: 'com.adobe.csxs.events.WindowVisibilityChanged'
    };

    // Reconnect schedule (sticky at the last entry), outbound buffering and
    // liveness. The app closes a client that stays silent for 15 s, so we ping
    // at 5 s and treat 20 s without any inbound byte as a dead pipe.
    const BACKOFF_MS = [250, 500, 1000, 2000, 5000];
    const RING_MAX = 200;
    const PING_MS = 5000;
    const STALE_LINK_MS = 20000;
    const MAX_LINE_BYTES = 1024 * 1024;

    // Launcher pacing: never auto-spawn more than once per 10 s; when no exe
    // can be found at all, wait a minute before probing the disk again.
    const LAUNCH_MIN_INTERVAL_MS = 10000;
    const NOT_INSTALLED_RETRY_MS = 60000;
    const CHILD_GRACE_MS = 5000;

    // Bounds reporter cadence.
    const BOUNDS_POLL_MS = 100;
    const BOUNDS_STABLE_TICKS = 10;
    const BOUNDS_HEARTBEAT_MS = 1000;

    const LOG_MAX_LINES = 200;

    // ---- diagnostics log ------------------------------------------------------
    // A bounded list rendered into <pre id="log"> on a short timer so a burst
    // of messages does not thrash the DOM. Also mirrored to the DevTools console.
    const logLines = [];
    let logRenderPending = false;

    function timeStamp() {
        try {
            const d = new Date();
            const two = (n) => (n < 10 ? '0' : '') + n;
            return two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds());
        } catch (_) {
            return '--:--:--';
        }
    }

    function log(message) {
        try {
            const line = timeStamp() + ' ' + String(message);
            logLines.push(line);
            while (logLines.length > LOG_MAX_LINES) { logLines.shift(); }
            try { console.log('[HdrHint] ' + line); } catch (_) { /* no console */ }
            if (!logRenderPending) {
                logRenderPending = true;
                setTimeout(renderLog, 50);
            }
        } catch (_) { /* logging must never throw */ }
    }

    function renderLog() {
        logRenderPending = false;
        const el = ui.log;
        if (!el) { return; }
        try {
            // Keep the view pinned to the bottom unless the user scrolled up.
            const atBottom = (el.scrollTop + el.clientHeight) >= (el.scrollHeight - 4);
            el.textContent = logLines.join('\n');
            if (atBottom) { el.scrollTop = el.scrollHeight; }
        } catch (_) { /* detached element */ }
    }

    // ---- DOM references -------------------------------------------------------
    function byId(id) {
        try { return document.getElementById(id); } catch (_) { return null; }
    }

    const ui = {
        dot: byId('dot'),
        statusText: byId('statusText'),
        detailText: byId('detailText'),
        bridgeText: byId('bridgeText'),
        btnLaunch: byId('btnLaunch'),
        btnShow: byId('btnShow'),
        btnDock: byId('btnDock'),
        btnLocate: byId('btnLocate'),
        btnCopyLog: byId('btnCopyLog'),
        btnClearLog: byId('btnClearLog'),
        log: byId('log')
    };

    // ---- helpers --------------------------------------------------------------
    function num(v) {
        const n = Number(v);
        return Number.isFinite(n) ? n : 0;
    }

    function clampByte(v) {
        const n = Math.round(num(v));
        return n < 0 ? 0 : (n > 255 ? 255 : n);
    }

    function hex2(n) {
        const s = clampByte(n).toString(16);
        return s.length < 2 ? '0' + s : s;
    }

    // Windows-style path normalisation: forward slashes -> backslashes, no
    // trailing separator. Used for everything handed to Node or the exe.
    function toBackslashes(p) {
        return String(p || '').replace(/\//g, '\\').replace(/[\\]+$/, '');
    }

    // ExtendScript prefers forward slashes inside string literals; also escape
    // quotes and backslashes so the path survives being embedded in a script.
    function toJsxLiteral(p) {
        return String(p || '').replace(/\\/g, '/').replace(/"/g, '\\"');
    }

    // ---- CSInterface + Node bootstrap ----------------------------------------
    let cs = null;
    try {
        if (typeof CSInterface === 'function') { cs = new CSInterface(); }
        else { log('CSInterface.js did not load'); }
    } catch (e) {
        log('CSInterface: ' + e);
    }

    // In mixed context Node lives on window.cep_node; older engines exposed a
    // plain global require. Try both, and treat neither as a fatal bridge error.
    function resolveNodeRequire() {
        try {
            if (window.cep_node && typeof window.cep_node.require === 'function') { return window.cep_node.require; }
        } catch (_) { /* fall through */ }
        try {
            if (typeof window.require === 'function') { return window.require; }
        } catch (_) { /* fall through */ }
        return null;
    }

    const nodeRequire = resolveNodeRequire();

    function tryRequire(name) {
        if (!nodeRequire) { return null; }
        try { return nodeRequire(name); } catch (e) { log('require(' + name + '): ' + e); return null; }
    }

    const net = tryRequire('net');
    const cp = tryRequire('child_process');
    const fs = tryRequire('fs');
    const nodePath = tryRequire('path');

    function resolveNodeProcess() {
        try {
            if (window.cep_node && window.cep_node.process) { return window.cep_node.process; }
        } catch (_) { /* fall through */ }
        try {
            if (typeof process !== 'undefined' && process && process.env) { return process; }
        } catch (_) { /* fall through */ }
        return tryRequire('process');
    }

    const nodeProcess = resolveNodeProcess();

    function envVar(name) {
        try {
            if (nodeProcess && nodeProcess.env && typeof nodeProcess.env[name] === 'string') { return nodeProcess.env[name]; }
        } catch (_) { /* fall through */ }
        return '';
    }

    function joinPath(a, b) {
        try { if (nodePath) { return nodePath.join(a, b); } } catch (_) { /* fall through */ }
        return toBackslashes(a) + '\\' + String(b || '');
    }

    // ---- panel state ----------------------------------------------------------
    const state = {
        status: 'starting',       // starting | connecting | launching | linked | reconnecting | notinstalled | bridgeerror
        fatalBridgeError: '',     // set when the pipe can never work (no Node)
        docked: true,
        appVersion: '',
        detail: '',
        extPath: '',              // backslashes, for Node and the exe
        extPathFwd: '',           // forward slashes, for ExtendScript literals
        extensionId: '',
        pipeName: DEFAULT_PIPE,
        autoLaunch: true,
        exeOverride: '',          // path chosen via Locate... in this session
        skin: { panelBg: '#232323', isDark: true, appName: '', appVersion: '' },
        bridge: { registered: [], errors: [], seen: false, loadError: '' }
    };

    // ---- status rendering -----------------------------------------------------
    const STATUS_VIEW = {
        starting: { dot: 'dot-grey', text: 'Starting\u2026' },
        connecting: { dot: 'dot-blue dot-pulse', text: 'Connecting\u2026' },
        launching: { dot: 'dot-amber dot-pulse', text: 'Launching\u2026' },
        linked: { dot: 'dot-green', text: 'Linked' },
        reconnecting: { dot: 'dot-amber dot-pulse', text: 'Reconnecting\u2026' },
        notinstalled: { dot: 'dot-red', text: 'Not installed' },
        bridgeerror: { dot: 'dot-red', text: 'Bridge error (see diagnostics)' }
    };

    function setStatus(status, detail) {
        if (!STATUS_VIEW[status]) { status = 'starting'; }
        // A fatal bridge error (no Node) wins over every transient state.
        if (state.fatalBridgeError && status !== 'linked') { status = 'bridgeerror'; }
        const changed = state.status !== status;
        state.status = status;
        if (typeof detail === 'string') { state.detail = detail; }
        render();
        if (changed) { log('status: ' + STATUS_VIEW[status].text + (state.detail ? ' (' + state.detail + ')' : '')); }
    }

    function render() {
        try {
            const view = STATUS_VIEW[state.status] || STATUS_VIEW.starting;
            if (ui.dot) { ui.dot.className = 'dot ' + view.dot; }
            let text = view.text;
            if (state.status === 'linked' && state.appVersion) { text = 'Linked to HDR Hint v' + state.appVersion; }
            if (ui.statusText) { ui.statusText.textContent = text; }
            if (ui.detailText) { ui.detailText.textContent = state.detail || ''; }
            if (ui.btnDock) {
                ui.btnDock.textContent = state.docked ? 'Undock' : 'Dock';
                ui.btnDock.disabled = state.status !== 'linked';
            }
            if (ui.btnShow) { ui.btnShow.disabled = false; }
            if (ui.btnLaunch) { ui.btnLaunch.disabled = state.status === 'linked'; }
            renderBridge();
        } catch (e) {
            log('render: ' + e);
        }
    }

    function renderBridge() {
        if (!ui.bridgeText) { return; }
        const b = state.bridge;
        let text = '';
        if (b.loadError) {
            text = 'Bridge: ' + b.loadError;
        } else if (b.seen) {
            text = 'Bridge: ' + b.registered.length + ' listener' + (b.registered.length === 1 ? '' : 's');
            if (b.errors.length) { text += ' \u00b7 ' + b.errors.length + ' error' + (b.errors.length === 1 ? '' : 's') + ' (see diagnostics)'; }
        }
        ui.bridgeText.textContent = text;
    }

    // ---- skin / theme ---------------------------------------------------------
    // AME hands us its panel background as 0..255 components. Everything else
    // (text, borders) is derived from whether that colour is dark or light.
    function readSkin() {
        const skin = { panelBg: '#232323', isDark: true, appName: '', appVersion: '' };
        try {
            const env = cs ? cs.getHostEnvironment() : null;
            if (env) {
                skin.appName = typeof env.appName === 'string' ? env.appName : '';
                skin.appVersion = typeof env.appVersion === 'string' ? env.appVersion : '';
                const info = env.appSkinInfo;
                const c = info && info.panelBackgroundColor && info.panelBackgroundColor.color;
                if (c && typeof c.red === 'number') {
                    const r = clampByte(c.red), g = clampByte(c.green), b = clampByte(c.blue);
                    skin.panelBg = '#' + hex2(r) + hex2(g) + hex2(b);
                    skin.isDark = (0.2126 * r + 0.7152 * g + 0.0722 * b) < 128;
                }
            }
        } catch (e) {
            log('skin: ' + e);
        }
        return skin;
    }

    function applySkin() {
        const skin = readSkin();
        state.skin = skin;
        try {
            document.documentElement.style.setProperty('--bg', skin.panelBg);
            document.body.classList.toggle('dark', skin.isDark);
            document.body.classList.toggle('light', !skin.isDark);
        } catch (e) {
            log('applySkin: ' + e);
        }
    }

    // ---- extension path + config ---------------------------------------------
    function resolveExtPath() {
        // Preferred: CEP tells us where the extension lives.
        try {
            if (cs && typeof SystemPath !== 'undefined' && SystemPath && SystemPath.EXTENSION) {
                const p = cs.getSystemPath(SystemPath.EXTENSION);
                if (p && typeof p === 'string') { return toBackslashes(p); }
            }
        } catch (e) {
            log('getSystemPath: ' + e);
        }
        // Fallback: derive it from our own file:// URL.
        try {
            let p = decodeURIComponent(window.location.pathname || '');
            p = p.replace(/^\/+([A-Za-z]:)/, '$1');
            p = toBackslashes(p);
            const cut = p.lastIndexOf('\\');
            if (cut > 0) { p = p.slice(0, cut); }
            return p;
        } catch (e) {
            log('location fallback: ' + e);
        }
        return '';
    }

    // Reads a UTF-8 text file through Node, or through cep.fs when Node is
    // unavailable. Returns null when neither can read it.
    function readTextFile(p) {
        if (!p) { return null; }
        try {
            if (fs) { return fs.readFileSync(p, 'utf8'); }
        } catch (_) { /* try cep.fs */ }
        try {
            const cepFs = window.cep && window.cep.fs;
            if (cepFs && typeof cepFs.readFile === 'function') {
                const r = cepFs.readFile(p, 'utf-8');
                if (r && r.err === 0 && typeof r.data === 'string') { return r.data; }
            }
        } catch (_) { /* fall through */ }
        return null;
    }

    function fileExists(p) {
        if (!p) { return false; }
        try {
            if (fs) { return fs.existsSync(p); }
        } catch (_) { /* try cep.fs */ }
        try {
            const cepFs = window.cep && window.cep.fs;
            if (cepFs && typeof cepFs.stat === 'function') {
                const r = cepFs.stat(p);
                return !!(r && r.err === 0);
            }
        } catch (_) { /* fall through */ }
        return false;
    }

    function readJsonFile(p) {
        const text = readTextFile(p);
        if (text === null || text === undefined) { return null; }
        try {
            const obj = JSON.parse(String(text).replace(/^\uFEFF/, ''));
            return (obj && typeof obj === 'object' && !Array.isArray(obj)) ? obj : null;
        } catch (e) {
            log('json ' + p + ': ' + e);
            return null;
        }
    }

    // config.json is written by the installer next to the panel. Only a few
    // keys matter here; the exe path is consumed by the launcher.
    function loadConfig() {
        if (!state.extPath) { return; }
        const cfg = readJsonFile(joinPath(state.extPath, 'config.json'));
        if (!cfg) { log('config.json: not found or invalid (using defaults)'); return; }
        if (typeof cfg.autoLaunch === 'boolean') { state.autoLaunch = cfg.autoLaunch; }
        if (typeof cfg.pipeName === 'string' && cfg.pipeName.trim()) {
            const name = cfg.pipeName.trim();
            state.pipeName = (name.indexOf('\\pipe\\') >= 0) ? name : PIPE_PREFIX + name;
        }
        log('config.json: exePath=' + (cfg.exePath || '(none)') + ' autoLaunch=' + state.autoLaunch + ' pipe=' + state.pipeName);
    }

    // ---- pipe client ----------------------------------------------------------
    const pipe = {
        sock: null,
        state: 'disconnected',    // disconnected | connecting | linked
        buf: '',
        ring: [],
        attempt: 0,
        reconnectTimer: null,
        pingTimer: null,
        everLinked: false,
        lastInboundAt: 0,
        lastErrorCode: ''
    };

    function send(msg) {
        if (!msg || typeof msg !== 'object') { return false; }
        let line;
        try { line = JSON.stringify(msg) + '\n'; } catch (e) { log('stringify: ' + e); return false; }
        if (pipe.state === 'linked' && pipe.sock) {
            try {
                pipe.sock.write(line);
                return true;
            } catch (e) {
                log('write: ' + e);
            }
        }
        // Not linked: keep the message for the next connection. Pings and
        // bounds are stale by then (fresh ones are sent on connect), so only
        // events and commands are worth buffering.
        if (msg.kind === 'ping' || msg.kind === 'panelBounds') { return false; }
        pipe.ring.push(line);
        while (pipe.ring.length > RING_MAX) { pipe.ring.shift(); }
        return false;
    }

    function helloMessage() {
        let pid = 0;
        try { pid = num(nodeProcess && nodeProcess.pid); } catch (_) { pid = 0; }
        return {
            kind: 'hello',
            pid: pid,
            extPath: state.extPath,
            version: PANEL_VERSION,
            host: { appName: state.skin.appName, appVersion: state.skin.appVersion },
            skin: { panelBg: state.skin.panelBg, isDark: state.skin.isDark }
        };
    }

    function connect() {
        if (!net) {
            state.fatalBridgeError = 'Node.js is not available in this panel (manifest needs --enable-nodejs)';
            setStatus('bridgeerror', state.fatalBridgeError);
            return;
        }
        if (pipe.sock) { return; }              // already connecting or linked
        pipe.state = 'connecting';
        pipe.lastErrorCode = '';
        let sock = null;
        try {
            sock = net.connect({ path: state.pipeName });
        } catch (e) {
            log('connect threw: ' + e);
            pipe.state = 'disconnected';
            scheduleReconnect();
            return;
        }
        if (!sock) { pipe.state = 'disconnected'; scheduleReconnect(); return; }
        pipe.sock = sock;
        try { sock.setEncoding('utf8'); } catch (_) { /* default is Buffer; onData handles both */ }

        // Every handler checks that this socket is still the current one so a
        // late event from a discarded socket cannot corrupt the state machine.
        sock.on('connect', () => { if (pipe.sock === sock) { onConnected(); } });
        sock.on('data', (chunk) => { if (pipe.sock === sock) { onData(chunk); } });
        sock.on('error', (err) => { if (pipe.sock === sock) { onSocketError(err); } });
        sock.on('close', () => { if (pipe.sock === sock) { onClosed(); } });
    }

    function onConnected() {
        pipe.state = 'linked';
        pipe.attempt = 0;
        pipe.everLinked = true;
        pipe.lastInboundAt = Date.now();
        log('pipe connected: ' + state.pipeName);
        send(helloMessage());
        // Flush whatever accumulated while we were down, oldest first.
        const pending = pipe.ring.splice(0, pipe.ring.length);
        for (const line of pending) {
            try { pipe.sock.write(line); } catch (e) { log('flush: ' + e); break; }
        }
        if (pending.length) { log('flushed ' + pending.length + ' buffered message(s)'); }
        reportBounds(true);
        startPing();
        setStatus('linked', 'waiting for welcome');
    }

    function onData(chunk) {
        pipe.lastInboundAt = Date.now();
        try {
            pipe.buf += (typeof chunk === 'string') ? chunk : String(chunk);
        } catch (e) {
            log('data: ' + e);
            return;
        }
        // Guard against a runaway line: drop the buffer rather than grow forever.
        if (pipe.buf.length > MAX_LINE_BYTES) {
            log('inbound line exceeded ' + MAX_LINE_BYTES + ' bytes; discarding');
            pipe.buf = '';
            return;
        }
        let nl = pipe.buf.indexOf('\n');
        while (nl >= 0) {
            const line = pipe.buf.slice(0, nl).replace(/\r$/, '');
            pipe.buf = pipe.buf.slice(nl + 1);
            if (line.trim()) { handleLine(line); }
            nl = pipe.buf.indexOf('\n');
        }
    }

    function handleLine(line) {
        let msg = null;
        try { msg = JSON.parse(line); } catch (e) { log('bad json from app: ' + line.slice(0, 120)); return; }
        if (!msg || typeof msg !== 'object' || typeof msg.kind !== 'string') { log('message without kind: ' + line.slice(0, 120)); return; }
        try { handleInbound(msg); } catch (e) { log('handle ' + msg.kind + ': ' + e); }
    }

    function handleInbound(msg) {
        switch (msg.kind) {
            case 'welcome':
                state.appVersion = typeof msg.appVersion === 'string' ? msg.appVersion : '';
                if (typeof msg.docked === 'boolean') { state.docked = msg.docked; }
                setStatus('linked', state.docked ? 'docked' : 'floating');
                log('welcome: app v' + state.appVersion + ' protocol ' + (msg.protocol !== undefined ? msg.protocol : '?'));
                break;
            case 'pong':
                break;
            case 'status':
                applyStatusMessage(msg);
                break;
            case 'requestBounds':
                reportBounds(true);
                break;
            case 'ack':
                if (msg.ok === false) { log('app rejected request: ' + (msg.error || 'unknown error')); }
                else { log('ack' + (msg.cmd ? ' ' + msg.cmd : '') + (msg.request ? ' ' + msg.request : '')); }
                break;
            case 'jobs':
                log('jobs update: ' + (Array.isArray(msg.jobs) ? msg.jobs.length : '?') + ' job(s)');
                break;
            default:
                log('unhandled message kind: ' + msg.kind);
        }
    }

    // Status lines from the app are free-form counters; render whatever is there.
    function applyStatusMessage(msg) {
        if (typeof msg.docked === 'boolean') { state.docked = msg.docked; }
        const parts = [];
        parts.push(state.docked ? 'docked' : 'floating');
        const counters = [['encoding', 'encoding'], ['ready', 'ready'], ['muxing', 'muxing'], ['done', 'done'], ['failed', 'failed']];
        for (const [key, label] of counters) {
            if (typeof msg[key] === 'number' && msg[key] > 0) { parts.push(msg[key] + ' ' + label); }
        }
        const lastJob = msg.lastJob && typeof msg.lastJob === 'object' ? msg.lastJob : null;
        const lastName = lastJob ? lastJob.name : msg.lastJobName;
        const lastState = lastJob ? lastJob.state : msg.lastJobState;
        if (typeof lastName === 'string' && lastName) { parts.push('last: ' + lastName + (typeof lastState === 'string' && lastState ? ' (' + lastState + ')' : '')); }
        if (msg.mkvmergeOk === false) { parts.push('mkvmerge not found'); }
        state.detail = parts.join(' \u00b7 ');
        if (state.status === 'linked') { render(); }
    }

    function onSocketError(err) {
        pipe.lastErrorCode = (err && typeof err.code === 'string') ? err.code : '';
        const text = (err && err.message) ? err.message : String(err);
        // ENOENT (pipe absent) is the normal "app not running" case; log it
        // once per state change rather than every retry.
        if (pipe.lastErrorCode !== 'ENOENT' || pipe.attempt === 0) { log('pipe error: ' + text); }
    }

    function onClosed() {
        const wasLinked = pipe.state === 'linked';
        const sock = pipe.sock;
        pipe.sock = null;
        pipe.state = 'disconnected';
        pipe.buf = '';
        stopPing();
        try { if (sock) { sock.removeAllListeners(); sock.destroy(); } } catch (_) { /* already gone */ }
        if (wasLinked) { log('pipe closed'); }

        if (pipe.lastErrorCode === 'ENOENT') {
            if (pipe.everLinked) { setStatus('reconnecting', 'HDR Hint is not running'); }
            else if (state.status !== 'launching' && state.status !== 'notinstalled') { setStatus('connecting', 'HDR Hint is not running'); }
            maybeAutoLaunch();
        } else {
            setStatus(pipe.everLinked ? 'reconnecting' : 'connecting', pipe.lastErrorCode || '');
        }
        scheduleReconnect();
    }

    function scheduleReconnect() {
        if (pipe.reconnectTimer) { return; }
        const delay = BACKOFF_MS[Math.min(pipe.attempt, BACKOFF_MS.length - 1)];
        pipe.attempt += 1;
        pipe.reconnectTimer = setTimeout(() => {
            pipe.reconnectTimer = null;
            connect();
        }, delay);
    }

    function startPing() {
        stopPing();
        pipe.pingTimer = setInterval(() => {
            if (pipe.state !== 'linked' || !pipe.sock) { return; }
            // Half-open detection: the app answers every ping, so silence
            // beyond STALE_LINK_MS means the pipe is dead on the other side.
            if (Date.now() - pipe.lastInboundAt > STALE_LINK_MS) {
                log('no data from app for ' + STALE_LINK_MS + ' ms; reconnecting');
                try { pipe.sock.destroy(); } catch (_) { onClosed(); }
                return;
            }
            send({ kind: 'ping' });
        }, PING_MS);
    }

    function stopPing() {
        if (pipe.pingTimer) { clearInterval(pipe.pingTimer); pipe.pingTimer = null; }
    }

    // ---- launcher -------------------------------------------------------------
    const launcher = {
        lastAttemptAt: 0,
        notInstalledUntil: 0,
        inFlight: false
    };

    // reg.exe is the one process we run besides the app itself; it is a console
    // tool so hiding its window is correct here (unlike the exe spawn below).
    function queryRegistryExePath() {
        return new Promise((resolve) => {
            if (!cp) { resolve(''); return; }
            try {
                cp.execFile('reg', ['query', 'HKCU\\Software\\HdrHint', '/v', 'ExePath'],
                    { windowsHide: true, timeout: 4000 }, (err, stdout) => {
                        if (err || !stdout) { resolve(''); return; }
                        const lines = String(stdout).split(/\r?\n/);
                        for (const l of lines) {
                            const m = /ExePath\s+REG_(?:EXPAND_)?SZ\s+(.+)$/i.exec(l);
                            if (m && m[1] && m[1].trim()) { resolve(m[1].trim()); return; }
                        }
                        resolve('');
                    });
            } catch (e) {
                log('reg query: ' + e);
                resolve('');
            }
        });
    }

    // Candidate order is fixed: the installer's config.json, the app's own
    // launcher.json, the registry, then the default per-user install folder.
    // A path chosen with Locate... (this session) is tried before all of them.
    async function resolveExePath() {
        const candidates = [];
        if (state.exeOverride) { candidates.push({ exe: state.exeOverride, source: 'Locate' }); }

        const cfg = state.extPath ? readJsonFile(joinPath(state.extPath, 'config.json')) : null;
        if (cfg && typeof cfg.exePath === 'string' && cfg.exePath.trim()) { candidates.push({ exe: toBackslashes(cfg.exePath.trim()), source: 'config.json' }); }

        const appData = envVar('APPDATA');
        if (appData) {
            const lj = readJsonFile(joinPath(joinPath(appData, 'HdrHint'), 'launcher.json'));
            if (lj && typeof lj.exePath === 'string' && lj.exePath.trim()) { candidates.push({ exe: toBackslashes(lj.exePath.trim()), source: 'launcher.json' }); }
        }

        const reg = await queryRegistryExePath();
        if (reg) { candidates.push({ exe: toBackslashes(reg), source: 'registry' }); }

        const localAppData = envVar('LOCALAPPDATA');
        if (localAppData) { candidates.push({ exe: joinPath(joinPath(joinPath(localAppData, 'Programs'), 'HdrHint'), 'HdrHint.exe'), source: 'LOCALAPPDATA' }); }

        for (const c of candidates) {
            if (fileExists(c.exe)) { return c; }
            log('launch: ' + c.source + ' -> ' + c.exe + ' (missing)');
        }
        return null;
    }

    async function launchExe(reason) {
        if (launcher.inFlight) { log('launch: already in progress'); return; }
        launcher.inFlight = true;
        try {
            const found = await resolveExePath();
            if (!found) {
                launcher.notInstalledUntil = Date.now() + NOT_INSTALLED_RETRY_MS;
                setStatus('notinstalled', 'HdrHint.exe not found; use Locate\u2026');
                log('launch (' + reason + '): no HdrHint.exe found');
                return;
            }
            launcher.lastAttemptAt = Date.now();
            setStatus('launching', found.source + ': ' + found.exe);
            log('launch (' + reason + '): ' + found.exe + ' via ' + found.source);
            if (!spawnDetached(found.exe)) { launchViaJsx(found.exe); }
        } catch (e) {
            log('launch: ' + e);
        } finally {
            launcher.inFlight = false;
        }
    }

    // Detached Node spawn. Deliberately no windowsHide: libuv would pass
    // STARTF_USESHOWWINDOW/SW_HIDE and the exe's first ShowWindow would be
    // ignored, leaving the app invisible.
    function spawnDetached(exe) {
        if (!cp) { log('spawn: child_process unavailable'); return false; }
        const args = ['--from-panel', '--ext-path', state.extPath];
        let child = null;
        try {
            child = cp.spawn(exe, args, { detached: true, stdio: 'ignore' });
        } catch (e) {
            log('spawn threw: ' + e);
            return false;
        }
        if (!child) { return false; }
        const startedAt = Date.now();
        let settled = false;
        child.on('error', (err) => {
            if (settled) { return; }
            settled = true;
            log('spawn error: ' + ((err && err.message) ? err.message : String(err)));
            launchViaJsx(exe);
        });
        // A child that dies within the grace period was most likely killed by
        // CEF's job object; retry through AME's own process via ExtendScript.
        child.on('exit', (code, signal) => {
            if (settled) { return; }
            settled = true;
            const ms = Date.now() - startedAt;
            if (ms < CHILD_GRACE_MS) {
                if (code === 0 && pipe.state === 'linked') {
                    log('child exited cleanly after ' + ms + ' ms (second instance forwarded)');
                    return;
                }
                log('child exited after ' + ms + ' ms (code ' + code + (signal ? ', signal ' + signal : '') + '); using ExtendScript launcher');
                launchViaJsx(exe);
            } else {
                log('child exited after ' + ms + ' ms (code ' + code + ')');
            }
        });
        try { child.unref(); } catch (_) { /* older Node */ }
        log('spawned pid ' + (child.pid || '?') + ': ' + exe);
        return true;
    }

    // ExtendScript fallback: File.execute() is a ShellExecute from AME.exe,
    // outside CEF's process tree. Loads host.jsx first if the bridge is missing.
    function launchViaJsx(exe) {
        if (!cs) { log('jsx launch: CSInterface unavailable'); return; }
        const exeLit = toJsxLiteral(exe);
        const extLit = toJsxLiteral(state.extPathFwd);
        const script =
            '(function(){try{' +
            'if(!$.global.__hdrHintBridge){$.evalFile("' + extLit + '/jsx/host.jsx");}' +
            'var b=$.global.__hdrHintBridge;' +
            'if(!b||typeof b.launch!=="function"){return "no-bridge";}' +
            'return String(b.launch("' + exeLit + '"));' +
            '}catch(e){return "error: "+e;}})()';
        try {
            cs.evalScript(script, (r) => { log('jsx launch: ' + r); });
        } catch (e) {
            log('jsx launch threw: ' + e);
        }
    }

    function maybeAutoLaunch() {
        if (!state.autoLaunch) { return; }
        const now = Date.now();
        if (now < launcher.notInstalledUntil) { return; }
        if (now - launcher.lastAttemptAt < LAUNCH_MIN_INTERVAL_MS) { return; }
        launcher.lastAttemptAt = now;             // gate before the async resolve
        launchExe('pipe absent');
    }

    // Locate...: native file picker through cep.fs; the choice is persisted to
    // launcher.json so the next AME session finds it without asking again.
    function locateExe() {
        try {
            const cepFs = window.cep && window.cep.fs;
            if (!cepFs || typeof cepFs.showOpenDialog !== 'function') { log('Locate: cep.fs.showOpenDialog unavailable'); return; }
            const localAppData = envVar('LOCALAPPDATA');
            const initial = localAppData ? joinPath(joinPath(localAppData, 'Programs'), 'HdrHint') : '';
            const r = cepFs.showOpenDialog(false, false, 'Locate HdrHint.exe', initial, ['exe']);
            const chosen = (r && r.err === 0 && Array.isArray(r.data) && r.data.length) ? r.data[0] : '';
            if (!chosen) { log('Locate: cancelled'); return; }
            const p = toBackslashes(chosen);
            if (!fileExists(p)) { log('Locate: file does not exist: ' + p); return; }
            state.exeOverride = p;
            launcher.notInstalledUntil = 0;
            persistLauncherJson(p);
            launchExe('located');
        } catch (e) {
            log('Locate: ' + e);
        }
    }

    function persistLauncherJson(exe) {
        if (!fs) { return; }
        const appData = envVar('APPDATA');
        if (!appData) { return; }
        try {
            const dir = joinPath(appData, 'HdrHint');
            if (!fs.existsSync(dir)) { fs.mkdirSync(dir, { recursive: true }); }
            const file = joinPath(dir, 'launcher.json');
            const obj = readJsonFile(file) || {};
            obj.exePath = exe;
            obj.updatedBy = 'panel';
            obj.updatedAt = new Date().toISOString();
            fs.writeFileSync(file, JSON.stringify(obj, null, 2), 'utf8');
            log('launcher.json updated: ' + file);
        } catch (e) {
            log('launcher.json: ' + e);
        }
    }

    // ---- ExtendScript bridge --------------------------------------------------
    function loadJsx() {
        if (!cs) { state.bridge.loadError = 'CSInterface unavailable'; renderBridge(); return; }
        if (!state.extPathFwd) { state.bridge.loadError = 'extension path unknown'; renderBridge(); return; }
        const extLit = toJsxLiteral(state.extPathFwd);
        const script = '$.evalFile("' + extLit + '/jsx/json2.js"); $.evalFile("' + extLit + '/jsx/host.jsx"); "ok"';
        try {
            cs.evalScript(script, (r) => {
                log('jsx load: ' + r);
                if (typeof r === 'string' && r.indexOf('EvalScript error') >= 0) {
                    state.bridge.loadError = 'host.jsx failed to load (see diagnostics)';
                    renderBridge();
                }
                queryBridge();
            });
        } catch (e) {
            state.bridge.loadError = String(e);
            renderBridge();
        }
    }

    // Pull the bridge's registered/errors lists directly, in case the
    // bridgeReady CSXS event was dispatched before our listener was attached.
    function queryBridge() {
        if (!cs) { return; }
        const script =
            '(function(){try{var b=$.global.__hdrHintBridge;if(!b){return "none";}' +
            'return b.registered.join(",")+"|"+b.errors.join(";");}catch(e){return "error: "+e;}})()';
        try {
            cs.evalScript(script, (r) => {
                if (typeof r !== 'string' || r === 'none' || r.indexOf('error') === 0 || r.indexOf('EvalScript error') >= 0) {
                    log('bridge query: ' + r);
                    if (!state.bridge.seen) { state.bridge.loadError = 'bridge not installed (' + r + ')'; renderBridge(); }
                    return;
                }
                const bar = r.indexOf('|');
                const regs = bar >= 0 ? r.slice(0, bar) : r;
                const errs = bar >= 0 ? r.slice(bar + 1) : '';
                onBridgeReady({
                    registered: regs ? regs.split(',') : [],
                    errors: errs ? errs.split(';') : []
                });
            });
        } catch (e) {
            log('bridge query threw: ' + e);
        }
    }

    function onBridgeReady(d) {
        const regs = Array.isArray(d.registered) ? d.registered.map(String) : [];
        const errs = Array.isArray(d.errors) ? d.errors.map(String) : [];
        state.bridge.registered = regs;
        state.bridge.errors = errs;
        state.bridge.seen = true;
        state.bridge.loadError = '';
        log('bridge ready: ' + regs.length + ' listener(s)' + (regs.length ? ' [' + regs.join(', ') + ']' : '') + (d.ameVersion ? ' AME ' + d.ameVersion : ''));
        for (const e of errs) { log('bridge error: ' + e); }
        renderBridge();
    }

    // ---- CSXS events ----------------------------------------------------------
    function subscribeCsxs() {
        if (!cs) { return; }
        const sub = (type, fn) => {
            try {
                cs.addEventListener(type, (ev) => {
                    try { fn(ev); } catch (e) { log('handler ' + type + ': ' + e); }
                });
            } catch (e) {
                log('addEventListener ' + type + ': ' + e);
            }
        };

        // host.jsx payloads. CEP parses valid JSON natively, so data is usually
        // an object already; a string means parsing was skipped or failed.
        sub(EVENT_TYPE, (ev) => {
            let d = ev ? ev.data : null;
            if (typeof d === 'string') {
                try { d = JSON.parse(d); } catch (_) { d = { type: 'raw', raw: ev.data }; }
            }
            if (!d || typeof d !== 'object') { d = { type: 'raw', raw: String(d) }; }
            if (typeof d.type !== 'string' || !d.type) { d.type = 'raw'; }
            if (d.type === 'bridgeReady') { onBridgeReady(d); }
            else if (d.type !== 'progress') { log('ame ' + d.type + (d.outputFilePath ? ': ' + d.outputFilePath : '')); }
            send(Object.assign({}, d, { kind: 'ame' }));
        });

        // Lifecycle. The app un-owns its window on these before AME tears the
        // frame down, so send them straight away (they buffer if not linked).
        sub(CSXS_EVENTS.beforeQuit, () => {
            log('AME is quitting');
            send({ kind: 'ame', type: 'appBeforeQuit' });
        });
        sub(CSXS_EVENTS.extensionUnloaded, (ev) => {
            // CEP fires this for every extension; ignore other panels when the
            // payload names one.
            const data = ev && typeof ev.data === 'string' ? ev.data : '';
            if (data && state.extensionId && data.indexOf(state.extensionId) < 0 && data.indexOf('com.everett.hdrhint') < 0) { return; }
            log('extension unloaded');
            send({ kind: 'ame', type: 'extensionUnloaded' });
        });
        sub(CSXS_EVENTS.workspaceChanged, () => {
            log('workspace changed');
            send({ kind: 'ame', type: 'workspaceChanged' });
            startBoundsPoll();
        });
        sub(CSXS_EVENTS.visibilityChanged, () => {
            const b = measure();
            log('visibility changed: ' + (b.visible ? 'visible' : 'hidden'));
            send({ kind: 'ame', type: 'visibility', visible: b.visible });
            reportBounds(true);
        });
        sub(CSInterface.THEME_COLOR_CHANGED_EVENT, () => {
            applySkin();
            log('theme changed: ' + state.skin.panelBg);
            send({ kind: 'ame', type: 'themeChanged', panelBg: state.skin.panelBg, isDark: state.skin.isDark });
        });
    }

    // ---- bounds reporter ------------------------------------------------------
    const bounds = { last: null, seq: 0, pollTimer: null, stableTicks: 0 };

    function measure() {
        let visible;
        try {
            const cepApi = window.__adobe_cep__;
            visible = (cepApi && typeof cepApi.isWindowVisible === 'function') ? !!cepApi.isWindowVisible() : !document.hidden;
        } catch (_) {
            visible = !document.hidden;
        }
        return {
            x: num(window.screenX),
            y: num(window.screenY),
            w: num(window.innerWidth),
            h: num(window.innerHeight),
            dpr: num(window.devicePixelRatio) || 1,
            visible: visible,
            screenW: num(screen && screen.width),
            screenH: num(screen && screen.height)
        };
    }

    function reportBounds(force) {
        let b;
        try { b = measure(); } catch (e) { log('measure: ' + e); return false; }
        const keys = ['x', 'y', 'w', 'h', 'dpr', 'visible'];
        const changed = !bounds.last || keys.some((k) => b[k] !== bounds.last[k]);
        if (changed || force) {
            bounds.last = b;
            send(Object.assign({ kind: 'panelBounds', seq: ++bounds.seq }, b));
        }
        return changed;
    }

    // 100 ms polling while the rectangle keeps moving (drag/resize), stopping
    // after one second of stable values.
    function startBoundsPoll() {
        if (bounds.pollTimer) { bounds.stableTicks = 0; return; }
        bounds.stableTicks = 0;
        bounds.pollTimer = setInterval(() => {
            if (reportBounds(false)) {
                bounds.stableTicks = 0;
            } else if (++bounds.stableTicks >= BOUNDS_STABLE_TICKS) {
                clearInterval(bounds.pollTimer);
                bounds.pollTimer = null;
            }
        }, BOUNDS_POLL_MS);
    }

    function wireBounds() {
        try { window.addEventListener('resize', () => startBoundsPoll()); } catch (e) { log('resize: ' + e); }
        try { window.addEventListener('focus', () => startBoundsPoll()); } catch (e) { log('focus: ' + e); }
        try { document.addEventListener('visibilitychange', () => reportBounds(true)); } catch (e) { log('visibilitychange: ' + e); }
        // Slow heartbeat: a drag of AME's main window fires no DOM event here.
        setInterval(() => reportBounds(false), BOUNDS_HEARTBEAT_MS);
    }

    // ---- buttons --------------------------------------------------------------
    function onClick(el, fn) {
        if (!el) { return; }
        try {
            el.addEventListener('click', () => {
                try { fn(); } catch (e) { log('click: ' + e); }
            });
        } catch (e) {
            log('button wiring: ' + e);
        }
    }

    function copyLog() {
        const text = logLines.join('\n');
        try {
            if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
                navigator.clipboard.writeText(text).then(() => log('log copied'), (e) => { log('clipboard: ' + e); copyLogLegacy(text); });
                return;
            }
        } catch (_) { /* fall back */ }
        copyLogLegacy(text);
    }

    function copyLogLegacy(text) {
        try {
            const ta = document.createElement('textarea');
            ta.value = text;
            ta.style.position = 'fixed';
            ta.style.opacity = '0';
            document.body.appendChild(ta);
            ta.select();
            const ok = document.execCommand('copy');
            document.body.removeChild(ta);
            log(ok ? 'log copied' : 'copy failed');
        } catch (e) {
            log('copy: ' + e);
        }
    }

    function wireButtons() {
        onClick(ui.btnLaunch, () => launchExe('button'));
        onClick(ui.btnShow, () => {
            if (pipe.state === 'linked') { send({ kind: 'command', cmd: 'show' }); }
            else { launchExe('show'); }
        });
        onClick(ui.btnDock, () => {
            if (pipe.state !== 'linked') { return; }
            const cmd = state.docked ? 'undock' : 'dock';
            send({ kind: 'command', cmd: cmd });
            // Optimistic flip; the next status message corrects it if needed.
            state.docked = !state.docked;
            render();
        });
        onClick(ui.btnLocate, locateExe);
        onClick(ui.btnCopyLog, copyLog);
        onClick(ui.btnClearLog, () => { logLines.length = 0; renderLog(); });
    }

    // ---- lifecycle ------------------------------------------------------------
    function wireUnload() {
        try {
            window.addEventListener('beforeunload', () => {
                // Best effort: the pipe closing is the reliable signal anyway.
                try { send({ kind: 'ame', type: 'extensionUnloaded' }); } catch (_) { /* ignore */ }
                try { if (pipe.sock) { pipe.sock.end(); } } catch (_) { /* ignore */ }
            });
        } catch (e) {
            log('beforeunload: ' + e);
        }
        try {
            window.addEventListener('error', (ev) => {
                log('uncaught: ' + (ev && ev.message ? ev.message : String(ev)));
            });
        } catch (_) { /* ignore */ }
    }

    // ---- startup --------------------------------------------------------------
    function start() {
        log('HDR Hint panel v' + PANEL_VERSION + ' starting');
        applySkin();
        render();

        state.extPath = resolveExtPath();
        state.extPathFwd = state.extPath.replace(/\\/g, '/');
        try { state.extensionId = cs ? String(cs.getExtensionID() || '') : ''; } catch (_) { state.extensionId = ''; }
        log('extension: ' + (state.extensionId || '?') + ' at ' + (state.extPath || '(unknown path)'));
        log('host: ' + (state.skin.appName || '?') + ' ' + (state.skin.appVersion || '') + ', node: ' + (nodeRequire ? 'yes' : 'no') + ', skin ' + state.skin.panelBg + (state.skin.isDark ? ' (dark)' : ' (light)'));

        loadConfig();
        subscribeCsxs();
        loadJsx();
        wireButtons();
        wireBounds();
        wireUnload();

        if (!net) {
            state.fatalBridgeError = 'Node.js is not available in this panel (manifest needs --enable-nodejs)';
            log(state.fatalBridgeError);
            setStatus('bridgeerror', state.fatalBridgeError);
            return;
        }
        setStatus('connecting', '');
        connect();
    }

    try {
        start();
    } catch (e) {
        log('startup failed: ' + e);
        setStatus('bridgeerror', String(e));
    }
})();
