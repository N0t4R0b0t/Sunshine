/**
 * Negotiates and manages a browser streaming session over WebRTC.
 *
 * This talks to `POST /api/webrtc/session` and `DELETE /api/webrtc/session/<id>` (see
 * `src/confighttp.cpp` and `src/webrtc/session.cpp`) and knows nothing about Vue, so it can be
 * driven from a page or from a test. `StreamConnection` does the WebRTC and network work; the
 * handful of pure functions below it (building the request body, reading `getStats()`) are kept
 * separate so they can be tested without a real `RTCPeerConnection`.
 */

/** Longest time to wait for ICE candidate gathering before giving up on an offer. */
const GATHERING_TIMEOUT_MS = 5000

/**
 * Build the JSON body for the session-open request.
 *
 * Only fields the caller actually set are included, so the host's own defaults apply to the rest
 * (see `webrtc::stream_request_t` in `src/webrtc/session.h`).
 *
 * @param {RTCSessionDescriptionInit} offer The local offer, once ICE gathering has finished.
 * @param {object} options
 * @param {number} [options.width] Requested stream width in pixels.
 * @param {number} [options.height] Requested stream height in pixels.
 * @param {number} [options.fps] Requested frame rate.
 * @param {number} [options.bitrate] Requested video bitrate in kilobits per second.
 * @param {number} [options.appid] Application to launch; omitted or 0 streams the desktop as it is.
 * @returns {object} The request body.
 */
export function buildSessionRequest(offer, options = {}) {
  const body = { type: 'offer', sdp: offer.sdp }
  for (const key of ['width', 'height', 'fps', 'bitrate', 'appid']) {
    if (options[key] !== undefined && options[key] !== null) {
      body[key] = options[key]
    }
  }
  return body
}

/**
 * Reduce a `RTCStatsReport` to the numbers a stats overlay or a health check cares about.
 *
 * @param {RTCStatsReport} report From `RTCPeerConnection.getStats()`.
 * @returns {object} Video and audio numbers, each `null` where nothing was reported yet.
 */
export function summarizeStats(report) {
  const summary = { video: null, audio: null, candidateType: null }

  report.forEach((entry) => {
    if (entry.type === 'inbound-rtp' && entry.kind === 'video') {
      const codec = entry.codecId ? report.get(entry.codecId) : null
      summary.video = {
        framesDecoded: entry.framesDecoded ?? 0,
        framesDropped: entry.framesDropped ?? 0,
        frameWidth: entry.frameWidth ?? 0,
        frameHeight: entry.frameHeight ?? 0,
        packetsLost: entry.packetsLost ?? 0,
        packetsReceived: entry.packetsReceived ?? 0,
        bytesReceived: entry.bytesReceived ?? 0,
        jitterMs: (entry.jitter ?? 0) * 1000,
        codec: codec ? codec.mimeType : null,
      }
    } else if (entry.type === 'inbound-rtp' && entry.kind === 'audio') {
      const codec = entry.codecId ? report.get(entry.codecId) : null
      summary.audio = {
        packetsLost: entry.packetsLost ?? 0,
        packetsReceived: entry.packetsReceived ?? 0,
        codec: codec ? codec.mimeType : null,
      }
    } else if (entry.type === 'candidate-pair' && entry.state === 'succeeded' && entry.nominated) {
      const local = entry.localCandidateId ? report.get(entry.localCandidateId) : null
      summary.candidateType = local ? local.candidateType : null
      summary.roundTripMs = entry.currentRoundTripTime !== undefined ? entry.currentRoundTripTime * 1000 : null
    }
  })

  return summary
}

/**
 * Wait for a peer connection's ICE gathering to finish, or give up after a timeout.
 *
 * The host does not accept trickled candidates, so the offer sent to it must already be complete.
 *
 * @param {RTCPeerConnection} pc The connection.
 * @returns {Promise<void>} Resolves once gathering is complete, or after the timeout either way.
 */
function waitForIceGathering(pc) {
  if (pc.iceGatheringState === 'complete') {
    return Promise.resolve()
  }
  return new Promise((resolve) => {
    const onChange = () => {
      if (pc.iceGatheringState === 'complete') {
        pc.removeEventListener('icegatheringstatechange', onChange)
        clearTimeout(timer)
        resolve()
      }
    }
    pc.addEventListener('icegatheringstatechange', onChange)
    const timer = setTimeout(() => {
      pc.removeEventListener('icegatheringstatechange', onChange)
      resolve()
    }, GATHERING_TIMEOUT_MS)
  })
}

/**
 * A single browser streaming session: negotiates it, exposes what arrives, and can close it.
 */
export class StreamConnection {
  /**
   * @param {object} options
   * @param {(url: string, init: RequestInit) => Promise<Response>} options.fetch A `fetch`-compatible
   *   function; the page passes `apiFetch` from `fetch_utils.js` so requests carry a CSRF token.
   * @param {string} [options.stunServer] Optional STUN server, passed through to `RTCPeerConnection`.
   */
  constructor({ fetch, stunServer }) {
    this.fetch = fetch
    this.stunServer = stunServer
    this.id = null
    this.pc = null
    this.inputChannel = null
    this.motionChannel = null
    this.pingChannel = null

    /** @type {((track: MediaStreamTrack) => void)|null} Called for each track the host sends. */
    this.onTrack = null
    /** @type {((state: RTCPeerConnectionState) => void)|null} Called when the connection's state changes. */
    this.onConnectionStateChange = null
    /** @type {(() => void)|null} Called once the input channel is open and ready to send. */
    this.onInputChannelOpen = null
  }

  /**
   * Open a session: build an offer, gather candidates, ask the host to accept it, and apply its answer.
   *
   * @param {object} options Passed to buildSessionRequest(); see there for the fields.
   * @returns {Promise<void>} Resolves once the answer has been applied. The connection itself may
   *   still take a moment longer to reach `connected`; watch `onConnectionStateChange` for that.
   * @throws {Error} If the host refuses the offer or the request fails, with the host's reason
   *   where one was given.
   */
  async open(options = {}) {
    const config = { iceServers: [] }
    if (this.stunServer) {
      config.iceServers.push({ urls: this.stunServer })
    }

    const pc = new RTCPeerConnection(config)
    this.pc = pc

    for (const transceiver of [pc.addTransceiver('video', { direction: 'recvonly' }), pc.addTransceiver('audio', { direction: 'recvonly' })]) {
      // Ask Chrome to play out frames as soon as they arrive rather than holding a buffer to smooth
      // over jitter, which is the right trade for a host on the same network. Firefox has no
      // equivalent yet, so it keeps its default buffering.
      if ('playoutDelayHint' in transceiver.receiver) {
        transceiver.receiver.playoutDelayHint = 0
      }
    }

    pc.addEventListener('track', (event) => {
      this.onTrack?.(event.track)
    })
    pc.addEventListener('connectionstatechange', () => {
      this.onConnectionStateChange?.(pc.connectionState)
    })

    this.inputChannel = pc.createDataChannel('input')
    this.inputChannel.addEventListener('open', () => this.onInputChannelOpen?.())

    // Mouse and stick motion is a continuous stream of state, where a dropped update just gets
    // superseded by the next one a few milliseconds later. Sending it reliably and in order, like the
    // "input" channel above, would instead make a lost packet block every update behind it until it is
    // retransmitted - the opposite of what continuous motion needs.
    this.motionChannel = pc.createDataChannel('input-motion', { ordered: false, maxRetransmits: 0 })

    this.pingChannel = pc.createDataChannel('ping')

    await pc.setLocalDescription()
    await waitForIceGathering(pc)

    const response = await this.fetch('/api/webrtc/session', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(buildSessionRequest(pc.localDescription, options)),
    })

    let body = {}
    try {
      body = await response.json()
    } catch {
      // Fall through with an empty body; the status code alone still says whether this failed.
    }

    if (!response.ok || !body.status) {
      pc.close()
      this.pc = null
      throw new Error(body.error || `The host refused the connection (HTTP ${response.status})`)
    }

    this.id = body.id
    await pc.setRemoteDescription({ type: 'answer', sdp: body.sdp })
  }

  /**
   * Send a message on the input data channel.
   *
   * @param {string} text The message, already encoded (see `stream/input.js`).
   */
  sendInput(text) {
    if (this.inputChannel?.readyState === 'open') {
      this.inputChannel.send(text)
    }
  }

  /**
   * Send a message on the unreliable motion data channel, used for continuous mouse and stick state
   * where a dropped update does not need retransmission.
   *
   * @param {string} text The message, already encoded (see `stream/input.js`).
   */
  sendMotion(text) {
    if (this.motionChannel?.readyState === 'open') {
      this.motionChannel.send(text)
    }
  }

  /**
   * Read the connection's current statistics.
   *
   * @returns {Promise<object>} See summarizeStats(). Empty numbers if there is no connection.
   */
  async stats() {
    if (!this.pc) {
      return summarizeStats(new Map())
    }
    return summarizeStats(await this.pc.getStats())
  }

  /**
   * Close the session: tell the host, and tear down the local connection either way.
   *
   * @returns {Promise<void>} Resolves once the host has been asked, even if that request fails.
   */
  async close() {
    const id = this.id
    const pc = this.pc
    this.id = null
    this.pc = null
    this.inputChannel = null
    this.motionChannel = null
    this.pingChannel = null

    if (pc) {
      pc.close()
    }
    if (id) {
      try {
        await this.fetch(`/api/webrtc/session/${id}`, { method: 'DELETE' })
      } catch {
        // The connection is already torn down locally either way.
      }
    }
  }
}
