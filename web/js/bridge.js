// Bridge between the NaCl module (src/nacl/app.cpp) and the UI (app.js) for Tizen 3.0.
//
// C++ posts PP_VarDictionary messages ({event: type, data: json}) via pp::Instance::PostMessage;
// JS sends commands ({cmd: ..., ...}) via naclModule.postMessage(...) and exposes the same
// window.tsl interface expected by app.js.
(function () {
    'use strict';

    // Polyfill Element.prototype.closest for older WebKit/Chromium builds if needed.
    if (typeof Element !== 'undefined' && !Element.prototype.closest) {
        Element.prototype.closest = function (s) {
            var el = this;
            do {
                if (el.matches && el.matches(s)) return el;
                if (el.webkitMatchesSelector && el.webkitMatchesSelector(s)) return el;
                el = el.parentElement || el.parentNode;
            } while (el !== null && el.nodeType === 1);
            return null;
        };
    }

    var listeners = {};
    var resolveReady;
    var readyPromise = new Promise(function (resolve) {
        resolveReady = resolve;
    });

    function on(type, fn) {
        (listeners[type] = listeners[type] || []).push(fn);
    }

    function emit(type, data) {
        var list = listeners[type];
        if (!list) return;
        for (var i = 0; i < list.length; i++) {
            try {
                list[i](data);
            } catch (e) {
                console.error('tsl listener error on ' + type, e);
            }
        }
    }

    window.tslEvent = function (type, json) {
        var data = {};
        try {
            data = json ? JSON.parse(json) : {};
        } catch (e) {
            console.error('bad event JSON from module', type, json);
        }
        emit(type, data);
    };

    var cachedStats = JSON.stringify({
        connected: false,
        videoFrames: 0, keyFrames: 0, videoBytes: 0, audioPackets: 0,
        width: 0, height: 0, videoCodec: 0, colorspace: 0,
        targetBitrate: 0, framerateNum: 0, framerateDen: 0,
        videoAppended: 0, videoFailed: 0, audioAppended: 0, audioFailed: 0,
        silenceFilled: 0, audioDropped: 0, audioCallbackFrames: 0, ppbGamepads: 0,
        videoPackets: 0, videoPacketsLost: 0, lostReports: 0, roundTripMs: 0,
        fec: false, fecRecovered: 0, fecUnrecoverable: 0,
        videoSessionChanges: 0, videoTrackCloses: 0, lastCloseReason: -1,
        lastAppendError: 0, pipelineErrors: 0, lastPipelineError: -1,
        audioConcealed: 0, picturesDecoded: 0, picturesPainted: 0, picturesDropped: 0,
        audioGaps: 0, audioLeadMs: 0, maxFrameGapMs: 0
    });

    var playerSetUp = false;
    var streamingActive = false;

    function getNaclModule() {
        return document.getElementById('nacl_module');
    }

    function post(msg) {
        var el = getNaclModule();
        if (el && typeof el.postMessage === 'function') {
            el.postMessage(msg);
            return true;
        }
        return false;
    }

    var naclMod = {
        createIdentity: function (randomHex, name) {
            if (typeof randomHex !== 'string' || randomHex.length < 80) return '';
            var hex = randomHex.slice(0, 80).toLowerCase();
            var label = name || 'Samsung TV';
            return 'tsl1:' + hex.slice(0, 16) + ':' + hex.slice(16, 80) + ':' + label;
        },
        init: function (identity) {
            if (typeof identity !== 'string' || identity.indexOf('tsl1:') !== 0) return false;
            return post({cmd: 'init', identity: identity});
        },
        legacyPlayer: function () {
            return false;
        },
        keepsPlayer: function () {
            return true;
        },
        playerSetUp: function () {
            return playerSetUp;
        },
        latencyNeedsReload: function () {
            return false;
        },
        startDiscovery: function () {
            return post({cmd: 'startDiscovery'});
        },
        stopDiscovery: function () {
            post({cmd: 'stopDiscovery'});
        },
        probe: function (ip) {
            if (!ip) return false;
            return post({cmd: 'probe', ip: String(ip)});
        },
        wake: function (mac) {
            if (!mac) return false;
            return post({cmd: 'wake', mac: String(mac)});
        },
        pair: function (ip, pin) {
            if (!ip || !pin) return false;
            return post({cmd: 'pair', ip: String(ip), pin: String(pin)});
        },
        cancelPair: function () {
            post({cmd: 'cancelPair'});
        },
        startStream: function (ip, steamId, hostKeyHex, width, height, hevc, audio, bigPicture,
                               quality, bitrateKbps, framerate, hdr, keepPlaying) {
            if (!ip || typeof hostKeyHex !== 'string' || hostKeyHex.length !== 64) return false;
            playerSetUp = true;
            resetPadSnapshot();
            streamingActive = true;
            return post({
                cmd: 'startStream',
                ip: String(ip),
                steamId: String(steamId || ''),
                hostKeyHex: String(hostKeyHex),
                width: width | 0,
                height: height | 0,
                hevc: !!hevc,
                audio: !!audio,
                bigPicture: !!bigPicture,
                quality: quality | 0,
                bitrateKbps: bitrateKbps | 0,
                framerate: framerate | 0,
                hdr: !!hdr,
                keepPlaying: !!keepPlaying
            });
        },
        stopStream: function () {
            streamingActive = false;
            post({cmd: 'stopStream'});
        },
        key: function (code, down) {
            post({cmd: 'key', code: String(code), down: !!down});
        },
        sendText: function (text) {
            post({cmd: 'sendText', text: String(text)});
        },
        setPadInput: function (enabled) {
            post({cmd: 'setPadInput', enabled: !!enabled});
        },
        mouseMove: function (dx, dy) {
            post({cmd: 'mouseMove', dx: dx | 0, dy: dy | 0});
        },
        mouseButton: function (jsButton, down) {
            post({cmd: 'mouseButton', jsButton: jsButton | 0, down: !!down});
        },
        mouseWheel: function (deltaY) {
            post({cmd: 'mouseWheel', deltaY: deltaY | 0});
        },
        stats: function () {
            post({cmd: 'stats'});
            return cachedStats;
        },
        setLogLevel: function (level) {
            post({cmd: 'setLogLevel', level: level | 0});
        }
    };

    window.Module = window.Module || naclMod;

    var readyResolved = false;
    window.tslReady = function () {
        if (readyResolved) return;
        readyResolved = true;
        resolveReady(window.Module);
    };

    window.tsl = {
        on: on,
        emit: emit,
        ready: readyPromise
    };

    // Forward browser-side navigator.getGamepads() during streaming only for slots not already
    // handled natively by Pepper's PPB_Gamepad (250 Hz), avoiding duplicate 8 ms polling work.
    var ppbPadMask = 0;
    var prevPadConnect = [false, false, false, false];
    var prevPadSig = ['', '', '', ''];
    function resetPadSnapshot() {
        ppbPadMask = 0;
        prevPadConnect = [false, false, false, false];
        prevPadSig = ['', '', '', ''];
    }
    function pollBrowserGamepadsForStream() {
        var anyJsConnected = false;
        if (streamingActive && navigator.getGamepads) {
            var pads = navigator.getGamepads();
            for (var i = 0; i < 4; i++) {
                if (ppbPadMask & (1 << i)) {
                    prevPadConnect[i] = false;
                    prevPadSig[i] = '';
                    continue;
                }
                var p = pads && pads[i];
                var conn = !!(p && p.connected);
                if (conn) anyJsConnected = true;
                if (!conn && !prevPadConnect[i]) continue;
                var axes = [];
                var buttons = [];
                var sig = '';
                if (conn) {
                    var alen = Math.min(p.axes ? p.axes.length : 0, 4);
                    for (var a = 0; a < alen; a++) axes.push(Number(p.axes[a]) || 0);
                    var blen = Math.min(p.buttons ? p.buttons.length : 0, 17);
                    for (var b = 0; b < blen; b++) {
                        var btn = p.buttons[b];
                        buttons.push(btn ? (typeof btn.value === 'number' ? btn.value : (btn.pressed ? 1 : 0)) : 0);
                    }
                    sig = '1:' + axes.join(',') + '|' + buttons.join(',');
                }
                if (conn === prevPadConnect[i] && sig === prevPadSig[i]) continue;
                prevPadConnect[i] = conn;
                prevPadSig[i] = sig;
                post({
                    cmd: 'gamepadSample',
                    slot: i,
                    connected: conn,
                    id: conn ? String(p.id || 'Gamepad') : '',
                    axes: axes,
                    buttons: buttons
                });
            }
        }
        setTimeout(pollBrowserGamepadsForStream, anyJsConnected ? 8 : 250);
    }

    function attachNaclListeners() {
        var listener = document.getElementById('nacl-listener');
        if (!listener) return;

        listener.addEventListener('load', function () {
            window.tslReady();
        }, true);

        listener.addEventListener('message', function (e) {
            var msg = e.data;
            if (!msg || typeof msg !== 'object') return;
            if (msg.event === 'ready') {
                window.tslReady();
            } else if (msg.event === 'stats') {
                if (typeof msg.data === 'string') cachedStats = msg.data;
            } else if (msg.event === 'ppbPads') {
                try {
                    var pd = msg.data ? JSON.parse(msg.data) : {};
                    ppbPadMask = pd.mask | 0;
                } catch (err) { /* ignore malformed pad mask */ }
            } else if (msg.event) {
                if (msg.event === 'streamEnded' || msg.event === 'streamFailed') {
                    streamingActive = false;
                    resetPadSnapshot();
                }
                window.tslEvent(msg.event, msg.data);
            }
        }, true);

        listener.addEventListener('error', function () {
            var el = getNaclModule();
            var err = (el && el.lastError) || 'failed to load NaCl module';
            emit('fatal', {message: 'NaCl error: ' + err, crashed: true});
        }, true);

        listener.addEventListener('crash', function () {
            var el = getNaclModule();
            var status = el ? el.exitStatus : -1;
            var lastErr = (el && el.lastError) ? (' (' + el.lastError + ')') : '';
            emit('fatal', {message: 'The NaCl streaming module stopped (exit ' + status + ')' + lastErr + '.', crashed: true});
        }, true);

        if (!getNaclModule()) {
            var embed = document.createElement('embed');
            embed.setAttribute('name', 'nacl_module');
            embed.setAttribute('id', 'nacl_module');
            embed.setAttribute('width', '1920');
            embed.setAttribute('height', '1080');
            embed.setAttribute('path', '.');
            embed.setAttribute('src', 'tsl.nmf');
            embed.setAttribute('type', 'application/x-nacl');
            listener.appendChild(embed);
        }

        pollBrowserGamepadsForStream();
    }

    // ?mock=1 loads dev/mock.js instead of NaCl, for working on the UI in a browser.
    var mock = /[?&]mock=1\b/.test(location.search);
    if (mock) {
        var script = document.createElement('script');
        script.src = 'dev/mock.js';
        document.head.appendChild(script);
    } else if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', attachNaclListeners);
    } else {
        attachNaclListeners();
    }
})();
