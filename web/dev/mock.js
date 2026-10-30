// Stand-in for tsl.js when the UI runs in a desktop browser (index.html?mock). It mimics
// the module's functions and events with two fake computers. Not packaged into the app.
(function () {
    'use strict';

    var FAKE = {
        '192.168.1.20': {hostname: 'gaming-pc', ostype: -184, universe: 1, gamesRunning: false},
        '192.168.1.31': {hostname: 'office-desktop', ostype: 20, universe: 1, gamesRunning: true},
    };
    var frames = 0, bytes = 0, audio = 0, streaming = null, discovery = null;
    var codec = 4, colorspace = 3;  // what the fake computer streams: H.264 or HEVC, SDR or HDR10
    window.tslMockLog = [];

    function emit(type, data, delay) {
        setTimeout(function () {
            window.tslMockLog.push(type);
            window.tslEvent(type, JSON.stringify(data || {}));
        }, delay || 0);
    }

    function announce(ip) {
        var h = FAKE[ip];
        if (h) emit('hostFound', {ip: ip, hostname: h.hostname, ostype: h.ostype, universe: h.universe,
            gamesRunning: h.gamesRunning}, 50);
    }

    var calls = window.tslMockCalls = [];

    function record(name, args) {
        calls.push({name: name, args: Array.prototype.slice.call(args)});
    }

    Object.assign(window.Module, {
        createIdentity: function (randomHex, name) {
            record('createIdentity', arguments);
            return 'tsl1:' + randomHex.slice(0, 16) + ':' + randomHex.slice(16) + ':' + name;
        },
        init: function () {
            record('init', arguments);
            return true;
        },
        startDiscovery: function () {
            record('startDiscovery', arguments);
            var tick = function () {
                Object.keys(FAKE).forEach(announce);
            };
            tick();
            discovery = setInterval(tick, 2000);
        },
        stopDiscovery: function () {
            clearInterval(discovery);
        },
        probe: function (ip) {
            record('probe', arguments);
            announce(ip);
            return true;
        },
        pair: function (ip, pin) {
            record('pair', arguments);
            announce(ip);
            emit('pairProgress', {ip: ip}, 400);
            if (pin === '0000') {
                emit('pairFailed', {ip: ip, result: 1, reason: 'denied on the host'}, 900);
            } else {
                emit('pairSucceeded', {ip: ip, hostname: FAKE[ip] ? FAKE[ip].hostname : ip,
                    steamId: '76561198000000001', key: new Array(65).join('a')}, 1200);
            }
            return true;
        },
        cancelPair: function () {
            record('cancelPair', arguments);
        },
        startStream: function (ip, steamId, key, w, h, hevc, audioOn, bigPicture, quality, bitrate, fps, hdr,
                               keepPlaying) {
            record('startStream', arguments);
            codec = hevc || hdr ? 5 : 4;
            colorspace = hdr ? 5 : 3;
            emit('streamProgress', {ip: ip}, 300);
            emit('streamStarting', {}, 700);
            emit('streamConnected', {}, 1100);
            streaming = setInterval(function () {
                // window.tslMockFrozen = true stands in for a stuck stream (no frames, no audio).
                if (window.tslMockFrozen) return;
                frames += 60;
                bytes += 2500000;
                audio += 100;
            }, 1000);
            return true;
        },
        stopStream: function () {
            record('stopStream', arguments);
            clearInterval(streaming);
            emit('streamDisconnected', {}, 100);
            emit('streamEnded', {}, 300);
        },
        latencyNeedsReload: function () {
            return false;
        },
        legacyPlayer: function () {
            return false;
        },
        playerSetUp: function () {
            return false;
        },
        key: function () {
            record('key', arguments);
        },
        sendText: function () {
            record('sendText', arguments);
        },
        setPadInput: function () {
            record('setPadInput', arguments);
        },
        wake: function () {
            record('wake', arguments);
            return true;
        },
        mouseMove: function () {},
        mouseButton: function () {
            record('mouseButton', arguments);
        },
        mouseWheel: function () {},
        stats: function () {
            return JSON.stringify({connected: !!streaming, videoFrames: frames, keyFrames: 3, videoBytes: bytes,
                audioPackets: audio, width: 1920, height: 1080, videoCodec: codec, colorspace: colorspace, targetBitrate: 20000,
                framerateNum: 60, framerateDen: 1, videoAppended: frames, videoFailed: 0, audioAppended: audio,
                audioFailed: 0, silenceFilled: 0, videoPackets: frames * 18, videoPacketsLost: Math.floor(frames / 40),
                lostReports: 1, roundTripMs: 3.4, fec: true, fecRecovered: Math.floor(frames / 45),
                fecUnrecoverable: 1});
        },
        setLogLevel: function () {},
    });

    setTimeout(window.tslReady, 50);
})();
