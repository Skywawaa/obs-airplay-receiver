'use strict';

/**
 * server.js — mediasoup SFU server for airplay-stream
 *
 * Receives plain UDP RTP from airplay-stream.exe (H.264 + Opus) via two
 * mediasoup PlainTransports, then forwards the streams to any number of
 * WebRTC browsers connected via the mediasoup-client JS SDK.
 *
 * Environment variables:
 *   PORT            HTTP port for the browser player (default: 8888)
 *   ANNOUNCED_IP    Publicly reachable IP for ICE candidates (default: 127.0.0.1)
 *                   Set this to your LAN IP when browsers connect over the network.
 *   RTC_MIN_PORT    Lower bound of mediasoup UDP port range (default: 40000)
 *   RTC_MAX_PORT    Upper bound of mediasoup UDP port range (default: 40100)
 *
 * Usage:
 *   npm install
 *   node server.js
 */

const mediasoup = require('mediasoup');
const express   = require('express');
const http      = require('http');
const { WebSocketServer } = require('ws');
const path      = require('path');

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

const SERVER_PORT    = parseInt(process.env.PORT          || '8888',  10);
const ANNOUNCED_IP   = process.env.ANNOUNCED_IP            || '127.0.0.1';
const RTC_MIN_PORT   = parseInt(process.env.RTC_MIN_PORT  || '40000', 10);
const RTC_MAX_PORT   = parseInt(process.env.RTC_MAX_PORT  || '40100', 10);

/*
 * Fixed SSRCs — must exactly match the constants in webrtc-output.c:
 *   VIDEO_SSRC  0x00ABCDEF  →  11259375
 *   AUDIO_SSRC  0x00FEDCBA  →  16702650
 */
const VIDEO_SSRC = 0x00ABCDEF;
const AUDIO_SSRC = 0x00FEDCBA;

/** Router codec configuration (PT must match webrtc-output.c constants). */
const MEDIA_CODECS = [
    {
        kind            : 'video',
        mimeType        : 'video/H264',
        clockRate       : 90000,
        preferredPayloadType: 96,
        parameters      : {
            'packetization-mode'      : 1,
            'profile-level-id'        : '42e01f',
            'level-asymmetry-allowed' : 1,
        },
    },
    {
        kind            : 'audio',
        mimeType        : 'audio/opus',
        clockRate       : 48000,
        channels        : 2,
        preferredPayloadType: 111,
        parameters      : {
            minptime    : 10,
            useinbandfec: 1,
        },
    },
];

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

let worker;
let router;
let videoPlainTransport;
let audioPlainTransport;
let videoProducer;
let audioProducer;
let connectedBrowsers = 0;
let viewerKeyframeNeeded = false;
let lastViewerReadyMs = 0;

/* Avoid duplicate keyframe requests fired during reload reconnect storms. */
const VIEWER_READY_DEBOUNCE_MS = 1200;

/* ------------------------------------------------------------------ */
/* mediasoup initialisation                                             */
/* ------------------------------------------------------------------ */

async function startMediasoup() {
    worker = await mediasoup.createWorker({
        logLevel    : 'warn',
        rtcMinPort  : RTC_MIN_PORT,
        rtcMaxPort  : RTC_MAX_PORT,
    });

    worker.on('died', () => {
        console.error('[mediasoup] worker process died — exiting');
        process.exit(1);
    });

    router = await worker.createRouter({ mediaCodecs: MEDIA_CODECS });

    /* ---- Plain transport for video (from airplay-stream.exe) ---- */
    videoPlainTransport = await router.createPlainTransport({
        listenIp  : { ip: '127.0.0.1' },
        rtcpMux   : true,   /* RTCP and RTP share one port */
        comedia   : true,   /* learn remote address from first packet */
    });

    videoProducer = await videoPlainTransport.produce({
        kind          : 'video',
        rtpParameters : {
            codecs : [{
                mimeType    : 'video/H264',
                payloadType : 96,
                clockRate   : 90000,
                parameters  : {
                    'packetization-mode'      : 1,
                    'profile-level-id'        : '42e01f',
                    'level-asymmetry-allowed' : 1,
                },
            }],
            encodings : [{ ssrc: VIDEO_SSRC }],
        },
    });

    /* ---- Plain transport for audio (from airplay-stream.exe) ---- */
    audioPlainTransport = await router.createPlainTransport({
        listenIp  : { ip: '127.0.0.1' },
        rtcpMux   : true,
        comedia   : true,
    });

    audioProducer = await audioPlainTransport.produce({
        kind          : 'audio',
        rtpParameters : {
            codecs : [{
                mimeType    : 'audio/opus',
                payloadType : 111,
                clockRate   : 48000,
                channels    : 2,
                parameters  : { minptime: 10, useinbandfec: 1 },
            }],
            encodings : [{ ssrc: AUDIO_SSRC }],
        },
    });

    console.log(
        `[mediasoup] video plain-transport port : ${videoPlainTransport.tuple.localPort}`
    );
    console.log(
        `[mediasoup] audio plain-transport port : ${audioPlainTransport.tuple.localPort}`
    );
}

/* ------------------------------------------------------------------ */
/* WebSocket signalling handler                                         */
/* ------------------------------------------------------------------ */

/**
 * Handle one browser WebSocket connection.
 * Implements a minimal mediasoup-client signalling protocol:
 *
 *  Browser → Server
 *    { type: 'getRtpCapabilities' }
 *    { type: 'createTransport' }
 *    { type: 'connectTransport', dtlsParameters }
 *    { type: 'consume', rtpCapabilities }
 *
 *  Server → Browser
 *    { type: 'rtpCapabilities', data }
 *    { type: 'transportCreated', data: { id, iceParameters, iceCandidates, dtlsParameters } }
 *    { type: 'transportConnected' }
 *    { type: 'consumed', data: [ { id, producerId, kind, rtpParameters }, … ] }
 *    { type: 'error', message }
 */
function handleBrowserSocket(ws) {
    connectedBrowsers++;
    console.log(`[ws] browser connected (active: ${connectedBrowsers})`);

    /** @type {import('mediasoup').types.WebRtcTransport | null} */
    let recvTransport = null;
    /** @type {import('mediasoup').types.Consumer[]} */
    const recvConsumers = [];

    ws.on('message', async (raw) => {
        let msg;
        try {
            msg = JSON.parse(raw.toString());
        } catch {
            return;
        }

        try {
            switch (msg.type) {
                /* ---- Step 1: browser loads the Device ---- */
                case 'getRtpCapabilities':
                    ws.send(JSON.stringify({
                        type : 'rtpCapabilities',
                        data : router.rtpCapabilities,
                    }));
                    break;

                /* ---- Step 2: create WebRTC recv transport ---- */
                case 'createTransport': {
                    recvTransport = await router.createWebRtcTransport({
                        listenIps  : [{ ip: '0.0.0.0', announcedIp: ANNOUNCED_IP }],
                        enableUdp  : true,
                        enableTcp  : true,
                        preferUdp  : true,
                    });

                    ws.send(JSON.stringify({
                        type : 'transportCreated',
                        data : {
                            id             : recvTransport.id,
                            iceParameters  : recvTransport.iceParameters,
                            iceCandidates  : recvTransport.iceCandidates,
                            dtlsParameters : recvTransport.dtlsParameters,
                        },
                    }));
                    break;
                }

                /* ---- Step 3: browser completes DTLS handshake ---- */
                case 'connectTransport':
                    if (!recvTransport) break;
                    await recvTransport.connect({ dtlsParameters: msg.dtlsParameters });
                    ws.send(JSON.stringify({ type: 'transportConnected' }));
                    break;

                /* ---- Step 4: browser requests consumers ---- */
                case 'consume': {
                    if (!recvTransport) break;
                    const { rtpCapabilities } = msg;
                    const consumers = [];

                    for (const producer of [videoProducer, audioProducer]) {
                        if (!producer) continue;
                        if (!router.canConsume({ producerId: producer.id, rtpCapabilities }))
                            continue;

                        const consumer = await recvTransport.consume({
                            producerId      : producer.id,
                            rtpCapabilities,
                            paused          : false,
                        });

                        /* Keep a strong reference for the whole WebSocket
                         * session. If a Consumer is garbage-collected, mediasoup
                         * closes it and playback can freeze after reload. */
                        recvConsumers.push(consumer);

                        consumer.on('transportclose', () => {
                            const idx = recvConsumers.indexOf(consumer);
                            if (idx !== -1) recvConsumers.splice(idx, 1);
                        });

                        if (consumer.kind === 'video') {
                            console.log('[ws] video consumer created (new viewer)');
                            viewerKeyframeNeeded = true;
                        }

                        consumers.push({
                            id            : consumer.id,
                            producerId    : producer.id,
                            kind          : consumer.kind,
                            rtpParameters : consumer.rtpParameters,
                        });
                    }

                    ws.send(JSON.stringify({ type: 'consumed', data: consumers }));
                    break;
                }

                default:
                    ws.send(JSON.stringify({ type: 'error', message: `Unknown message type: ${msg.type}` }));
            }
        } catch (err) {
            console.error('[ws] signalling error:', err);
            ws.send(JSON.stringify({ type: 'error', message: err.message }));
        }
    });

    ws.on('close', () => {
        connectedBrowsers = Math.max(connectedBrowsers - 1, 0);
        console.log(`[ws] browser disconnected (active: ${connectedBrowsers})`);
        for (const consumer of recvConsumers.splice(0)) {
            try { consumer.close(); } catch {}
        }
        if (recvTransport) {
            recvTransport.close();
            recvTransport = null;
        }
    });

    ws.on('error', (err) => console.error('[ws] socket error:', err));
}

/* ------------------------------------------------------------------ */
/* HTTP + WebSocket server                                              */
/* ------------------------------------------------------------------ */

async function startServer() {
    await startMediasoup();

    const app    = express();
    const server = http.createServer(app);

    /* Static files (index.html, mediasoup-client.bundle.js) */
    app.use(express.static(path.join(__dirname, 'public')));

    /* RTP params endpoint — polled by airplay-stream.exe at startup */
    app.get('/rtp-params', (_req, res) => {
        res.json({
            videoPort : videoPlainTransport.tuple.localPort,
            audioPort : audioPlainTransport.tuple.localPort,
        });
    });

    /* Keyframe request endpoint — called by browser after consumer created,
     * polled by C sender via /keyframe-needed. */
    app.get('/viewer-ready', (_req, res) => {
        const now = Date.now();
        const accepted = (now - lastViewerReadyMs) >= VIEWER_READY_DEBOUNCE_MS;
        if (accepted) {
            viewerKeyframeNeeded = true;
            lastViewerReadyMs = now;
        }
        res.json({ ok: true, accepted });
    });

    /* Keyframe-needed endpoint — polled by airplay-stream.exe.
     * Returns true if a new viewer is waiting for a keyframe. */
    app.get('/keyframe-needed', (_req, res) => {
        const needed = viewerKeyframeNeeded;
        viewerKeyframeNeeded = false;
        res.json({ needed });
    });

    /* WebSocket server for browser signalling */
    const wss = new WebSocketServer({ server });
    wss.on('connection', handleBrowserSocket);

    server.listen(SERVER_PORT, () => {
        console.log(`[server] Listening on http://localhost:${SERVER_PORT}/`);
        console.log('[server] Open that URL in a browser to watch the AirPlay stream.');
        console.log('[server] Start airplay-stream.exe --port', SERVER_PORT);
    });
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

startServer().catch((err) => {
    console.error('[fatal]', err);
    process.exit(1);
});
