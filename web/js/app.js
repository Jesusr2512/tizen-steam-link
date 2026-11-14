// TV UI: computer list, pairing, settings and the stream view. Talks to the WASM module
// through window.tsl (bridge.js). Written for the Tizen 5.5 browser (Chromium 69): no
// optional chaining, no nullish coalescing, no class fields.
(function () {
    'use strict';

    var KEY = {
        LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACKSPACE: 8, ESCAPE: 27,
        BACK: 10009, RED: 403, GREEN: 404, YELLOW: 405, BLUE: 406, KEY_L: 76,
    };

    // Remote keys arrive without KeyboardEvent.code; map them for the host.
    var REMOTE_CODES = {37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown', 13: 'Enter'};

    var RESOLUTIONS = [
        {label: '720p', w: 1280, h: 720},
        {label: '1080p', w: 1920, h: 1080},
    ];

    var QUALITIES = {'-1': 'Automática', 1: 'Rápida', 2: 'Equilibrada', 3: 'Alta calidad'};

    function bandwidthText(kbps) {
        if (kbps === -2) return 'Sin límite (máx. red 100M)';
        if (kbps < 0) return 'Automático (Steam decide)';
        return (kbps / 1000) + ' Mbit/s';
    }

    var SETTINGS = [
        {key: 'bigPicture', label: 'Iniciar en', values: [false, true],
            text: function (v) { return v ? 'Big Picture' : 'Escritorio de Windows'; }},
        {key: 'quality', label: 'Calidad de transmisión', values: [1, 2, 3, -1],
            text: function (v) {
                if (v === 1) return 'Rápida (Mínima latencia)';
                if (v === 2) return 'Equilibrada';
                if (v === 3) return 'Alta calidad';
                return 'Automática';
            }},
        {key: 'resolution', label: 'Resolución máxima', values: [1, 0],
            text: function (v) { return v === 0 ? '720p (Más rápido)' : '1080p'; }},
        {key: 'framerate', label: 'Fotogramas por segundo', values: [60, 30],
            text: function (v) { return v + ' fps'; }},
        {key: 'bandwidth', label: 'Límite de ancho de banda',
            values: [-2, -1, 75000, 50000, 30000, 20000, 10000, 5000],
            text: function (v) { return bandwidthText(v); }},
        {key: 'audio', label: 'Audio', values: [true, false], kind: 'switch',
            text: function (v) { return v ? 'Activado' : 'Desactivado'; }},
        {key: 'stats', label: 'Estadísticas en pantalla', values: [false, true], kind: 'switch',
            text: function (v) { return v ? 'Visibles' : 'Ocultas'; }},
    ];

    // Valores por defecto optimizados para baja latencia en Tizen 3.0 NaCl
    var DEFAULT_SETTINGS = {resolution: 1, framerate: 60, hevc: false, latency: 1, quality: 1, bandwidth: -1,
        audio: true, bigPicture: false, stats: false, keepPlaying: false};
    var ONLINE_MS = 9000;
    var PAIR_TIMEOUT_MS = 120000;
    var CONNECT_TIMEOUT_MS = 30000;
    var PAIR_NO_ANSWER_MS = 8000;
    var LOG_LINES = 40;
    var BACK_HOLD_MS = 1000;
    var MAX_RECONNECTS = 2;
    var RECONNECT_WAIT_MS = 15000;
    var RECONNECT_RESET_MS = 60000;
    // No new video frame or audio packet for this long, with the session still up, means it is
    // stuck: a host that goes quiet ends the session after 10 s, and that reconnects already.
    var STALL_MS = 15000;
    // Far longer than the module takes to start on the TVs it was tried on.
    var STARTUP_TIMEOUT_MS = 30000;

    var mod = null;  // WASM module, once ready
    var state = {
        fatal: false,  // the module failed or can't start; streaming is off
        legacyPlayer: false,  // true only on legacy Tizen 5.5 MSE player needing app restart to change tracks
        keepsPlayer: false,   // true when module stays loaded across streams without page reload
        hosts: {},
        reconnects: 0,  // automatic reconnects in a row
        settings: {},
        scope: null,
        userMoved: false,
        inputMode: 'remote',
        padConnected: false,
        returnFocus: null,
        broadcasts: [],
        pairing: null,
        stream: null,
    };

    // Storage

    var store = {
        get: function (key, fallback) {
            try {
                var raw = localStorage.getItem('tsl.' + key);
                return raw === null ? fallback : JSON.parse(raw);
            } catch (e) {
                return fallback;
            }
        },
        set: function (key, value) {
            try {
                localStorage.setItem('tsl.' + key, JSON.stringify(value));
            } catch (e) {
                console.warn('Could not save ' + key, e);
            }
        },
        remove: function (key) {
            try {
                localStorage.removeItem('tsl.' + key);
            } catch (e) { /* nothing to do */ }
        },
    };

    function saveHosts() {
        var saved = [];
        Object.keys(state.hosts).forEach(function (ip) {
            var h = state.hosts[ip];
            if (h.paired || h.manual) {
                saved.push({ip: h.ip, hostname: h.hostname, steamId: h.steamId, key: h.key, paired: h.paired,
                    manual: h.manual, mac: h.mac || ''});
            }
        });
        store.set('hosts', saved);
    }

    function loadState() {
        var settings = store.get('settings', {});
        Object.keys(DEFAULT_SETTINGS).forEach(function (k) {
            state.settings[k] = settings.hasOwnProperty(k) ? settings[k] : DEFAULT_SETTINGS[k];
        });
        if (state.settings.bandwidth === 100000) {
            state.settings.bandwidth = -2;
        }
        // Discard obsolete or unsupported values saved by earlier pre-NaCl versions on the TV.
        SETTINGS.forEach(function (s) {
            if (s.values.indexOf(state.settings[s.key]) < 0) {
                state.settings[s.key] = DEFAULT_SETTINGS[s.key];
            }
        });
        state.settings.hevc = false;
        state.settings.latency = 1;
        state.settings.keepPlaying = false;
        store.get('hosts', []).forEach(function (h) {
            // Streaming needs the key negotiated when pairing; a host saved without one pairs again.
            var key = typeof h.key === 'string' && h.key.length === 64 ? h.key : '';
            state.hosts[h.ip] = {
                ip: h.ip, hostname: h.hostname || h.ip, steamId: h.steamId || '', key: key,
                paired: !!h.paired && key !== '', manual: !!h.manual, lastSeen: 0, gamesRunning: false,
                mac: typeof h.mac === 'string' ? h.mac : '',
            };
        });
    }

    // Helpers

    function $(sel, root) {
        return (root || document).querySelector(sel);
    }

    function $all(sel, root) {
        return Array.prototype.slice.call((root || document).querySelectorAll(sel));
    }

    function field(root, name) {
        return $('[data-field="' + name + '"]', root);
    }

    function hex(bytes) {
        var out = '';
        for (var i = 0; i < bytes.length; i++) {
            out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
        }
        return out;
    }

    function randomPin() {
        var a = new Uint32Array(1);
        crypto.getRandomValues(a);
        return ('000' + (a[0] % 10000)).slice(-4);
    }

    function isIPv4(s) {
        var parts = s.split('.');
        if (parts.length !== 4) return false;
        return parts.every(function (p) {
            return /^\d{1,3}$/.test(p) && Number(p) <= 255;
        });
    }

    var toastTimer = null;

    // sticky: stays until another toast replaces it, for errors the app can't get past.
    function toast(text, bad, sticky) {
        var el = $('#toast');
        el.textContent = text;
        el.className = 'toast' + (bad ? ' bad' : '');
        el.hidden = false;
        clearTimeout(toastTimer);
        if (sticky) return;
        toastTimer = setTimeout(function () {
            el.hidden = true;
        }, bad ? 7000 : 4000);
    }

    function exitApp() {
        try {
            tizen.application.getCurrentApplication().exit();
        } catch (e) {
            window.close();
        }
    }

    // Focus and spatial navigation

    function focusables() {
        if (!state.scope) return [];
        return $all('.focusable, .tile', state.scope).filter(function (el) {
            return el.offsetParent !== null;
        });
    }

    function setFocus(el) {
        $all('.focused').forEach(function (e) {
            e.classList.remove('focused');
        });
        if (el) {
            el.classList.add('focused');
            if (el.classList.contains('tile')) {
                scrollIntoRow(el);
                updateDetails(el);
            }
            if (el.tagName === 'INPUT') {
                el.focus();
            } else if (document.activeElement && document.activeElement.tagName === 'INPUT') {
                document.activeElement.blur();
            }
        }
        updateHints();
    }

    // Keep a focused tile inside its row, with a tile's width of context on the leading side.
    function scrollIntoRow(el) {
        var row = el.parentNode;
        var pad = 96;
        var left = el.offsetLeft - pad;
        var right = el.offsetLeft + el.offsetWidth + pad;
        if (left < row.scrollLeft) {
            row.scrollLeft = Math.max(0, left);
        } else if (right > row.scrollLeft + row.clientWidth) {
            row.scrollLeft = right - row.clientWidth;
        }
    }

    function focused() {
        var el = $('.focused');
        return el && state.scope && state.scope.contains(el) ? el : null;
    }

    function moveFocus(dir) {
        state.userMoved = true;
        var items = focusables();
        var cur = focused();
        if (!cur) {
            setFocus(items[0]);
            return;
        }
        var a = cur.getBoundingClientRect();
        var best = null, bestScore = Infinity;
        // Distance along the direction between facing edges, and sideways distance between the
        // two spans (0 when they overlap). Ties go to the earlier element in DOM order.
        function gap(lo1, hi1, lo2, hi2) {
            return Math.max(0, lo2 - hi1, lo1 - hi2);
        }
        items.forEach(function (el) {
            if (el === cur) return;
            var b = el.getBoundingClientRect();
            var along, across;
            if (dir === 'left') { along = a.left - b.right; across = gap(a.top, a.bottom, b.top, b.bottom); }
            if (dir === 'right') { along = b.left - a.right; across = gap(a.top, a.bottom, b.top, b.bottom); }
            if (dir === 'up') { along = a.top - b.bottom; across = gap(a.left, a.right, b.left, b.right); }
            if (dir === 'down') { along = b.top - a.bottom; across = gap(a.left, a.right, b.left, b.right); }
            if (along < -1) return;
            var score = Math.max(along, 0) + across * 2;
            if (score < bestScore) {
                bestScore = score;
                best = el;
            }
        });
        if (best) setFocus(best);
    }

    function setScope(root, first) {
        state.scope = root;
        setFocus(first || focusables()[0]);
    }

    // Log overlay: the module's Info+ lines and the UI's own notes, newest last. Toggled
    // with the blue (D) key, or L on a keyboard.

    var logLines = [];

    function note(text) {
        var t = new Date();
        var stamp = ('0' + t.getHours()).slice(-2) + ':' + ('0' + t.getMinutes()).slice(-2) + ':' +
            ('0' + t.getSeconds()).slice(-2);
        logLines.push(stamp + '  ' + text);
        if (logLines.length > LOG_LINES) logLines.shift();
        var el = $('#log-overlay');
        if (el && !el.hidden) el.textContent = logLines.join('\n');
    }

    function toggleLog() {
        var el = $('#log-overlay');
        el.hidden = !el.hidden;
        if (!el.hidden) el.textContent = logLines.length ? logLines.join('\n') : '(no log lines yet)';
    }

    // Hint bar: what the main buttons do here, drawn for the remote or for a gamepad,
    // whichever was used last (Big Picture does the same with its glyphs).

    var GLYPHS = {
        ok: {pad: '<kbd class="glyph">A</kbd>', remote: '<kbd class="key">OK</kbd>'},
        back: {pad: '<kbd class="glyph">B</kbd>', remote: '<kbd class="key">Back</kbd>'},
        forget: {pad: '<kbd class="glyph">X</kbd>', remote: '<kbd class="key red">A</kbd>'},
        change: {pad: '<kbd class="glyph">‹ ›</kbd>', remote: '<kbd class="key">‹ ›</kbd>'},
    };

    function setInputMode(mode) {
        if (state.inputMode === mode) return;
        state.inputMode = mode;
        updateHints();
    }

    function currentHints() {
        var el = focused();
        if (!state.scope || !el) return [];
        if (state.scope === $('#home')) {
            var host = el.hasAttribute('data-ip') && state.hosts[el.getAttribute('data-ip')];
            if (host) {
                return [['ok', canWake(host) ? 'Despertar' : host.paired ? 'Transmitir' : 'Vincular'],
                    ['forget', 'Olvidar'], ['back', 'Salir']];
            }
            return [['ok', 'Seleccionar'], ['back', 'Salir']];
        }
        var hints = [];
        if (el.hasAttribute('data-key')) hints.push(['change', 'Cambiar']);
        hints.push(['ok', el.tagName === 'INPUT' ? 'Siguiente' : 'Seleccionar']);
        hints.push(['back', state.scope === $('#dialog-settings') ? 'Listo' : 'Cancelar']);
        return hints;
    }

    function updateHints() {
        var mode = state.inputMode === 'pad' ? 'pad' : 'remote';
        $('#hints').innerHTML = currentHints().map(function (h) {
            return '<span class="hint">' + GLYPHS[h[0]][mode] + '<span>' + h[1] + '</span></span>';
        }).join('');
    }

    function updateClock() {
        var now = new Date();
        $('#clock').textContent = now.toLocaleTimeString([], {hour: 'numeric', minute: '2-digit'});
    }

    function settingsSummary() {
        var res = state.settings.resolution === 0 ? RESOLUTIONS[0] : RESOLUTIONS[1];
        return res.label + ' a ' + state.settings.framerate + ' fps  ·  H.264  ·  ' +
            (QUALITIES[state.settings.quality] || QUALITIES[1]) + ', ' + bandwidthText(state.settings.bandwidth) +
            '  ·  audio ' + (state.settings.audio ? 'sí' : 'no') + '  ·  abre ' +
            (state.settings.bigPicture ? 'Big Picture' : 'el escritorio');
    }

    function updateDetails(el) {
        var box = $('#details');
        var title = '', line = '';
        var action = el.getAttribute('data-action');
        var host = el.hasAttribute('data-ip') && state.hosts[el.getAttribute('data-ip')];
        if (action === 'add') {
            title = 'Otro equipo';
            line = 'Para conectar un equipo que no aparece automáticamente. Necesitas su dirección IP.';
        } else if (action === 'settings') {
            title = 'Ajustes';
            line = settingsSummary();
        } else if (host) {
            title = host.hostname;
            var st = hostState(host).cls;
            if (st === 'offline' && host.mac) {
                line = 'Sin respuesta. Selecciónalo para enviar Wake-on-LAN si el PC lo tiene activado, o abre Steam allí.';
            } else if (st === 'offline') {
                line = 'Sin respuesta. Abre Steam en el equipo y comprueba que está en la misma red que la TV.';
            } else if (st === 'unpaired') {
                line = 'Aún sin vincular a este televisor. Selecciónalo para obtener el PIN e introducirlo en Steam.';
            } else if (host.gamesRunning) {
                line = 'Hay un juego en ejecución; la transmisión lo recogerá directamente.  ' + settingsSummary();
            } else {
                line = settingsSummary();
            }
        }
        field(box, 'title').textContent = title;
        field(box, 'line').textContent = line;
    }

    // Home screen

    function hostOnline(h) {
        return Date.now() - h.lastSeen < ONLINE_MS;
    }

    // An offline computer that reported its MAC address while it was online can get a wake-up.
    function canWake(h) {
        return !!h.mac && !hostOnline(h);
    }

    // Steam Link's wording for a computer's state.
    function hostState(h) {
        if (!hostOnline(h)) return {cls: 'offline', text: 'Desconectado'};
        if (!h.paired) return {cls: 'unpaired', text: 'Vincular para transmitir'};
        if (h.gamesRunning) return {cls: 'ready busy', text: 'In a game'};
        return {cls: 'ready', text: 'Ready to stream'};
    }

    // EOSType ranges from ihslib's IHS_SteamOSType.
    function osName(t) {
        if (typeof t !== 'number') return '';
        if (t >= 0 && t <= 20) return 'Windows';
        if (t >= -203 && t <= -182) return 'Linux';
        if (t >= -102 && t <= -73) return 'macOS';
        return '';
    }

    function renderHosts() {
        var grid = $('#hosts');
        var cur = focused();
        var keepIp = cur && grid.contains(cur) ? cardKey(cur) : null;

        var hosts = Object.keys(state.hosts).map(function (ip) {
            return state.hosts[ip];
        });
        hosts.sort(function (a, b) {
            var oa = hostOnline(a), ob = hostOnline(b);
            if (oa !== ob) return oa ? -1 : 1;
            if (a.paired !== b.paired) return a.paired ? -1 : 1;
            return a.hostname.localeCompare(b.hostname);
        });

        grid.innerHTML = '';
        hosts.forEach(function (h) {
            var st = hostState(h);
            var tile = document.createElement('div');
            tile.className = 'tile ' + st.cls;
            tile.setAttribute('data-ip', h.ip);
            tile.innerHTML = '<div class="art"><svg viewBox="0 0 64 64"><use href="#i-monitor" xlink:href="#i-monitor"/></svg></div>' +
                '<div class="body"><div class="name"></div><div class="state"></div><div class="meta"></div></div>';
            $('.name', tile).textContent = h.hostname;
            $('.state', tile).textContent = st.text;
            var os = osName(h.ostype);
            $('.meta', tile).textContent = (os ? os + '  ·  ' : '') + h.ip;
            grid.appendChild(tile);
        });
        [['add', 'Other computer', 'Add by IP address', 'i-plus'],
            ['settings', 'Settings', 'Resolution, codec, audio', 'i-gear']].forEach(function (a) {
            var tile = document.createElement('div');
            tile.className = 'tile action';
            tile.setAttribute('data-action', a[0]);
            tile.innerHTML = '<div class="art"><svg viewBox="0 0 64 64"><use href="#' + a[3] + '" xlink:href="#' + a[3] + '"/></svg></div>' +
                '<div class="body"><div class="name"></div><div class="meta"></div></div>';
            $('.name', tile).textContent = a[1];
            $('.meta', tile).textContent = a[2];
            grid.appendChild(tile);
        });

        var online = hosts.filter(hostOnline).length;
        $('#discovery-status').textContent = online > 0
            ? online + (online === 1 ? ' computer is' : ' computers are') + ' ready. Steam has to be running on them.'
            : 'Looking for computers running Steam on this network…';

        if (state.scope === $('#home')) {
            // Until the user moves, follow the list so the first computer found gets focus.
            var again = state.userMoved && keepIp &&
                $('[data-ip="' + keepIp + '"], [data-action="' + keepIp + '"]', grid);
            setFocus(again || grid.firstElementChild);
        }
    }

    var renderPending = false;

    function scheduleRender() {
        if (renderPending || state.stream) return;
        renderPending = true;
        setTimeout(function () {
            renderPending = false;
            renderHosts();
        }, 150);
    }

    function activateHome(el) {
        var action = el.getAttribute('data-action');
        if (action === 'add') return openAddDialog();
        if (action === 'settings') return openSettings();
        var host = state.hosts[el.getAttribute('data-ip')];
        if (!host) return;
        if (canWake(host)) {
            mod.wake(host.mac);
            note('wake-up sent to ' + host.hostname + ' (' + host.mac + ')');
            toast('Sent a wake-up to ' + host.hostname + '. It shows up here once Steam is running.');
            return;
        }
        if (host.paired) {
            startStream(host);
        } else {
            startPairing(host);
        }
    }

    function forgetFocusedHost() {
        var el = focused();
        var ip = el && el.getAttribute('data-ip');
        if (!ip) return;
        var host = state.hosts[ip];
        var dlg = $('#dialog-confirm');
        field(dlg, 'title').textContent = 'Forget ' + host.hostname + '?';
        field(dlg, 'text').textContent = host.paired
            ? 'You will have to pair again to stream from it.'
            : 'It will show up again if it answers on this network.';
        openDialog(dlg, $('[data-action="no"]', dlg), function (action) {
            if (action === 'yes') {
                delete state.hosts[ip];
                saveHosts();
            }
            closeDialog();
            renderHosts();
        });
    }

    // Dialogs

    var dialogHandler = null;

    function cardKey(el) {
        return el ? el.getAttribute('data-ip') || el.getAttribute('data-action') : null;
    }

    function openDialog(dlg, first, handler) {
        if (state.scope === $('#home')) state.returnFocus = cardKey(focused());
        $all('.dialog').forEach(function (d) {
            d.hidden = true;
        });
        dlg.hidden = false;
        dialogHandler = handler;
        setScope(dlg, first);
    }

    function closeDialog() {
        $all('.dialog').forEach(function (d) {
            d.hidden = true;
        });
        dialogHandler = null;
        var key = state.returnFocus;
        state.returnFocus = null;
        var back = key && $('#hosts [data-ip="' + key + '"], #hosts [data-action="' + key + '"]');
        setScope($('#home'), back);
        renderHosts();
    }

    function startPairing(host) {
        if (!mod) return;
        var dlg = $('#dialog-pair');
        var pin = randomPin();
        field(dlg, 'hostname').textContent = host.hostname;
        field(dlg, 'pin').innerHTML = pin.split('').map(function (d) {
            return '<span>' + d + '</span>';
        }).join('');
        var status = field(dlg, 'status');
        status.textContent = 'Contactando con ' + host.hostname + '…';
        status.className = 'status';
        $('[data-action="cancel"]', dlg).textContent = 'Cancelar';
        state.pairing = {ip: host.ip, pin: pin, timer: setTimeout(function () {
            pairFailed('Sin respuesta de Steam en ' + host.hostname + '.');
        }, PAIR_TIMEOUT_MS)};
        openDialog(dlg, null, function () {
            if (state.pairing) mod.cancelPair();
            endPairing();
            closeDialog();
        });
        var sent = mod.pair(host.ip, pin);
        note('pair ' + host.ip + ': probe ' + (sent ? 'sent' : 'NOT sent'));
        if (!sent) {
            pairFailed('No se pudo contactar con ' + host.hostname + '.');
            return;
        }
        state.pairing.answered = false;
        state.pairing.noAnswer = setTimeout(function () {
            if (state.pairing && !state.pairing.answered) {
                field(dlg, 'status').textContent = 'Sin respuesta aún de ' + host.hostname + '. Steam debe estar ' +
                    'abierto allí y su firewall permitir UDP 27036 desde el televisor. Tecla azul (D): registro.';
                field(dlg, 'status').className = 'status bad';
            }
        }, PAIR_NO_ANSWER_MS);
    }

    function endPairing() {
        if (state.pairing) {
            clearTimeout(state.pairing.timer);
            clearTimeout(state.pairing.noAnswer);
        }
        state.pairing = null;
    }

    function pairFailed(reason) {
        if (!state.pairing) return;
        if (mod) mod.cancelPair();
        endPairing();
        var dlg = $('#dialog-pair');
        var status = field(dlg, 'status');
        status.textContent = reason;
        status.className = 'status bad';
        $('[data-action="cancel"]', dlg).textContent = 'Cerrar';
    }

    function openAddDialog() {
        var dlg = $('#dialog-add');
        var input = $('#add-ip');
        input.value = '';
        field(dlg, 'status').textContent = '';
        openDialog(dlg, input, function (action) {
            if (action !== 'add') return closeDialog();
            var ip = input.value.trim();
            if (!isIPv4(ip)) {
                field(dlg, 'status').textContent = 'No es una dirección IPv4 válida.';
                field(dlg, 'status').className = 'status bad';
                return;
            }
            if (!state.hosts[ip]) {
                state.hosts[ip] = {ip: ip, hostname: ip, steamId: '', key: '', paired: false, manual: true,
                    lastSeen: 0, gamesRunning: false};
            } else {
                state.hosts[ip].manual = true;
            }
            saveHosts();
            if (mod) mod.probe(ip);
            closeDialog();
            toast('Añadido ' + ip + '. Aparecerá en línea en cuanto responda Steam.');
        });
    }

    function renderSettings() {
        var list = $('#settings-list');
        var keep = focused() && focused().getAttribute('data-key');
        list.innerHTML = '';
        SETTINGS.forEach(function (s) {
            var li = document.createElement('li');
            li.className = 'focusable';
            li.setAttribute('data-key', s.key);
            var v = state.settings[s.key];
            if (s.kind === 'switch') {
                li.innerHTML = '<span class="label"></span><span class="switch' + (v ? ' on' : '') + '"></span>';
            } else {
                li.innerHTML = '<span class="label"></span><span class="selector"><span class="chev">‹</span>' +
                    '<span class="value"></span><span class="chev">›</span></span>';
                $('.value', li).textContent = s.text(v);
            }
            $('.label', li).textContent = s.label;
            list.appendChild(li);
        });
        if (keep) setFocus($('[data-key="' + keep + '"]', list));
    }

    function changeSetting(el, step) {
        var key = el.getAttribute('data-key');
        var def = SETTINGS.filter(function (s) {
            return s.key === key;
        })[0];
        if (!def) return false;
        var i = def.values.indexOf(state.settings[key]);
        i = (i + step + def.values.length) % def.values.length;
        state.settings[key] = def.values[i];
        store.set('settings', state.settings);
        renderSettings();
        return true;
    }

    function openSettings() {
        renderSettings();
        var dlg = $('#dialog-settings');
        openDialog(dlg, $('#settings-list li'), function (action, el) {
            if (action === 'close' || action === 'cancel') {
                closeDialog();
                if (tracksPending()) {
                    toast('El cambio de audio se aplicará al reiniciar la aplicación.');
                }
                return;
            }
            if (el && el.hasAttribute('data-key')) changeSetting(el, 1);
        });
    }

    // Streaming

    function sendWinKeyCombo(keyCode) {
        var s = state.stream;
        if (!mod || !mod.key || !s) return;
        s.heldKeys['MetaLeft'] = true;
        s.heldKeys[keyCode] = true;
        mod.key('MetaLeft', true);
        mod.key(keyCode, true);
        setTimeout(function () {
            if (!mod || !mod.key) return;
            mod.key(keyCode, false);
            mod.key('MetaLeft', false);
            if (state.stream === s && s.heldKeys) {
                delete s.heldKeys[keyCode];
                delete s.heldKeys['MetaLeft'];
            }
        }, 60);
    }

    function sendDesktopShortcut() {
        sendWinKeyCombo('KeyD');
    }

    function sendMinimizeShortcut() {
        sendWinKeyCombo('KeyM');
    }

    function releaseHeldKeys(s) {
        if (!s || !s.heldKeys || !mod || !mod.key) return;
        Object.keys(s.heldKeys).forEach(function (code) {
            if (s.heldKeys[code]) mod.key(code, false);
        });
        s.heldKeys = {};
    }

    function showStreamStatus(text, spinner) {
        var box = $('#stream-status');
        field(box, 'text').textContent = text;
        if (state.stream) field(box, 'host').textContent = state.stream.hostname;
        $('.throbber', box).className = 'throbber' + (spinner === false ? ' stopped' : '');
        box.hidden = false;
    }

    function startStream(host) {
        if (!mod) return;
        var res = state.settings.resolution === 0 ? RESOLUTIONS[0] : RESOLUTIONS[1];
        state.stream = {
            ip: host.ip, hostname: host.hostname, connected: false, ending: false,
            bigPicture: !!state.settings.bigPicture,
            hadGameRunning: !!host.gamesRunning,
            hostActivity: 0, hostTitle: '', desktopShortcutSent: false, desktopTimer: null,
            heldKeys: {},
            backTimer: null, statsTimer: null, last: null,
            timeout: setTimeout(function () {
                endStream('Tiempo de espera agotado conectando con ' + host.hostname + '.');
            }, CONNECT_TIMEOUT_MS),
        };
        $('#home').hidden = true;
        $('#stream').hidden = false;
        document.body.classList.add('streaming');
        state.scope = null;
        setFocus(null);
        showStreamStatus('Iniciando transmisión…');
        if (mod && mod.stopDiscovery) mod.stopDiscovery();
        // The legacy player keeps the codec and audio of its first stream until the app restarts.
        var tracks = playerTracks();
        var ok = mod.startStream(host.ip, String(host.steamId || ''), host.key, res.w, res.h,
            !!tracks.hevc, !!tracks.audio, !!state.settings.bigPicture,
            Number(state.settings.quality), Number(state.settings.bandwidth), Number(state.settings.framerate),
            tracks.hevc === 'hdr', keepPlaying());
        if (!ok) streamFailed('No se pudo conectar con ' + host.hostname + '.');
    }

    // After a lost connection: stream again once the computer answers.
    function resumeStream(ip) {
        var host = state.hosts[ip];
        if (!host || !host.paired) return;
        toast('Reconectando con ' + host.hostname + '…');
        var deadline = Date.now() + RECONNECT_WAIT_MS;
        (function wait() {
            if (state.stream) return;
            if (hostOnline(host)) {
                startStream(host);
            } else if (Date.now() < deadline) {
                setTimeout(wait, 500);
            } else {
                state.reconnects = 0;
                toast(host.hostname + ' no responde. Puede estar suspendido o fuera de la red.', true);
            }
        })();
    }

    // Before any session exists: just go back to the list.
    function streamFailed(reason) {
        if (!state.stream) return;
        clearTimeout(state.stream.timeout);
        state.stream = null;
        $('#stream').hidden = true;
        $('#home').hidden = false;
        document.body.classList.remove('streaming');
        setScope($('#home'));
        if (mod && mod.startDiscovery) mod.startDiscovery();
        renderHosts();
        toast(reason, true);
    }

    // The codec and audio settings the next stream's tracks follow: the current ones, or on the
    // legacy player, once it has set up its tracks, the ones it set them up with.
    function playerTracks() {
        if (state.legacyPlayer && state.tracks && mod && mod.playerSetUp()) return state.tracks;
        state.tracks = {hevc: state.settings.hevc, audio: state.settings.audio};
        return state.tracks;
    }

    // A codec or audio change the legacy player can't make before the app starts again.
    function tracksPending() {
        var t = state.tracks;
        return !!(state.legacyPlayer && t && mod && mod.playerSetUp() &&
            (t.hevc !== state.settings.hevc || t.audio !== state.settings.audio));
    }

    function keepPlaying() {
        return state.settings.keepPlaying === true;
    }

    // Once a session exists the legacy Tizen 5.5 MSE player stays bound to its tracks until reload.
    // On Tizen 3.0 NaCl, keepsPlayer() is true while legacyPlayer is false so streams return cleanly
    // to the home screen without reloading AND Audio settings can still change between streams.
    function keepsPlayer() {
        return !!(mod && state.keepsPlayer);
    }

    // Ends the stream view once the session is gone: after keepMs back to the list when the
    // player is kept, otherwise a reload after reloadMs, with s.after carried over.
    function scheduleEnd(s, keepMs, reloadMs) {
        clearTimeout(s.finishTimer);
        if (keepsPlayer()) {
            s.finishTimer = setTimeout(finishStream, keepMs);
            return;
        }
        var after = s.after || {};
        try {
            if (after.reconnect) sessionStorage.setItem('tsl.reconnect', JSON.stringify(after.reconnect));
            if (after.message) sessionStorage.setItem('tsl.message', after.message);
        } catch (e) { /* not important */ }
        s.finishTimer = setTimeout(reloadPage, reloadMs);
    }

    function endStream(message) {
        var s = state.stream;
        if (!s || s.ending) return;
        s.ending = true;
        s.after = {message: message || null};
        [s.timeout, s.desktopTimer].forEach(clearTimeout);
        releaseHeldKeys(s);
        showStreamStatus('Desconectando…');
        if (mod) mod.stopStream();
        // If the session never comes back with streamEnded, end anyway.
        scheduleEnd(s, 8000, 6000);
    }

    // The session is gone and the player is kept: back to the list, then reconnect if that was
    // asked for.
    function finishStream() {
        var s = state.stream;
        if (!s) return;
        [s.timeout, s.backTimer, s.finishTimer, s.reconnectReset, s.desktopTimer].forEach(clearTimeout);
        clearInterval(s.statsTimer);
        releaseHeldKeys(s);
        var after = s.after || {};
        state.stream = null;
        if (state.streamMenu || dialogHandler) closeStreamDialogs();
        try {
            if (document.pointerLockElement && document.exitPointerLock) document.exitPointerLock();
        } catch (e) { /* not important */ }
        $('#stream-status').hidden = true;
        $('#stream-hint').hidden = true;
        $('#stream-stats').hidden = true;
        $('#stream-stats').textContent = '';
        $('#stream').hidden = true;
        $('#home').hidden = false;
        document.body.classList.remove('streaming');
        setScope($('#home'));
        if (mod && mod.startDiscovery) mod.startDiscovery();
        renderHosts();
        if (after.reconnect) {
            state.reconnects = after.reconnect.count;
            resumeStream(after.reconnect.ip);
        } else if (after.message) {
            toast(after.message);
        }
    }

    function reloadPage() {
        location.reload();
    }

    // Packet loss over the last interval and in total, what FEC made up for, and the round trip.
    function networkStats(st, last) {
        var lost = (st.videoPacketsLost || 0) - last.videoPacketsLost;
        var sent = (st.videoPackets || 0) - last.videoPackets + lost;
        var total = (st.videoPackets || 0) + (st.videoPacketsLost || 0);
        var pct = function (n, d) { return (d > 0 ? 100 * n / d : 0).toFixed(2) + '%'; };
        return 'loss ' + pct(lost, sent) + ' now, ' + pct(st.videoPacketsLost || 0, total) + ' overall\n' +
            (st.fec ? 'FEC: ' + st.fecRecovered + ' rebuilt, ' + st.fecUnrecoverable + ' lost' : 'FEC: off') +
            '   keyframe requests ' + (st.lostReports || 0) + '\n' +
            'round trip ' + (st.roundTripMs > 0 ? st.roundTripMs.toFixed(1) + ' ms' : 'sin medir') + '\n' +
            'audio: ' + (st.audioGaps || 0) + ' gaps, ' + (st.audioDropped || 0) + ' dropped, lead ' +
            (st.audioLeadMs || 0) + ' ms   longest frame gap ' + (st.maxFrameGapMs || 0) + ' ms';
    }

    // What the TV's player does with the frames: appended, decoded, shown.
    function playerStats(st) {
        var v = $('#tsl-video');
        var hasVideoEl = !!(v && !state.keepsPlayer && v.readyState > 0);
        var q = hasVideoEl && v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
        var decoded = st.picturesDecoded !== undefined ? st.picturesDecoded
            : (q ? q.totalVideoFrames : (hasVideoEl ? v.webkitDecodedFrameCount : undefined));
        var dropped = st.picturesDropped !== undefined ? st.picturesDropped
            : (q ? q.droppedVideoFrames : (hasVideoEl ? v.webkitDroppedFrameCount : undefined));
        var videoExtra = hasVideoEl
            ? (', time ' + v.currentTime.toFixed(1) + ', ready ' + v.readyState)
            : '';
        var audioCfg = st.audioCallbackFrames ? (', hw ' + st.audioCallbackFrames + 'f') : '';
        var latCfg = (st.latencyModeResult !== undefined && st.latencyModeResult !== -999)
            ? (', vdecLow ' + st.latencyModeResult) : '';
        return 'player: appended ' + (st.videoAppended || 0) + ', decoded ' + (decoded === undefined ? '?' : decoded) +
            ', painted ' + (st.picturesPainted || 0) +
            ', dropped ' + (dropped === undefined ? '?' : dropped) + latCfg + videoExtra + '\n' +
            'sessions ' + (st.videoSessionChanges || 0) + ', closes ' + (st.videoTrackCloses || 0) +
            (st.lastCloseReason >= 0 ? ' (reason ' + st.lastCloseReason + ')' : '') +
            ', append error ' + (st.lastAppendError || 0) + ', pipeline errors ' + (st.pipelineErrors || 0) +
            (st.lastPipelineError >= 0 ? ' (' + st.lastPipelineError + ')' : '') +
            '\naudio: ' + (st.audioAppended || 0) + ' frames, ' + (st.audioConcealed || 0) + ' plc, ' +
            (st.silenceFilled || 0) + ' silence' + audioCfg;
    }

    function updateStats() {
        var s = state.stream;
        if (!s || !mod) return;
        var st = null;
        try {
            st = JSON.parse(mod.stats());
        } catch (e) {
            // A trap in the module means its memory is broken; anything else counts as nothing
            // new below.
            if (typeof WebAssembly !== 'undefined' && e instanceof WebAssembly.RuntimeError) {
                tsl.emit('fatal', {message: 'El módulo de vídeo se detuvo: ' + e.message, crashed: true});
                return;
            }
        }
        var now = Date.now();
        checkStalled(s, st, now);
        if (!st || s.ending) return;
        var text = '';
        if (s.last) {
            var dt = (now - s.last.time) / 1000;
            var fps = (st.videoFrames - s.last.videoFrames) / dt;
            var mbps = (st.videoBytes - s.last.videoBytes) * 8 / dt / 1e6;
            text = st.width + 'x' + st.height + ' ' + (st.videoCodec === 5 ? 'HEVC' : 'H.264') +
                (st.colorspace === 5 ? ' HDR' : '') + '\n' +
                fps.toFixed(0) + ' fps   ' + mbps.toFixed(1) + ' Mbit/s\n' +
                'target ' + (st.targetBitrate > 0 ? (st.targetBitrate / 1000).toFixed(1) + ' Mbit/s' : 'sin límite') +
                (st.framerateDen ? ' @ ' + (st.framerateNum / st.framerateDen).toFixed(0) + ' fps' : '') + '\n' +
                'keyframes ' + st.keyFrames + '   failed appends ' + (st.videoFailed + st.audioFailed) + '\n' +
                networkStats(st, s.last) + '\n' + playerStats(st);
        }
        s.last = {time: now, videoFrames: st.videoFrames, videoBytes: st.videoBytes,
            videoPackets: st.videoPackets || 0, videoPacketsLost: st.videoPacketsLost || 0};
        var statsEl = $('#stream-stats');
        if (statsEl && !statsEl.hidden) {
            statsEl.textContent = text;
        }
    }

    // A frozen stream reconnects instead of staying frozen. Only once audio has arrived: with audio
    // off, a still picture can mean no new frames for a while.
    function checkStalled(s, st, now) {
        if (s.ending) return;
        var count = st ? st.videoFrames + st.audioPackets : -1;
        var armed = (s.media && s.media.armed) || !!(st && st.audioPackets > 0);
        if (!s.media || count !== s.media.count) {
            s.media = {count: count, time: now, armed: armed};
            return;
        }
        if (!armed || now - s.media.time < STALL_MS) return;
        var stalled = 'Sin vídeo ni audio durante ' + Math.round((now - s.media.time) / 1000) + ' s, reconectando';
        note('WARN [Stream] ' + stalled);
        console.warn('[Stream] ' + stalled);
        try {
            mod.stopStream();
        } catch (e) { /* reloading ends it anyway */ }
        connectionLost(s, true, 'La transmisión se detuvo, reconectando…');
    }

    // The host going silent, or the stream getting stuck, reconnects a few times in a row at
    // most; the host ending the stream itself doesn't.
    function connectionLost(s, retryable, retryText) {
        s.ending = true;
        [s.timeout, s.desktopTimer].forEach(clearTimeout);
        releaseHeldKeys(s);
        var retry = retryable && state.reconnects < MAX_RECONNECTS;
        s.after = retry ? {reconnect: {ip: s.ip, count: state.reconnects + 1}}
            : {message: retryable ? 'Se perdió la conexión con ' + s.hostname + '.' : s.hostname + ' finalizó la transmisión.'};
        showStreamStatus(retry ? retryText : 'Desconectado');
        // The session ends by itself here (streamEnded); this is in case it doesn't.
        scheduleEnd(s, 8000, retry ? 1500 : 6000);
    }

    function toggleStats() {
        state.settings.stats = !state.settings.stats;
        store.set('settings', state.settings);
        $('#stream-stats').hidden = !state.settings.stats;
    }

    function streamKey(e, down) {
        var s = state.stream;
        if (e.keyCode === KEY.BACK) {
            if (down && !e.repeat && !s.backTimer) {
                s.backTimer = setTimeout(function () {
                    s.backTimer = null;
                    // While connecting there is nothing to come back to: holding Back cancels.
                    if (s.connected) {
                        s.backHeld = true;
                        openStreamMenu();
                    } else {
                        endStream();
                    }
                }, BACK_HOLD_MS);
            } else if (!down && s.backTimer) {
                // A short press is Escape on the host.
                clearTimeout(s.backTimer);
                s.backTimer = null;
                if (s.connected) {
                    mod.key('Escape', true);
                    mod.key('Escape', false);
                }
            }
            return;
        }
        if (e.keyCode === KEY.RED) {
            if (down && !e.repeat) toggleStats();
            return;
        }
        if (!s.connected) return;
        var code = e.code || REMOTE_CODES[e.keyCode];
        if (code && !(down && e.repeat)) {
            if (down) s.heldKeys[code] = true;
            else delete s.heldKeys[code];
            mod.key(code, down);
        }
    }

    // The menu over a stream: hold Back, or the pad's exit combo. While it is open the remote
    // and the pads drive it, and the module stops passing pads on to the computer.
    function openStreamMenu() {
        var s = state.stream;
        if (!s || state.streamMenu) return;
        state.streamMenu = true;
        releaseHeldKeys(s);
        try {
            if (document.pointerLockElement && document.exitPointerLock) document.exitPointerLock();
        } catch (e) { /* ignore */ }
        if (mod) mod.setPadInput(false);
        var dlg = $('#dialog-stream-menu');
        field(dlg, 'hostname').textContent = s.hostname;
        var stats = $('[data-action="stats"]', dlg);
        var label = function () {
            stats.textContent = $('#stream-stats').hidden ? 'Mostrar estadísticas' : 'Ocultar estadísticas';
        };
        label();
        openDialog(dlg, $('[data-action="resume"]', dlg), function (action) {
            if (action === 'stats') {
                toggleStats();
                label();
            } else if (action === 'desktop') {
                closeStreamDialogs();
                sendDesktopShortcut();
                toast('Mostrando escritorio (Win+D)…');
            } else if (action === 'minimize') {
                closeStreamDialogs();
                sendMinimizeShortcut();
                toast('Minimizando ventanas (Win+M)…');
            } else if (action === 'text') {
                openTextDialog();
            } else if (action === 'leave') {
                closeStreamDialogs();
                endStream();
            } else {
                closeStreamDialogs();
            }
        });
    }

    function openTextDialog() {
        var dlg = $('#dialog-text');
        var input = $('#send-text');
        input.value = '';
        openDialog(dlg, input, function (action) {
            var text = input.value;
            if (action === 'send' && text && mod) {
                mod.sendText(text);
                note('sent ' + text.length + ' characters of text');
            }
            closeStreamDialogs();
        });
    }

    function closeStreamDialogs() {
        $all('.dialog').forEach(function (d) {
            d.hidden = true;
        });
        dialogHandler = null;
        state.scope = null;
        setFocus(null);
        state.streamMenu = false;
        if (mod) mod.setPadInput(true);
    }

    function wireStreamInput() {
        var streamEl = $('#stream');
        if (streamEl) {
            streamEl.addEventListener('click', function () {
                if (state.stream && state.stream.connected && !state.streamMenu && !dialogHandler &&
                        streamEl.requestPointerLock) {
                    streamEl.requestPointerLock();
                }
            });
        }
        document.addEventListener('mousemove', function (e) {
            if (!state.stream || !state.stream.connected || state.streamMenu || dialogHandler || !mod) return;
            if (e.movementX || e.movementY) mod.mouseMove(e.movementX | 0, e.movementY | 0);
        });
        document.addEventListener('mousedown', function (e) {
            if (state.stream && state.stream.connected && !state.streamMenu && !dialogHandler && mod) {
                mod.mouseButton(e.button, true);
            }
        });
        document.addEventListener('mouseup', function (e) {
            if (state.stream && state.stream.connected && !state.streamMenu && !dialogHandler && mod) {
                mod.mouseButton(e.button, false);
            }
        });
        document.addEventListener('wheel', function (e) {
            if (state.stream && state.stream.connected && !state.streamMenu && !dialogHandler && mod) {
                mod.mouseWheel(e.deltaY > 0 ? 1 : (e.deltaY < 0 ? -1 : 0));
            }
        }, false);
        document.addEventListener('contextmenu', function (e) {
            if (state.stream) e.preventDefault();
        });
        window.addEventListener('blur', function () {
            if (state.stream) releaseHeldKeys(state.stream);
        });
        document.addEventListener('visibilitychange', function () {
            // The TV's Home button hides the app; don't keep a stream running in the background.
            if (document.hidden && state.stream) endStream();
        });
    }

    // Keys outside the stream

    function onKey(e, down) {
        if (down && (e.keyCode === KEY.BLUE || (e.keyCode === KEY.KEY_L && !state.stream &&
                !(focused() && focused().tagName === 'INPUT')))) {
            if (e.preventDefault) e.preventDefault();
            toggleLog();
            return;
        }
        if (state.stream && !state.streamMenu) {
            e.preventDefault();
            streamKey(e, down);
            return;
        }
        // The Back press that opened the menu is still down; its repeats mustn't close it.
        if (state.streamMenu && e.keyCode === KEY.BACK && state.stream.backHeld) {
            if (!down) state.stream.backHeld = false;
            if (e.preventDefault) e.preventDefault();
            return;
        }
        if (!down) return;
        var el = focused();
        var inInput = el && el.tagName === 'INPUT';
        switch (e.keyCode) {
            case KEY.LEFT:
                if (inInput) return;
                e.preventDefault();
                if (el && el.hasAttribute('data-key')) {
                    changeSetting(el, -1);
                } else {
                    moveFocus('left');
                }
                break;
            case KEY.RIGHT:
                if (inInput) return;
                e.preventDefault();
                if (el && el.hasAttribute('data-key')) {
                    changeSetting(el, 1);
                } else {
                    moveFocus('right');
                }
                break;
            case KEY.UP:
                e.preventDefault();
                moveFocus('up');
                break;
            case KEY.DOWN:
                e.preventDefault();
                moveFocus('down');
                break;
            case KEY.ENTER:
                e.preventDefault();
                if (!el) return;
                if (state.scope === $('#home')) {
                    activateHome(el);
                } else if (dialogHandler) {
                    if (inInput) {
                        moveFocus('down');
                    } else {
                        dialogHandler(el.getAttribute('data-action'), el);
                    }
                }
                break;
            case KEY.RED:
                if (state.scope === $('#home')) forgetFocusedHost();
                break;
            case KEY.BACK:
            case KEY.ESCAPE:
                e.preventDefault();
                if (inInput && e.keyCode === KEY.BACK) {
                    el.blur();
                    return;
                }
                if (state.scope === $('#home')) {
                    exitApp();
                } else if (dialogHandler) {
                    dialogHandler('cancel');
                }
                break;
        }
    }

    // Gamepad navigation outside the stream and in its menu (otherwise the module owns pads).

    var padPrev = {};
    var padRepeat = {};
    var padSeen = {};

    // A short buzz when a controller first shows up, so it's clear the TV has it.
    function hello(pad) {
        if (pad.vibrationActuator && pad.vibrationActuator.playEffect) {
            pad.vibrationActuator.playEffect('dual-rumble', {duration: 150, strongMagnitude: 0.3, weakMagnitude: 0.6})
                .catch(function () {});
        }
        note('controller ' + (pad.index + 1) + ': ' + pad.id);
    }

    function pollPads(ts) {
        if (state.stream && !state.streamMenu) {
            // Do not schedule 60 Hz requestAnimationFrame callbacks in Blink while streaming:
            // on Tizen 3.0's compositor, a 60 Hz JS rAF loop competes with NaCl SwapBuffers
            // for VSync slots and halves the video render rate to 30 fps.
            setTimeout(function () {
                pollPads(Date.now());
            }, 250);
            return;
        }
        var anyPad = false;
        if (navigator.getGamepads) {
            var all = navigator.getGamepads();
            for (var j = 0; j < all.length; j++) {
                if (all[j] && all[j].connected) {
                    anyPad = true;
                    if (!padSeen[all[j].index]) {
                        padSeen[all[j].index] = true;
                        hello(all[j]);
                    }
                } else if (all[j]) {
                    padSeen[all[j].index] = false;
                }
            }
        }
        if (anyPad !== state.padConnected) {
            state.padConnected = anyPad;
            $('#pad-indicator').hidden = !anyPad;
        }
        if ((!state.stream || state.streamMenu) && navigator.getGamepads) {
            var pads = navigator.getGamepads();
            for (var i = 0; i < pads.length; i++) {
                var p = pads[i];
                if (!p || !p.connected) continue;
                var b = function (n) {
                    return p.buttons[n] ? p.buttons[n].pressed : false;
                };
                var ax = p.axes[0] || 0, ay = p.axes[1] || 0;
                var dirs = {
                    up: b(12) || ay < -0.6, down: b(13) || ay > 0.6,
                    left: b(14) || ax < -0.6, right: b(15) || ax > 0.6,
                    a: b(0), b: b(1), x: b(2),
                };
                Object.keys(dirs).forEach(function (k) {
                    var id = i + k;
                    var was = padPrev[id];
                    if (dirs[k] && (!was || (padRepeat[id] && ts > padRepeat[id]))) {
                        padRepeat[id] = ts + (was ? 120 : 400);
                        var codes = {up: KEY.UP, down: KEY.DOWN, left: KEY.LEFT, right: KEY.RIGHT, a: KEY.ENTER,
                            b: KEY.BACK, x: KEY.RED};
                        setInputMode('pad');
                        onKey({keyCode: codes[k], preventDefault: function () {}}, true);
                    }
                    if (!dirs[k]) padRepeat[id] = 0;
                    padPrev[id] = dirs[k];
                });
            }
        }
        requestAnimationFrame(pollPads);
    }

    // Events from the module

    function wireEvents() {
        tsl.on('hostFound', function (d) {
            var h = state.hosts[d.ip];
            if (!h) {
                h = state.hosts[d.ip] = {ip: d.ip, hostname: d.hostname, steamId: '', key: '', paired: false,
                    manual: false, lastSeen: 0, gamesRunning: false, mac: ''};
            }
            var renamed = h.hostname !== d.hostname;
            h.hostname = d.hostname;
            h.lastSeen = Date.now();
            h.gamesRunning = !!d.gamesRunning;
            h.ostype = d.ostype;
            var newMac = !!d.mac && d.mac !== h.mac;
            if (d.mac) h.mac = d.mac;
            if ((renamed || newMac) && (h.paired || h.manual)) saveHosts();
            if (!h.noted) {
                h.noted = true;
                note('found ' + d.hostname + ' at ' + d.ip);
            }
            if (state.pairing && state.pairing.ip === d.ip && !state.pairing.answered) {
                state.pairing.answered = true;
                var status = field($('#dialog-pair'), 'status');
                status.textContent = 'Introduce el PIN en la ventana de Steam en ' + d.hostname + '.';
                status.className = 'status';
                note('pair ' + d.ip + ': host answered, pairing request sent');
            }
            scheduleRender();
        });
        tsl.on('pairProgress', function (d) {
            if (state.pairing && state.pairing.ip === d.ip) {
                field($('#dialog-pair'), 'status').textContent = 'Esperando confirmación del PIN…';
            }
        });
        tsl.on('pairSucceeded', function (d) {
            var h = state.hosts[d.ip];
            if (h) {
                h.paired = true;
                h.steamId = d.steamId;
                h.key = d.key;
                h.hostname = d.hostname || h.hostname;
                saveHosts();
            }
            if (state.pairing && state.pairing.ip === d.ip) {
                endPairing();
                closeDialog();
                toast('Vinculado con ' + (h ? h.hostname : d.ip) + '. Pulsa OK para conectar.');
            }
        });
        tsl.on('pairFailed', function (d) {
            if (state.pairing && state.pairing.ip === d.ip) pairFailed('Error al vincular: ' + d.reason + '.');
        });
        tsl.on('streamProgress', function () {
            if (state.stream) showStreamStatus('Esperando a ' + state.stream.hostname + '…');
        });
        tsl.on('streamStarting', function () {
            if (state.stream) showStreamStatus('Conectando…');
        });
        tsl.on('streamFailed', function (d) {
            if (d.result === 1 && state.stream) {
                // Unauthorized: Steam forgot this device; pair again.
                var h = state.hosts[state.stream.ip];
                if (h) {
                    h.paired = false;
                    h.key = '';
                    saveHosts();
                }
            }
            streamFailed('No se pudo iniciar la transmisión: ' + d.reason + '.');
        });
        tsl.on('hostTitle', function (d) {
            var s = state.stream;
            if (!s) return;
            s.hostTitle = String(d && d.title || '');
        });
        tsl.on('hostActivity', function (d) {
            var s = state.stream;
            if (!s) return;
            s.hostActivity = (d && d.activity) | 0;
            // IHS_StreamActivityGame (2): never trigger Win+D minimize when a game is running.
            if (s.hostActivity === 2 && s.desktopTimer) {
                clearTimeout(s.desktopTimer);
                s.desktopTimer = null;
            }
        });
        tsl.on('streamConnected', function () {
            var s = state.stream;
            if (!s) return;
            s.connected = true;
            clearTimeout(s.timeout);
            // A stream that held up for a while starts the reconnect count over.
            s.reconnectReset = setTimeout(function () {
                state.reconnects = 0;
            }, RECONNECT_RESET_MS);
            $('#stream-status').hidden = true;
            var hint = $('#stream-hint');
            hint.hidden = false;
            setTimeout(function () {
                hint.hidden = true;
            }, 6000);
            $('#stream-stats').hidden = !state.settings.stats;
            s.statsTimer = setInterval(updateStats, 1000);
            if (!s.bigPicture && !s.hadGameRunning && !s.desktopShortcutSent && s.hostActivity !== 2) {
                s.desktopTimer = setTimeout(function () {
                    s.desktopTimer = null;
                    if (state.stream === s && s.connected && !s.ending &&
                            !s.desktopShortcutSent && s.hostActivity !== 2) {
                        s.desktopShortcutSent = true;
                        sendDesktopShortcut();
                    }
                }, 1200);
            }
        });
        tsl.on('streamDisconnected', function (d) {
            var s = state.stream;
            if (!s) return;
            s.connected = false;
            if (!s.ending) connectionLost(s, !!(d && d.timedOut), 'Conexión perdida, reconectando…');
        });
        tsl.on('videoFailed', function (d) {
            var s = state.stream;
            if (!s || s.ending) return;
            s.ending = true;
            [s.timeout, s.desktopTimer].forEach(clearTimeout);
            releaseHeldKeys(s);
            if (mod && mod.stopStream) {
                try { mod.stopStream(); } catch (e) { /* ignore */ }
            }
            s.after = {message: d.restartNeeded
                ? 'Este televisor requiere reiniciar la app para cambiar el formato de vídeo.'
                : 'El televisor no pudo decodificar esta configuración. Prueba 720p o 30 fps' +
                    (state.legacyPlayer ? ' y reinicia la aplicación.' : '.')};
            showStreamStatus('El televisor no pudo reproducir el vídeo', false);
            scheduleEnd(s, 8000, 6000);
        });
        tsl.on('streamEnded', function () {
            var s = state.stream;
            if (!s) return;
            // A stream that ends without the app asking: the host or the network ended it.
            if (!s.ending) connectionLost(s, false);
            // With a kept player, a short pause so the last status stays readable.
            scheduleEnd(s, s.after && s.after.reconnect ? 300 : 1200, 0);
        });
        tsl.on('exitCombo', function () {
            if (!state.stream) return;
            if (state.stream.connected) {
                openStreamMenu();
            } else {
                endStream();
            }
        });
        tsl.on('gamepadConnected', function (d) {
            if (state.stream) toast('Mando ' + (d.slot + 1) + ' conectado');
        });
        tsl.on('gamepadDisconnected', function (d) {
            if (state.stream) toast('Mando ' + (d.slot + 1) + ' desconectado');
        });
        tsl.on('rumble', function (d) {
            var pads = navigator.getGamepads ? navigator.getGamepads() : [];
            var p = pads[d.slot];
            if (p && p.vibrationActuator && p.vibrationActuator.playEffect) {
                p.vibrationActuator.playEffect('dual-rumble', {
                    duration: Math.min(d.ms, 5000), strongMagnitude: d.low / 65535, weakMagnitude: d.high / 65535,
                }).catch(function () {});
            }
        });
        tsl.on('log', function (d) {
            var level = ['FATAL', 'ERROR', 'WARN', 'info'][d.level] || 'debug';
            note(level + ' [' + d.tag + '] ' + d.message);
            if (d.level <= 2) console.warn('[' + d.tag + '] ' + d.message);
        });
        tsl.on('pairFailed', function (d) {
            note('pair ' + d.ip + ' failed: ' + d.reason);
        });
        tsl.on('pairSucceeded', function (d) {
            note('paired with ' + d.ip);
        });
        tsl.on('streamFailed', function (d) {
            note('stream failed: ' + d.reason);
        });
        tsl.on('fatal', function (d) {
            note('FATAL ' + d.message);
            // After a crash nothing calls into the module again: the thread may have died holding
            // one of its locks, and a call would then hang the page before it can reload.
            if (d.crashed) mod = null;
            // A crash during a stream: a fresh page brings a fresh module, and the stream
            // reconnects as it does when it gets stuck.
            if (d.crashed && state.stream) {
                // The input handlers call the module while the stream is connected.
                state.stream.connected = false;
                if (!state.stream.ending) {
                    console.warn('[Stream] ' + d.message);
                    connectionLost(state.stream, true, 'La aplicación sufrió un error, reconectando…');
                }
                return;
            }
            state.fatal = true;
            toast(d.message, true, true);
        });
    }

    // Startup

    function registerRemoteKeys() {
        try {
            ['ColorF0Red', 'ColorF3Blue'].forEach(function (k) {
                tizen.tvinputdevice.registerKey(k);
            });
        } catch (e) { /* not on a TV */ }
    }

    // Discovery also goes to the subnet's broadcast address: a Linux Steam host has been seen
    // ignoring 255.255.255.255 while answering that. The TV's own address comes from
    // systeminfo; the network is taken to be a /24.
    // App version and TV model, for reports; empty in a browser.
    function deviceLabel() {
        var parts = [];
        try {
            parts.push('v' + tizen.application.getCurrentApplication().appInfo.version);
        } catch (e) { /* not on a TV */ }
        try {
            parts.push(tizen.systeminfo.getCapability('http://tizen.org/system/model_name'));
        } catch (e) { /* not on a TV */ }
        try {
            parts.push('Tizen ' + tizen.systeminfo.getCapability('http://tizen.org/feature/platform.version'));
        } catch (e) { /* not on a TV */ }
        if (parts.length) note(parts.join(', '));
        return parts.length ? ' · ' + parts.join(' · ') : '';
    }

    function findLocalBroadcasts() {
        ['ETHERNET_NETWORK', 'WIFI_NETWORK'].forEach(function (prop) {
            try {
                tizen.systeminfo.getPropertyValue(prop, function (net) {
                    var ip = net && net.ipAddress;
                    if (!ip || !isIPv4(ip) || ip === '0.0.0.0') return;
                    var broadcast = ip.split('.').slice(0, 3).concat('255').join('.');
                    if (state.broadcasts.indexOf(broadcast) < 0) {
                        state.broadcasts.push(broadcast);
                        note('TV address ' + ip + ' (' + prop + '), also probing ' + broadcast);
                    }
                }, function () { /* that interface is down */ });
            } catch (e) {
                note('systeminfo ' + prop + ' unavailable: ' + e);
            }
        });
    }

    function probeSaved() {
        if (!mod || state.stream) return;
        state.broadcasts.forEach(function (b) {
            mod.probe(b);
        });
        Object.keys(state.hosts).forEach(function (ip) {
            var h = state.hosts[ip];
            if (h.paired || h.manual) mod.probe(ip);
        });
        scheduleRender();
    }

    function start() {
        loadState();
        registerRemoteKeys();
        wireEvents();
        wireStreamInput();
        document.addEventListener('keydown', function (e) {
            setInputMode('remote');
            onKey(e, true);
        });
        document.addEventListener('keyup', function (e) {
            onKey(e, false);
        });
        $all('.dialog [data-action]').forEach(function (btn) {
            btn.addEventListener('click', function () {
                if (dialogHandler) dialogHandler(btn.getAttribute('data-action'), btn);
            });
        });
        $('#hosts').addEventListener('click', function (e) {
            var card = e.target.closest('.tile');
            if (card) {
                state.userMoved = true;
                setFocus(card);
                activateHome(card);
            }
        });
        setScope($('#home'));
        if (mod && mod.startDiscovery) mod.startDiscovery();
        renderHosts();
        requestAnimationFrame(pollPads);
        updateClock();
        setInterval(updateClock, 10000);
        $('#footer-left').textContent = 'Cliente no oficial · sin afiliación con Valve' + deviceLabel();

        var message = null, reconnect = null;
        try {
            message = sessionStorage.getItem('tsl.message');
            sessionStorage.removeItem('tsl.message');
            reconnect = JSON.parse(sessionStorage.getItem('tsl.reconnect') || 'null');
            sessionStorage.removeItem('tsl.reconnect');
        } catch (e) { /* not important */ }
        if (message) toast(message);
        state.reconnects = reconnect ? reconnect.count : 0;

        // On Tizen 3.0+, the streaming module runs as a NaCl plugin (#nacl_module).
        setTimeout(function () {
            if (!mod && !state.fatal) {
                tsl.emit('fatal', {message: 'El módulo de transmisión no ha respondido tras ' +
                        STARTUP_TIMEOUT_MS / 1000 + ' segundos. Reinicia el televisor por completo y vuelve a abrir la app.'});
            }
        }, STARTUP_TIMEOUT_MS);

        tsl.ready.then(function (m) {
            var identity = store.get('identity', null);
            if (!identity) {
                var bytes = new Uint8Array(40);
                crypto.getRandomValues(bytes);
                identity = m.createIdentity(hex(bytes), 'Samsung TV');
                store.set('identity', identity);
            }
            if (!m.init(identity)) {
                store.remove('identity');
                toast('No se pudo iniciar el módulo de transmisión. Reinicia la aplicación.', true);
                return;
            }
            mod = m;
            state.legacyPlayer = !!(m.legacyPlayer && m.legacyPlayer());
            state.keepsPlayer = !!(m.keepsPlayer ? m.keepsPlayer() : state.legacyPlayer);
            note('module ready, identity ' + identity.slice(5, 21));
            findLocalBroadcasts();
            mod.startDiscovery();
            probeSaved();
            if (reconnect) resumeStream(reconnect.ip);
            setInterval(probeSaved, 4000);
            setInterval(scheduleRender, 3000);
        });
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', start);
    } else {
        start();
    }
})();
