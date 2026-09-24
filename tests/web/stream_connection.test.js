import { afterEach, describe, expect, it, vi } from 'vitest'

import { buildSessionRequest, selectVideoCodecs, StreamConnection, summarizeStats } from '../../src_assets/common/assets/web/stream/connection.js'

describe('buildSessionRequest', () => {
  it('always carries the offer type and SDP', () => {
    expect(buildSessionRequest({ sdp: 'v=0...' })).toEqual({ type: 'offer', sdp: 'v=0...' })
  })

  it('includes only the options the caller actually set', () => {
    const body = buildSessionRequest({ sdp: 'x' }, { width: 1920, height: 1080 })
    expect(body).toEqual({ type: 'offer', sdp: 'x', width: 1920, height: 1080 })
  })

  it('includes all five known options when given', () => {
    const body = buildSessionRequest({ sdp: 'x' }, { width: 1280, height: 720, fps: 60, bitrate: 20000, appid: 3 })
    expect(body).toEqual({ type: 'offer', sdp: 'x', width: 1280, height: 720, fps: 60, bitrate: 20000, appid: 3 })
  })

  it('treats appid 0 as a real value, not an absent one', () => {
    // appid 0 means "stream the desktop", which is a meaningful choice, not "unset".
    expect(buildSessionRequest({ sdp: 'x' }, { appid: 0 })).toEqual({ type: 'offer', sdp: 'x', appid: 0 })
  })

  it('drops undefined and null but keeps zero and empty-ish falsy numbers', () => {
    const body = buildSessionRequest({ sdp: 'x' }, { width: undefined, height: null, fps: 0, bitrate: 500 })
    expect(body).toEqual({ type: 'offer', sdp: 'x', fps: 0, bitrate: 500 })
  })

  it('ignores unknown fields', () => {
    const body = buildSessionRequest({ sdp: 'x' }, { width: 640, mischief: true })
    expect(body).toEqual({ type: 'offer', sdp: 'x', width: 640 })
  })
})

/** Build a fake RTCStatsReport, which is just something with a Map-like forEach and get(). */
function statsReport(entries) {
  const map = new Map(entries.map((entry) => [entry.id, entry]))
  return map
}

/** A codec capability entry the way `RTCRtpReceiver.getCapabilities('video').codecs` reports it. */
const codec = (mimeType, sdpFmtpLine) => ({ mimeType, clockRate: 90000, sdpFmtpLine })

describe('selectVideoCodecs', () => {
  const allCodecs = [
    codec('video/VP8'),
    codec('video/VP9', 'profile-id=0'),
    codec('video/H264', 'packetization-mode=1;profile-level-id=42e01f'),
    codec('video/H264', 'packetization-mode=1;profile-level-id=64001f'),
    codec('video/AV1', 'profile=0'),
    codec('video/AV1', 'profile=1'),
    codec('video/red'),
  ]

  it('offers only H.264 by default, dropping every other codec the browser supports', () => {
    const chosen = selectVideoCodecs(allCodecs, undefined)
    expect(chosen.every((c) => c.mimeType === 'video/H264')).toBe(true)
    expect(chosen).toHaveLength(2)
  })

  it('offers only H.264 when explicitly asked for it', () => {
    expect(selectVideoCodecs(allCodecs, 'h264').every((c) => c.mimeType === 'video/H264')).toBe(true)
  })

  it('offers only AV1 when AV1 is requested, deliberately with no H.264 fallback', () => {
    const chosen = selectVideoCodecs(allCodecs, 'av1')
    expect(chosen.every((c) => c.mimeType === 'video/AV1')).toBe(true)
    expect(chosen).toHaveLength(2)
  })

  it('offers nothing, rather than quietly falling back to H.264, when AV1 was asked for but the browser has none', () => {
    const noAv1 = allCodecs.filter((c) => c.mimeType !== 'video/AV1')
    expect(selectVideoCodecs(noAv1, 'av1')).toEqual([])
  })

  it('returns nothing rather than throwing when the browser reports no usable codec at all', () => {
    expect(selectVideoCodecs([codec('video/VP8')], 'h264')).toEqual([])
    expect(selectVideoCodecs([], 'av1')).toEqual([])
  })
})

describe('summarizeStats', () => {
  it('reports nothing when the report has nothing', () => {
    expect(summarizeStats(statsReport([]))).toEqual({ video: null, audio: null, candidateType: null })
  })

  it('reads the video inbound-rtp entry and its codec', () => {
    const report = statsReport([
      { id: 'c1', type: 'codec', mimeType: 'video/H264' },
      {
        id: 'v1',
        type: 'inbound-rtp',
        kind: 'video',
        codecId: 'c1',
        framesDecoded: 900,
        framesDropped: 3,
        frameWidth: 1920,
        frameHeight: 1080,
        packetsLost: 2,
        packetsReceived: 5000,
        bytesReceived: 123456,
        jitter: 0.01,
      },
    ])
    expect(summarizeStats(report).video).toEqual({
      framesDecoded: 900,
      framesDropped: 3,
      frameWidth: 1920,
      frameHeight: 1080,
      packetsLost: 2,
      packetsReceived: 5000,
      bytesReceived: 123456,
      jitterMs: 10,
      codec: 'video/H264',
    })
  })

  it('reads the audio inbound-rtp entry independently of video', () => {
    const report = statsReport([
      { id: 'c2', type: 'codec', mimeType: 'audio/opus' },
      { id: 'a1', type: 'inbound-rtp', kind: 'audio', codecId: 'c2', packetsLost: 1, packetsReceived: 900 },
    ])
    const summary = summarizeStats(report)
    expect(summary.audio).toEqual({ packetsLost: 1, packetsReceived: 900, codec: 'audio/opus' })
    expect(summary.video).toBeNull()
  })

  it('copes with an inbound-rtp entry that has no matching codec entry', () => {
    const report = statsReport([{ id: 'v1', type: 'inbound-rtp', kind: 'video', framesDecoded: 5 }])
    expect(summarizeStats(report).video.codec).toBeNull()
  })

  it('fills in zero for fields a report has not started sending yet', () => {
    const report = statsReport([{ id: 'v1', type: 'inbound-rtp', kind: 'video' }])
    expect(summarizeStats(report).video).toMatchObject({ framesDecoded: 0, framesDropped: 0, packetsLost: 0 })
  })

  it('reads the round trip time from the active candidate pair, in milliseconds', () => {
    const report = statsReport([
      { id: 'l1', type: 'local-candidate', candidateType: 'host' },
      { id: 'p1', type: 'candidate-pair', state: 'succeeded', nominated: true, localCandidateId: 'l1', currentRoundTripTime: 0.012 },
    ])
    const summary = summarizeStats(report)
    expect(summary.candidateType).toBe('host')
    expect(summary.roundTripMs).toBeCloseTo(12)
  })

  it('ignores a candidate pair that is not the nominated, succeeded one', () => {
    const report = statsReport([
      { id: 'l1', type: 'local-candidate', candidateType: 'host' },
      { id: 'p1', type: 'candidate-pair', state: 'failed', nominated: true, localCandidateId: 'l1', currentRoundTripTime: 0.5 },
      { id: 'p2', type: 'candidate-pair', state: 'succeeded', nominated: false, localCandidateId: 'l1', currentRoundTripTime: 0.5 },
    ])
    expect(summarizeStats(report).candidateType).toBeNull()
  })
})

/**
 * A minimal fake RTCPeerConnection, just capable enough to drive StreamConnection through a
 * successful negotiation without a real network or browser WebRTC stack.
 */
class FakePeerConnection extends EventTarget {
  static instances = []

  constructor(config) {
    super()
    this.config = config
    this.connectionState = 'new'
    this.iceGatheringState = 'complete'
    this.localDescription = null
    this.transceivers = []
    this.channels = []
    this.closed = false
    FakePeerConnection.instances.push(this)
  }

  addTransceiver(kind, init) {
    // playoutDelayHint must be a real, present key (even if undefined), the way a browser that
    // supports it always has the property - only the "unsupported browser" test below omits it.
    const transceiver = { kind, init, receiver: { playoutDelayHint: undefined }, setCodecPreferences: vi.fn() }
    this.transceivers.push(transceiver)
    return transceiver
  }

  createDataChannel(label, init) {
    const channel = new EventTarget()
    channel.label = label
    channel.init = init
    channel.readyState = 'connecting'
    channel.send = vi.fn()
    channel.open = () => {
      channel.readyState = 'open'
      channel.dispatchEvent(new Event('open'))
    }
    this.channels.push(channel)
    return channel
  }

  async setLocalDescription() {
    this.localDescription = { type: 'offer', sdp: 'fake-offer-sdp' }
  }

  async setRemoteDescription(description) {
    this.remoteDescription = description
  }

  async getStats() {
    return statsReport([])
  }

  emitTrack(track) {
    const event = new Event('track')
    event.track = track
    this.dispatchEvent(event)
  }

  setConnectionState(state) {
    this.connectionState = state
    this.dispatchEvent(new Event('connectionstatechange'))
  }

  close() {
    this.closed = true
  }
}

/**
 * A `fetch`-compatible function that returns canned JSON, and records what it was called with.
 *
 * @param {object} json The body to return.
 * @param {number} [status] HTTP status to report; defaults to 200.
 * @returns {(url: string, init: RequestInit) => Promise<Response>} The fake fetch.
 */
function fakeFetch(json, status = 200) {
  const fn = vi.fn(async () => ({
    ok: status >= 200 && status < 300,
    status,
    json: async () => json,
  }))
  return fn
}

describe('StreamConnection', () => {
  const originalRTCPeerConnection = globalThis.RTCPeerConnection
  const originalRTCRtpReceiver = globalThis.RTCRtpReceiver

  afterEach(() => {
    globalThis.RTCPeerConnection = originalRTCPeerConnection
    globalThis.RTCRtpReceiver = originalRTCRtpReceiver
    FakePeerConnection.instances = []
  })

  /**
   * @param {object} [options]
   * @param {RTCRtpCodecCapability[]} [options.videoCodecs] What the fake browser reports having;
   *   defaults to H.264 and AV1, matching real Chrome.
   * @param {boolean} [options.capabilitiesSupported] `false` simulates a browser (like Firefox) with
   *   no `RTCRtpReceiver.getCapabilities` at all.
   */
  function install({ videoCodecs, capabilitiesSupported = true } = {}) {
    globalThis.RTCPeerConnection = FakePeerConnection
    if (capabilitiesSupported) {
      globalThis.RTCRtpReceiver = {
        getCapabilities: (kind) => (kind === 'video' ? { codecs: videoCodecs ?? [codec('video/H264', 'packetization-mode=1;profile-level-id=64001f'), codec('video/AV1', 'profile=0')] } : { codecs: [] }),
      }
    } else {
      globalThis.RTCRtpReceiver = undefined
    }
  }

  it('opens a session and applies the host answer', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'abc123', type: 'answer', sdp: 'fake-answer-sdp' })
    const connection = new StreamConnection({ fetch })

    await connection.open({ width: 1280, height: 720 })

    expect(connection.id).toBe('abc123')
    expect(connection.pc.remoteDescription).toEqual({ type: 'answer', sdp: 'fake-answer-sdp' })

    const [url, init] = fetch.mock.calls[0]
    expect(url).toBe('/api/webrtc/session')
    expect(JSON.parse(init.body)).toEqual({ type: 'offer', sdp: 'fake-offer-sdp', width: 1280, height: 720 })
  })

  it('offers to receive both video and audio', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()

    const kinds = connection.pc.transceivers.map((t) => t.kind)
    expect(kinds).toContain('video')
    expect(kinds).toContain('audio')
    for (const transceiver of connection.pc.transceivers) {
      expect(transceiver.init.direction).toBe('recvonly')
    }
  })

  it('restricts the video offer to H.264 by default', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()

    const [videoTransceiver] = connection.pc.transceivers
    expect(videoTransceiver.setCodecPreferences).toHaveBeenCalledTimes(1)
    const [offered] = videoTransceiver.setCodecPreferences.mock.calls[0]
    expect(offered.every((c) => c.mimeType === 'video/H264')).toBe(true)
  })

  it('offers only AV1, deliberately with no H.264 fallback, when asked for AV1', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open({ codec: 'av1' })

    const [videoTransceiver] = connection.pc.transceivers
    const [offered] = videoTransceiver.setCodecPreferences.mock.calls[0]
    expect(offered.every((c) => c.mimeType === 'video/AV1')).toBe(true)
  })

  it('refuses locally, without ever contacting the host, when AV1 was requested but the browser has none', async () => {
    install({ videoCodecs: [codec('video/H264', 'packetization-mode=1;profile-level-id=64001f')] })
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })

    await expect(connection.open({ codec: 'av1' })).rejects.toThrow('AV1')
    expect(fetch).not.toHaveBeenCalled()
    expect(connection.pc).toBeNull()
    expect(FakePeerConnection.instances[0].closed).toBe(true)
  })

  it('never leaves setCodecPreferences uncalled just because the audio transceiver has no video codecs', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()

    const audioTransceiver = connection.pc.transceivers.find((t) => t.kind === 'audio')
    expect(audioTransceiver.setCodecPreferences).not.toHaveBeenCalled()
  })

  it('does not throw, and simply offers whatever the default is, when the browser has no capability query at all', async () => {
    install({ capabilitiesSupported: false })
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await expect(connection.open()).resolves.toBeUndefined()
    const [videoTransceiver] = connection.pc.transceivers
    expect(videoTransceiver.setCodecPreferences).not.toHaveBeenCalled()
  })

  it('does not throw when the browser reports capabilities but the transceiver itself cannot set preferences', async () => {
    install()
    globalThis.RTCPeerConnection = class extends FakePeerConnection {
      addTransceiver(kind, init) {
        const transceiver = super.addTransceiver(kind, init)
        delete transceiver.setCodecPreferences
        return transceiver
      }
    }
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await expect(connection.open()).resolves.toBeUndefined()
  })

  it('asks for low-latency playout on both receivers when the browser supports it', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()

    for (const transceiver of connection.pc.transceivers) {
      expect(transceiver.receiver.playoutDelayHint).toBe(0)
    }
  })

  it('does not fail when the browser has no playoutDelayHint support', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })

    // Replace addTransceiver just for this test with one whose receiver has no playoutDelayHint
    // property at all, the way a browser without that (Chrome-only) API would behave.
    const original = FakePeerConnection.prototype.addTransceiver
    FakePeerConnection.prototype.addTransceiver = function (kind, init) {
      const transceiver = { kind, init, receiver: Object.freeze({}) }
      this.transceivers.push(transceiver)
      return transceiver
    }
    try {
      await expect(connection.open()).resolves.toBeUndefined()
    } finally {
      FakePeerConnection.prototype.addTransceiver = original
    }
  })

  it('creates a separate unreliable, unordered channel for motion', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()

    expect(connection.motionChannel.label).toBe('input-motion')
    expect(connection.motionChannel.init).toEqual({ ordered: false, maxRetransmits: 0 })
  })

  it('sends on the motion channel only once it is open', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()

    connection.sendMotion('too-early')
    expect(connection.motionChannel.send).not.toHaveBeenCalled()

    connection.motionChannel.open()
    connection.sendMotion('mr')
    expect(connection.motionChannel.send).toHaveBeenCalledWith('mr')
  })

  it('sends nothing on either input channel before a connection exists', () => {
    const connection = new StreamConnection({ fetch: fakeFetch({}) })
    expect(() => connection.sendInput('x')).not.toThrow()
    expect(() => connection.sendMotion('x')).not.toThrow()
  })

  it('passes a configured STUN server to the peer connection', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch, stunServer: 'stun:example.com:19302' })
    await connection.open()
    expect(connection.pc.config.iceServers).toEqual([{ urls: 'stun:example.com:19302' }])
  })

  it('creates no ICE servers when none is configured', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()
    expect(connection.pc.config.iceServers).toEqual([])
  })

  it('reports the tracks the host sends', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    const seen = []
    connection.onTrack = (track) => seen.push(track)
    await connection.open()

    connection.pc.emitTrack({ kind: 'video' })
    connection.pc.emitTrack({ kind: 'audio' })
    expect(seen).toEqual([{ kind: 'video' }, { kind: 'audio' }])
  })

  it('reports connection state changes', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    const seen = []
    connection.onConnectionStateChange = (state) => seen.push(state)
    await connection.open()

    connection.pc.setConnectionState('connecting')
    connection.pc.setConnectionState('connected')
    expect(seen).toEqual(['connecting', 'connected'])
  })

  it('notifies when the input channel opens, and sends on it once open', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    let opened = false
    connection.onInputChannelOpen = () => {
      opened = true
    }
    await connection.open()

    connection.sendInput('too-early')
    expect(connection.inputChannel.send).not.toHaveBeenCalled()

    connection.inputChannel.open()
    expect(opened).toBe(true)

    connection.sendInput('hello')
    expect(connection.inputChannel.send).toHaveBeenCalledWith('hello')
  })

  it('rejects with the host reason when the offer is refused', async () => {
    install()
    const fetch = fakeFetch({ status: false, error: 'Browser does not support H.264 video' }, 400)
    const connection = new StreamConnection({ fetch })

    await expect(connection.open()).rejects.toThrow('Browser does not support H.264 video')
    expect(connection.pc).toBeNull()
  })

  it('closes the local connection even when a refusal carries no body', async () => {
    install()
    const fetch = vi.fn(async () => ({ ok: false, status: 500, json: async () => { throw new Error('not json') } }))
    const connection = new StreamConnection({ fetch })

    await expect(connection.open()).rejects.toThrow(/500/)
    expect(FakePeerConnection.instances[0].closed).toBe(true)
  })

  it('tells the host to close the session and closes the connection locally', async () => {
    install()
    const openFetch = fakeFetch({ status: true, id: 'to-close', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch: openFetch })
    await connection.open()

    const closeFetch = fakeFetch({ status: true })
    connection.fetch = closeFetch
    const pc = connection.pc
    await connection.close()

    expect(closeFetch).toHaveBeenCalledWith('/api/webrtc/session/to-close', { method: 'DELETE' })
    expect(pc.closed).toBe(true)
    expect(connection.pc).toBeNull()
    expect(connection.id).toBeNull()
    expect(connection.inputChannel).toBeNull()
    expect(connection.motionChannel).toBeNull()
  })

  it('closes locally even if telling the host fails', async () => {
    install()
    const openFetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch: openFetch })
    await connection.open()

    connection.fetch = vi.fn(async () => {
      throw new Error('network is gone')
    })
    const pc = connection.pc
    await expect(connection.close()).resolves.toBeUndefined()
    expect(pc.closed).toBe(true)
  })

  it('does nothing and does not throw when closed without ever having opened', async () => {
    const connection = new StreamConnection({ fetch: fakeFetch({}) })
    await expect(connection.close()).resolves.toBeUndefined()
  })

  it('reports empty stats before a connection exists', async () => {
    const connection = new StreamConnection({ fetch: fakeFetch({}) })
    expect(await connection.stats()).toEqual({ video: null, audio: null, candidateType: null })
  })

  it('reports live stats once connected', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })
    await connection.open()
    connection.pc.getStats = async () => statsReport([{ id: 'v1', type: 'inbound-rtp', kind: 'video', framesDecoded: 42 }])
    expect((await connection.stats()).video.framesDecoded).toBe(42)
  })

  it('gives up waiting for gathering after a timeout, and sends the offer anyway', async () => {
    install()
    vi.useFakeTimers()
    try {
      const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
      const connection = new StreamConnection({ fetch })

      class NeverGathers extends FakePeerConnection {
        constructor(config) {
          super(config)
          this.iceGatheringState = 'new'
          // Never dispatches icegatheringstatechange, so only the timeout can resolve this.
        }
      }
      globalThis.RTCPeerConnection = NeverGathers

      const opening = connection.open()
      await vi.advanceTimersByTimeAsync(5000)
      await opening
      expect(fetch).toHaveBeenCalledOnce()
    } finally {
      vi.useRealTimers()
    }
  })

  it('waits for ICE gathering to finish before sending the offer', async () => {
    install()
    const fetch = fakeFetch({ status: true, id: 'x', sdp: 'sdp' })
    const connection = new StreamConnection({ fetch })

    // Simulate gathering being slow: it starts incomplete, then finishes a tick later.
    const originalCreate = FakePeerConnection
    class SlowGathering extends originalCreate {
      constructor(config) {
        super(config)
        this.iceGatheringState = 'new'
        setTimeout(() => {
          this.iceGatheringState = 'complete'
          this.dispatchEvent(new Event('icegatheringstatechange'))
        }, 5)
      }
    }
    globalThis.RTCPeerConnection = SlowGathering

    await connection.open()
    expect(fetch).toHaveBeenCalledOnce()
  })
})
