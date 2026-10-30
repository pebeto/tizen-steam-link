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
        {label: '1440p', w: 2560, h: 1440},
        {label: '4K', w: 3840, h: 2160},
    ];

    var QUALITIES = {'-1': 'Automatic', 1: 'Fast', 2: 'Balanced', 3: 'Beautiful'};
    // The player's latency mode; Automatic follows the Tizen version and the codec.
    var LATENCIES = {0: 'Automatic', 1: 'Low', 2: 'Ultra low'};

    function codecText(v) {
        return v === 'hdr' ? 'HEVC HDR' : v ? 'HEVC' : 'H.264';
    }

    function bandwidthText(kbps) {
        return kbps === -2 ? 'Adaptive' : kbps < 0 ? 'Automatic' : kbps / 1000 + ' Mbit/s';
    }

    var SETTINGS = [
        {key: 'resolution', label: 'Maximum resolution', values: [0, 1, 2, 3],
            text: function (v) { return RESOLUTIONS[v].label; }},
        {key: 'framerate', label: 'Frame rate', values: [60, 120],
            text: function (v) { return v + ' fps'; }},
        // 'hdr' is HEVC with HDR10 offered: HDR only comes as HEVC.
        {key: 'hevc', label: 'Video codec', values: [false, true, 'hdr'], text: codecText},
        {key: 'latency', label: 'Player latency', values: [0, 1, 2],
            text: function (v) { return LATENCIES[v]; }},
        {key: 'quality', label: 'Quality', values: [-1, 1, 2, 3],
            text: function (v) { return QUALITIES[v]; }},
        {key: 'bandwidth', label: 'Bandwidth limit', values: [-2, -1, 5000, 10000, 15000, 20000, 30000, 50000, 75000, 100000],
            text: bandwidthText},
        {key: 'audio', label: 'Audio', values: [true, false], kind: 'switch',
            text: function (v) { return v ? 'On' : 'Off'; }},
        {key: 'bigPicture', label: 'Start in', values: [false, true],
            text: function (v) { return v ? 'Big Picture' : 'Desktop'; }},
        {key: 'stats', label: 'Show stream statistics', values: [false, true], kind: 'switch',
            text: function (v) { return v ? 'Shown' : 'Hidden'; }},
    ];

    // 20 Mbit/s, Moonlight's default for 1080p at 60 fps: on Wi-Fi a lower limit means smaller
    // keyframes, so fewer packets to lose. Big Picture, as in Steam's own Steam Link app.
    var DEFAULT_SETTINGS = {resolution: 1, framerate: 60, hevc: false, latency: 0, quality: 2, bandwidth: 20000,
        audio: true, bigPicture: true, stats: false};
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
                return [['ok', canWake(host) ? 'Wake' : host.paired ? 'Stream' : 'Pair'], ['forget', 'Forget'],
                    ['back', 'Exit']];
            }
            return [['ok', 'Select'], ['back', 'Exit']];
        }
        var hints = [];
        if (el.hasAttribute('data-key')) hints.push(['change', 'Change']);
        hints.push(['ok', el.tagName === 'INPUT' ? 'Next' : 'Select']);
        hints.push(['back', state.scope === $('#dialog-settings') ? 'Done' : 'Cancel']);
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
        var res = RESOLUTIONS[state.settings.resolution] || RESOLUTIONS[1];
        return 'Up to ' + res.label + (state.settings.framerate > 60 ? ' at ' + state.settings.framerate + ' fps' : '') +
            '  ·  ' + codecText(state.settings.hevc) + '  ·  ' +
            QUALITIES[state.settings.quality] + ', ' + bandwidthText(state.settings.bandwidth) + '  ·  audio ' +
            (state.settings.audio ? 'on' : 'off') + '  ·  opens ' +
            (state.settings.bigPicture ? 'Big Picture' : 'the desktop');
    }

    function updateDetails(el) {
        var box = $('#details');
        var title = '', line = '';
        var action = el.getAttribute('data-action');
        var host = el.hasAttribute('data-ip') && state.hosts[el.getAttribute('data-ip')];
        if (action === 'add') {
            title = 'Other computer';
            line = 'For a computer that doesn\'t show up on its own. You need its IP address.';
        } else if (action === 'settings') {
            title = 'Settings';
            line = settingsSummary();
        } else if (host) {
            title = host.hostname;
            var st = hostState(host).cls;
            if (st === 'offline' && host.mac) {
                line = 'Not answering. Select it to send a wake-up (Wake-on-LAN, if the computer has it on). ' +
                    'Otherwise start Steam on it, on the same network as the TV.';
            } else if (st === 'offline') {
                line = 'Not answering. Start Steam on it, and check that it is on the same network as the TV.';
            } else if (st === 'unpaired') {
                line = 'Not paired with this TV yet. Select it to get a PIN, then type the PIN into Steam on ' +
                    'that computer.';
            } else if (host.gamesRunning) {
                line = 'A game is running there; the stream picks it up.  ' + settingsSummary();
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
        if (!hostOnline(h)) return {cls: 'offline', text: 'Offline'};
        if (!h.paired) return {cls: 'unpaired', text: 'Pair to stream'};
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
            tile.innerHTML = '<div class="art"><svg viewBox="0 0 64 64"><use href="#i-monitor"/></svg></div>' +
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
            tile.innerHTML = '<div class="art"><svg viewBox="0 0 64 64"><use href="#' + a[3] + '"/></svg></div>' +
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
        if (renderPending) return;
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
        status.textContent = 'Contacting ' + host.hostname + '…';
        status.className = 'status';
        $('[data-action="cancel"]', dlg).textContent = 'Cancel';
        state.pairing = {ip: host.ip, pin: pin, timer: setTimeout(function () {
            pairFailed('No answer from Steam on ' + host.hostname + '.');
        }, PAIR_TIMEOUT_MS)};
        openDialog(dlg, null, function () {
            if (state.pairing) mod.cancelPair();
            endPairing();
            closeDialog();
        });
        var sent = mod.pair(host.ip, pin);
        note('pair ' + host.ip + ': probe ' + (sent ? 'sent' : 'NOT sent'));
        if (!sent) {
            pairFailed('Could not contact ' + host.hostname + '.');
            return;
        }
        state.pairing.answered = false;
        state.pairing.noAnswer = setTimeout(function () {
            if (state.pairing && !state.pairing.answered) {
                field(dlg, 'status').textContent = 'No answer from ' + host.hostname + ' yet. Steam has to be ' +
                    'running there, and its firewall has to let in UDP 27036 from the TV. Blue (D) key: log.';
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
        $('[data-action="cancel"]', dlg).textContent = 'Close';
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
                field(dlg, 'status').textContent = 'That is not an IPv4 address.';
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
            toast('Added ' + ip + '. It shows as online once Steam answers.');
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
        var hevcAtOpen = state.settings.hevc;
        var latencyAtOpen = state.settings.latency;
        openDialog(dlg, $('#settings-list li'), function (action, el) {
            if (action === 'close' || action === 'cancel') {
                closeDialog();
                // The module picks the player's latency mode from these settings at page load.
                if (state.settings.hevc !== hevcAtOpen || state.settings.latency !== latencyAtOpen) location.reload();
                return;
            }
            if (el && el.hasAttribute('data-key')) changeSetting(el, 1);
        });
    }

    // Streaming

    function showStreamStatus(text, spinner) {
        var box = $('#stream-status');
        field(box, 'text').textContent = text;
        if (state.stream) field(box, 'host').textContent = state.stream.hostname;
        $('.throbber', box).className = 'throbber' + (spinner === false ? ' stopped' : '');
        box.hidden = false;
    }

    function startStream(host) {
        if (!mod) return;
        var res = RESOLUTIONS[state.settings.resolution] || RESOLUTIONS[1];
        state.stream = {
            ip: host.ip, hostname: host.hostname, connected: false, ending: false,
            backTimer: null, statsTimer: null, last: null,
            timeout: setTimeout(function () {
                endStream('Timed out connecting to ' + host.hostname + '.');
            }, CONNECT_TIMEOUT_MS),
        };
        $('#home').hidden = true;
        $('#stream').hidden = false;
        document.body.classList.add('streaming');
        state.scope = null;
        setFocus(null);
        showStreamStatus('Starting the stream…');
        var ok = mod.startStream(host.ip, String(host.steamId || ''), host.key, res.w, res.h,
            !!state.settings.hevc, !!state.settings.audio, !!state.settings.bigPicture,
            Number(state.settings.quality), Number(state.settings.bandwidth), Number(state.settings.framerate),
            state.settings.hevc === 'hdr');
        if (!ok) streamFailed('Could not contact ' + host.hostname + '.');
    }

    // After a lost connection: stream again once the computer answers.
    function resumeStream(ip) {
        var host = state.hosts[ip];
        if (!host || !host.paired) return;
        toast('Reconnecting to ' + host.hostname + '…');
        var deadline = Date.now() + RECONNECT_WAIT_MS;
        (function wait() {
            if (state.stream) return;
            if (hostOnline(host)) {
                startStream(host);
            } else if (Date.now() < deadline) {
                setTimeout(wait, 500);
            } else {
                state.reconnects = 0;
                toast(host.hostname + ' isn\'t answering. It may be asleep or off the network.', true);
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
        renderHosts();
        toast(reason, true);
    }

    // Once a session exists the player stays bound to it, so the page is reloaded
    // after the session has ended.
    function endStream(message) {
        var s = state.stream;
        if (!s || s.ending) return;
        s.ending = true;
        clearTimeout(s.timeout);
        if (message) {
            try {
                sessionStorage.setItem('tsl.message', message);
            } catch (e) { /* not important */ }
        }
        showStreamStatus('Disconnecting…');
        if (mod) mod.stopStream();
        // If the session never comes back with streamEnded, reload anyway.
        setTimeout(reloadPage, 6000);
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
            'round trip ' + (st.roundTripMs > 0 ? st.roundTripMs.toFixed(1) + ' ms' : 'not measured yet');
    }

    // What the TV's player does with the frames: appended, decoded, shown.
    function playerStats(st) {
        var v = $('#tsl-video');
        var q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
        var decoded = q ? q.totalVideoFrames : v.webkitDecodedFrameCount;
        var dropped = q ? q.droppedVideoFrames : v.webkitDroppedFrameCount;
        return 'player: appended ' + (st.videoAppended || 0) + ', decoded ' + (decoded === undefined ? '?' : decoded) +
            ', dropped ' + (dropped === undefined ? '?' : dropped) + ', time ' + v.currentTime.toFixed(1) +
            ', ready ' + v.readyState + '\n' +
            'sessions ' + (st.videoSessionChanges || 0) + ', closes ' + (st.videoTrackCloses || 0) +
            (st.lastCloseReason >= 0 ? ' (reason ' + st.lastCloseReason + ')' : '') +
            ', append error ' + (st.lastAppendError || 0) + ', pipeline errors ' + (st.pipelineErrors || 0) +
            (st.lastPipelineError >= 0 ? ' (' + st.lastPipelineError + ')' : '') +
            '\naudio: ' + (st.audioAppended || 0) + ' frames, ' + (st.audioConcealed || 0) + ' lost ones filled in';
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
            if (e instanceof WebAssembly.RuntimeError) {
                tsl.emit('fatal', {message: 'The streaming module stopped: ' + e.message, crashed: true});
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
                'target ' + (st.targetBitrate / 1000).toFixed(1) + ' Mbit/s' +  // Steam sends kbit/s
                (st.framerateDen ? ' @ ' + (st.framerateNum / st.framerateDen).toFixed(0) + ' fps' : '') + '\n' +
                'keyframes ' + st.keyFrames + '   failed appends ' + (st.videoFailed + st.audioFailed) + '\n' +
                networkStats(st, s.last) + '\n' + playerStats(st);
        }
        s.last = {time: now, videoFrames: st.videoFrames, videoBytes: st.videoBytes,
            videoPackets: st.videoPackets || 0, videoPacketsLost: st.videoPacketsLost || 0};
        $('#stream-stats').textContent = text;
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
        var stalled = 'No video or audio for ' + Math.round((now - s.media.time) / 1000) + ' s, reconnecting';
        note('WARN [Stream] ' + stalled);
        console.warn('[Stream] ' + stalled);
        try {
            mod.stopStream();
        } catch (e) { /* reloading ends it anyway */ }
        connectionLost(s, true, 'The stream stopped, reconnecting…');
    }

    // The host going silent, or the stream getting stuck, reconnects a few times in a row at
    // most; the host ending the stream itself doesn't.
    function connectionLost(s, retryable, retryText) {
        s.ending = true;
        var retry = retryable && state.reconnects < MAX_RECONNECTS;
        try {
            if (retry) {
                sessionStorage.setItem('tsl.reconnect', JSON.stringify({ip: s.ip, count: state.reconnects + 1}));
            } else {
                sessionStorage.setItem('tsl.message', retryable
                    ? 'Lost the connection to ' + s.hostname + '.'
                    : s.hostname + ' ended the stream.');
            }
        } catch (e) { /* not important */ }
        showStreamStatus(retry ? retryText : 'Disconnected');
        setTimeout(reloadPage, retry ? 1500 : 6000);
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
        if (code && !(down && e.repeat)) mod.key(code, down);
    }

    // The menu over a stream: hold Back, or the pad's exit combo. While it is open the remote
    // and the pads drive it, and the module stops passing pads on to the computer.
    function openStreamMenu() {
        var s = state.stream;
        if (!s || state.streamMenu) return;
        state.streamMenu = true;
        if (mod) mod.setPadInput(false);
        var dlg = $('#dialog-stream-menu');
        field(dlg, 'hostname').textContent = s.hostname;
        var stats = $('[data-action="stats"]', dlg);
        var label = function () {
            stats.textContent = $('#stream-stats').hidden ? 'Show statistics' : 'Hide statistics';
        };
        label();
        openDialog(dlg, $('[data-action="resume"]', dlg), function (action) {
            if (action === 'stats') {
                toggleStats();
                label();
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
        var video = $('#tsl-video');
        video.addEventListener('click', function () {
            if (state.stream && state.stream.connected && video.requestPointerLock) video.requestPointerLock();
        });
        document.addEventListener('mousemove', function (e) {
            if (!state.stream || !state.stream.connected || !mod) return;
            if (e.movementX || e.movementY) mod.mouseMove(e.movementX | 0, e.movementY | 0);
        });
        document.addEventListener('mousedown', function (e) {
            if (state.stream && state.stream.connected && mod) mod.mouseButton(e.button, true);
        });
        document.addEventListener('mouseup', function (e) {
            if (state.stream && state.stream.connected && mod) mod.mouseButton(e.button, false);
        });
        document.addEventListener('wheel', function (e) {
            if (state.stream && state.stream.connected && mod) mod.mouseWheel(e.deltaY > 0 ? 1 : (e.deltaY < 0 ? -1 : 0));
        }, {passive: true});
        document.addEventListener('contextmenu', function (e) {
            if (state.stream) e.preventDefault();
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
                status.textContent = 'Enter the PIN in the dialog Steam opened on ' + d.hostname + '.';
                status.className = 'status';
                note('pair ' + d.ip + ': host answered, pairing request sent');
            }
            scheduleRender();
        });
        tsl.on('pairProgress', function (d) {
            if (state.pairing && state.pairing.ip === d.ip) {
                field($('#dialog-pair'), 'status').textContent = 'Waiting for the PIN…';
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
                toast('Paired with ' + (h ? h.hostname : d.ip) + '. Press OK to stream.');
            }
        });
        tsl.on('pairFailed', function (d) {
            if (state.pairing && state.pairing.ip === d.ip) pairFailed('Pairing failed: ' + d.reason + '.');
        });
        tsl.on('streamProgress', function () {
            if (state.stream) showStreamStatus('Waiting for ' + state.stream.hostname + '…');
        });
        tsl.on('streamStarting', function () {
            if (state.stream) showStreamStatus('Connecting…');
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
            streamFailed('Could not stream: ' + d.reason + '.');
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
        });
        tsl.on('streamDisconnected', function (d) {
            var s = state.stream;
            if (!s) return;
            s.connected = false;
            if (!s.ending) connectionLost(s, !!(d && d.timedOut), 'Connection lost, reconnecting…');
        });
        tsl.on('streamEnded', function () {
            if (state.stream) reloadPage();
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
            if (state.stream) toast('Controller ' + (d.slot + 1) + ' connected');
        });
        tsl.on('gamepadDisconnected', function (d) {
            if (state.stream) toast('Controller ' + (d.slot + 1) + ' disconnected');
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
                    connectionLost(state.stream, true, 'The app hit an error, reconnecting…');
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
        if (!mod) return;
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
        renderHosts();
        requestAnimationFrame(pollPads);
        updateClock();
        setInterval(updateClock, 10000);
        $('#footer-left').textContent = 'Unofficial client · not affiliated with Valve' + deviceLabel();

        var message = null, reconnect = null;
        try {
            message = sessionStorage.getItem('tsl.message');
            sessionStorage.removeItem('tsl.message');
            reconnect = JSON.parse(sessionStorage.getItem('tsl.reconnect') || 'null');
            sessionStorage.removeItem('tsl.reconnect');
        } catch (e) { /* not important */ }
        if (message) toast(message);
        state.reconnects = reconnect ? reconnect.count : 0;

        // Without WebAssembly (TVs older than Tizen 5.5) the module can't load at all, and a module
        // that never finishes starting leaves a page that looks fine but can't do anything. Say so.
        if (typeof WebAssembly !== 'object') {
            tsl.emit('fatal', {message: "This TV can't run the streaming part of the app. It needs " +
                    'Tizen 5.5 or later (2020 models on).'});
        }
        setTimeout(function () {
            if (!mod && !state.fatal) {
                tsl.emit('fatal', {message: "The streaming part of the app hasn't started after " +
                        STARTUP_TIMEOUT_MS / 1000 + ' seconds. Restart the TV fully (hold the power ' +
                        'button), then open the app again.'});
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
                toast('Could not start the streaming module. Restart the app.', true);
                return;
            }
            mod = m;
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
