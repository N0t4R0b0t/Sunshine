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
 * @param {string} [options.codecs] `any`, or `vp8` to offer only a video codec the host cannot send.
 * @param {string} [options.audio] `opus` (the default), `none` to ask for no audio, or `pcmu` to offer only an audio codec the host cannot send.
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

  // A fixed place and size, so a test can aim the mouse at known coordinates in the picture.
  Object.assign(state.video.style, { position: 'fixed', left: '20px', top: '20px', width: '640px', height: '360px' });
  state.video.autoplay = true;
  // Not muted: a muted element does not pull audio through the decoder, so audio statistics stay at zero.
  state.video.playsInline = true;
  document.body.append(state.video);

  const transceiver = pc.addTransceiver('video', { direction: 'recvonly' });
  if (options.codecs === 'vp8') {
    const vp8 = RTCRtpReceiver.getCapabilities('video').codecs.filter((codec) => codec.mimeType === 'video/VP8');
    transceiver.setCodecPreferences(vp8);
  }

  if (options.audio !== 'none') {
    const audioTransceiver = pc.addTransceiver('audio', { direction: 'recvonly' });
    if (options.audio === 'pcmu') {
      const pcmu = RTCRtpReceiver.getCapabilities('audio').codecs.filter((codec) => codec.mimeType === 'audio/PCMU');
      audioTransceiver.setCodecPreferences(pcmu);
    }
  }

  // Audio and video are attached to the same element, so the browser lines them up with each other.
  pc.ontrack = (event) => {
    if (!state.video.srcObject) {
      state.video.srcObject = new MediaStream();
    }
    state.video.srcObject.addTrack(event.track);
  };

  // One data channel per purpose, matching the real page: "ping" is echoed back, "input" carries
  // keyboard, mouse buttons and controller events reliably, and "input-motion" carries mouse and stick
  // motion unreliably, since a dropped position update is superseded by the next one moments later.
  state.channel = pc.createDataChannel('ping');
  state.channel.onmessage = (event) => state.echoes.push(event.data);
  state.inputChannel = pc.createDataChannel('input');
  state.motionChannel = pc.createDataChannel('input-motion', { ordered: false, maxRetransmits: 0 });

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
    audioPacketsReceived: 0,
    audioPacketsLost: 0,
    audioSamplesReceived: 0,
    audioConcealedSamples: 0,
    audioEnergy: 0,
    audioCodec: null,
    videoPlayoutMs: null,
    audioPlayoutMs: null,
  };

  report.forEach((entry) => {
    if (entry.type === 'inbound-rtp' && entry.kind === 'audio') {
      result.audioPacketsReceived = entry.packetsReceived ?? 0;
      result.audioPacketsLost = entry.packetsLost ?? 0;
      result.audioSamplesReceived = entry.totalSamplesReceived ?? 0;
      result.audioConcealedSamples = entry.concealedSamples ?? 0;
      result.audioEnergy = entry.totalAudioEnergy ?? 0;
      result.audioPlayoutMs = entry.estimatedPlayoutTimestamp ?? null;
      const codec = entry.codecId ? report.get(entry.codecId) : null;
      result.audioCodec = codec ? codec.mimeType : null;
    }
    if (entry.type === 'inbound-rtp' && entry.kind === 'video') {
      result.videoPlayoutMs = entry.estimatedPlayoutTimestamp ?? null;
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

/**
 * Look at the picture the browser is actually showing, so a run can tell real content from a blank frame.
 *
 * The video is drawn onto a small canvas and its pixels summarized.
 *
 * @param {number} handle Session handle from connect().
 * @returns {object} Mean brightness, standard deviation, and how many distinct coarse colours appear.
 */
function pictureStats(handle) {
  const { video } = sessions.get(handle);
  const width = 64;
  const height = 36;
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext('2d', { willReadFrequently: true });
  context.drawImage(video, 0, 0, width, height);
  const { data } = context.getImageData(0, 0, width, height);

  let sum = 0;
  let sumSquares = 0;
  const colours = new Set();
  for (let i = 0; i < data.length; i += 4) {
    const luma = 0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2];
    sum += luma;
    sumSquares += luma * luma;
    // Quantize each channel to 4 levels, so compression noise does not count as a new colour.
    colours.add(((data[i] >> 6) << 4) | ((data[i + 1] >> 6) << 2) | (data[i + 2] >> 6));
  }
  const pixels = data.length / 4;
  const mean = sum / pixels;
  return { mean, stddev: Math.sqrt(Math.max(0, sumSquares / pixels - mean * mean)), distinctColours: colours.size };
}

/**
 * Start capturing keyboard, mouse and controller input from the page, using the real input module.
 *
 * @param {number} handle Session handle from connect().
 * @returns {Promise<void>} Resolves once the input channel is open and capture has begun.
 */
async function startInput(handle) {
  const state = sessions.get(handle);
  if (state.inputChannel.readyState !== 'open') {
    await new Promise((resolve) => {
      state.inputChannel.addEventListener('open', resolve, { once: true });
      setTimeout(resolve, 5000);
    });
  }

  state.input = new window.e2eInputModule.InputSender({
    send: (text) => {
      if (state.inputChannel.readyState === 'open') {
        state.inputChannel.send(text);
      }
    },
    sendMotion: (text) => {
      if (state.motionChannel.readyState === 'open') {
        state.motionChannel.send(text);
      }
    },
    element: state.video,
    getGamepads: () => window.e2eGamepads,
  });
  state.input.start();
}

/**
 * Stop capturing input, releasing anything still held.
 *
 * @param {number} handle Session handle from connect().
 */
function stopInput(handle) {
  sessions.get(handle).input.stop();
}

/**
 * Switch between relative and absolute mouse movement, as pointer lock would.
 *
 * Headless Chrome cannot lock the pointer, so the mode is set directly.
 *
 * @param {number} handle Session handle from connect().
 * @param {boolean} relative Whether to send relative movement.
 */
function setRelative(handle, relative) {
  sessions.get(handle).input.setRelative(relative);
}

/**
 * Make the browser report mouse movement, as a locked pointer does with `movementX` and `movementY`.
 *
 * @param {number} handle Session handle from connect().
 * @param {number} dx Horizontal movement.
 * @param {number} dy Vertical movement.
 */
function moveMouseBy(handle, dx, dy) {
  sessions.get(handle).video.dispatchEvent(new MouseEvent('mousemove', { movementX: dx, movementY: dy, bubbles: true }));
}

/**
 * Plug in, change, or unplug a fake controller. Headless Chrome has none of its own.
 *
 * @param {number} index Controller slot.
 * @param {object|null} spec `null` to unplug, or `{buttons: {index: pressed-or-value}, axes: [...]}`.
 */
function setGamepad(index, spec) {
  if (spec === null) {
    window.e2eGamepads[index] = null;
    return;
  }
  const buttons = Array.from({ length: 17 }, () => ({ pressed: false, value: 0 }));
  for (const [button, value] of Object.entries(spec.buttons || {})) {
    buttons[button] = { pressed: value === true || value >= 0.5, value: value === true ? 1 : value };
  }
  window.e2eGamepads[index] = { connected: true, buttons, axes: spec.axes || [0, 0, 0, 0] };
}

/**
 * Send raw text on the input channel, bypassing the input module, to test how the host copes with junk.
 *
 * @param {number} handle Session handle from connect().
 * @param {string} text What to send.
 */
function sendRawInput(handle, text) {
  sessions.get(handle).inputChannel.send(text);
}

window.e2eGamepads = [];

window.e2e = { startInput, stopInput, setRelative, moveMouseBy, setGamepad, sendRawInput, pictureStats, connect, stats, echo, abandon, hangUp, connectionState, receiverCodecs, h264Profiles };
