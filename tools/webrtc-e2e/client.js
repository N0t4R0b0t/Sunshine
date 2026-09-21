// Browser-side client used by the end-to-end tests.
//
// It speaks the same signaling as the real streaming page will: POST an SDP offer to
// /api/webrtc/session, apply the answer, and receive H.264 video plus a data channel. It is shared by
// both test tiers, so a test against the fake host and one against a real Sunshine run identical code.
//
// Everything is exposed on `window.e2e` so the test runner can drive it from outside the page.

const sessions = new Map();
let nextHandle = 1;

/**
 * Wait for ICE gathering to finish, since the server does not accept trickled candidates.
 *
 * @param {RTCPeerConnection} pc Connection whose gathering to wait on.
 * @returns {Promise<void>} Resolves when gathering completes, or after a short cap.
 */
function gathered(pc) {
  return new Promise((resolve) => {
    if (pc.iceGatheringState === 'complete') {
      resolve();
      return;
    }
    pc.addEventListener('icegatheringstatechange', () => {
      if (pc.iceGatheringState === 'complete') {
        resolve();
      }
    });
    setTimeout(resolve, 5000);
  });
}

/**
 * Open a streaming session.
 *
 * @param {object} options What to ask for.
 * @param {number} [options.width] Requested width.
 * @param {number} [options.height] Requested height.
 * @param {number} [options.fps] Requested frame rate.
 * @param {number} [options.bitrate] Requested bitrate in kbps.
 * @param {string} [options.codecs] `any`, or `vp8` to offer only a codec the host cannot send.
 * @param {number} [options.die_after_s] Test knob for the fake host: end the stream after this long.
 * @returns {Promise<object>} `{ok, handle}` on success, or `{ok:false, status, error}`.
 */
async function connect(options = {}) {
  const pc = new RTCPeerConnection({ iceServers: [] });
  const state = {
    pc,
    id: null,
    echoes: [],
    video: document.createElement('video'),
    lastSample: null,
  };

  state.video.autoplay = true;
  state.video.muted = true;
  state.video.playsInline = true;
  document.body.append(state.video);

  const transceiver = pc.addTransceiver('video', { direction: 'recvonly' });
  if (options.codecs === 'vp8') {
    const vp8 = RTCRtpReceiver.getCapabilities('video').codecs.filter((codec) => codec.mimeType === 'video/VP8');
    transceiver.setCodecPreferences(vp8);
  }

  pc.ontrack = (event) => {
    state.video.srcObject = event.streams[0] || new MediaStream([event.track]);
  };

  state.channel = pc.createDataChannel('input');
  state.channel.onmessage = (event) => state.echoes.push(event.data);

  await pc.setLocalDescription();
  await gathered(pc);

  const response = await fetch('/api/webrtc/session', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      type: 'offer',
      sdp: pc.localDescription.sdp,
      width: options.width,
      height: options.height,
      fps: options.fps,
      bitrate: options.bitrate,
      die_after_s: options.die_after_s,
    }),
  });

  let body = {};
  try {
    body = await response.json();
  } catch (_) {
    // The response was not JSON; leave the body empty.
  }

  if (!response.ok || !body.status) {
    pc.close();
    state.video.remove();
    return { ok: false, status: response.status, error: body.error || `HTTP ${response.status}` };
  }

  state.id = body.id;
  await pc.setRemoteDescription({ type: 'answer', sdp: body.sdp });

  const handle = nextHandle++;
  sessions.set(handle, state);
  return { ok: true, handle, id: state.id };
}

/**
 * Read what the browser currently knows about the incoming video.
 *
 * @param {number} handle Session handle from connect().
 * @returns {Promise<object>} Decoder and transport statistics.
 */
async function stats(handle) {
  const { pc, video } = sessions.get(handle);
  const report = await pc.getStats();
  const result = {
    connectionState: pc.connectionState,
    iceConnectionState: pc.iceConnectionState,
    videoWidth: video.videoWidth,
    videoHeight: video.videoHeight,
    framesDecoded: 0,
    keyFramesDecoded: 0,
    framesDropped: 0,
    packetsReceived: 0,
    packetsLost: 0,
    bytesReceived: 0,
    nackCount: 0,
    pliCount: 0,
    frameWidth: 0,
    frameHeight: 0,
    codec: null,
    fmtp: null,
  };

  report.forEach((entry) => {
    if (entry.type === 'inbound-rtp' && entry.kind === 'video') {
      for (const key of ['framesDecoded', 'keyFramesDecoded', 'framesDropped', 'packetsReceived', 'packetsLost', 'bytesReceived', 'nackCount', 'pliCount', 'frameWidth', 'frameHeight']) {
        result[key] = entry[key] ?? 0;
      }
      const codec = entry.codecId ? report.get(entry.codecId) : null;
      if (codec) {
        result.codec = codec.mimeType;
        result.fmtp = codec.sdpFmtpLine || null;
      }
    }
  });

  return result;
}

/**
 * Send a message on the data channel and wait for it to come back.
 *
 * @param {number} handle Session handle from connect().
 * @param {string} text Message to send.
 * @returns {Promise<boolean>} Whether the same text was echoed back in time.
 */
async function echo(handle, text) {
  const state = sessions.get(handle);
  if (state.channel.readyState !== 'open') {
    await new Promise((resolve) => {
      state.channel.addEventListener('open', resolve, { once: true });
      setTimeout(resolve, 5000);
    });
  }
  state.channel.send(text);
  for (let i = 0; i < 50; i++) {
    if (state.echoes.includes(text)) {
      return true;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  return false;
}

/**
 * Close a session by ending the browser's side of the connection, as closing a tab would.
 *
 * @param {number} handle Session handle from connect().
 */
function abandon(handle) {
  sessions.get(handle).pc.close();
}

/**
 * Close a session the polite way, by asking the host to end it.
 *
 * @param {number} handle Session handle from connect().
 * @returns {Promise<number>} HTTP status of the close request.
 */
async function hangUp(handle) {
  const { pc, id } = sessions.get(handle);
  const response = await fetch(`/api/webrtc/session/${id}`, { method: 'DELETE' });
  pc.close();
  return response.status;
}

/**
 * Report the browser's own view of the connection.
 *
 * @param {number} handle Session handle from connect().
 * @returns {string} The connection state.
 */
function connectionState(handle) {
  return sessions.get(handle).pc.connectionState;
}

/**
 * Report which video decoders this browser has, so a run can fail clearly if H.264 is missing.
 *
 * @returns {string[]} MIME types the browser can receive.
 */
function receiverCodecs() {
  return RTCRtpReceiver.getCapabilities('video').codecs.map((codec) => codec.mimeType);
}

/**
 * List the H.264 format parameter strings this browser is willing to receive.
 *
 * @returns {string[]} One `sdpFmtpLine` per H.264 codec entry.
 */
function h264Profiles() {
  return RTCRtpReceiver.getCapabilities('video').codecs
    .filter((codec) => codec.mimeType === 'video/H264')
    .map((codec) => codec.sdpFmtpLine || '');
}

window.e2e = { connect, stats, echo, abandon, hangUp, connectionState, receiverCodecs, h264Profiles };
