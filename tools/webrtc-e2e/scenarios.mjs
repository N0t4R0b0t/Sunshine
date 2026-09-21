// Scenarios shared by both test tiers.
//
// A scenario drives the browser through `window.e2e` (see client.js) and reads the host through a
// small interface: `mark()`, `waitEvent(name, since, ms)`, `countEvents(name, since)`, and `can(feature)`.
// The fake host can force a stream to die and can count sessions; a real Sunshine cannot, so scenarios
// that need those declare it in `requires`, and individual checks ask `host.can(...)`.

/** Wait for a while. */
export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Poll until a value satisfies a predicate.
 *
 * @param {() => Promise<any>} produce Produces the value to test.
 * @param {(value: any) => boolean} accept Whether the value is good enough.
 * @param {number} timeoutMs How long to keep trying.
 * @returns {Promise<{value: any, ok: boolean}>} The last value, and whether it was accepted.
 */
export async function until(produce, accept, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let value;
  while (Date.now() < deadline) {
    value = await produce();
    if (accept(value)) {
      return { value, ok: true };
    }
    await sleep(200);
  }
  return { value, ok: false };
}

export const call = (page, fn, ...params) => page.evaluate(([name, values]) => window.e2e[name](...values), [fn, params]);

export const scenarios = [
  {
    name: 'basic: decodes a live H.264 stream',
    async run({ page, host, t }) {
      const mark = host.mark();
      const session = await call(page, 'connect', { width: 640, height: 360, fps: 30, bitrate: 2000 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const handle = session.handle;

      const first = await until(() => call(page, 'stats', handle), (s) => s.framesDecoded >= 30, 25000);
      t.check('browser decoded 30 frames', first.ok, `framesDecoded=${first.value.framesDecoded}, state=${first.value.connectionState}`);
      if (!first.ok) {
        return;
      }

      // Measure the rate over a couple of seconds rather than trusting a single snapshot.
      const t0 = Date.now();
      await sleep(2000);
      const second = await call(page, 'stats', handle);
      const fps = ((second.framesDecoded - first.value.framesDecoded) * 1000) / (Date.now() - t0);

      t.check('picture is 640x360', second.frameWidth === 640 && second.frameHeight === 360, `${second.frameWidth}x${second.frameHeight}`);
      t.check('video element sized', second.videoWidth === 640 && second.videoHeight === 360, `${second.videoWidth}x${second.videoHeight}`);
      t.check('codec is H.264', second.codec === 'video/H264', second.codec);
      // The host prefers High, then Main, then anything else that is non-interleaved. Whatever this
      // browser offers, the agreed profile should be the best of those it offered.
      const offered = (await call(page, 'h264Profiles')).filter((f) => /packetization-mode=1/.test(f));
      const profileOf = (fmtp) => /profile-level-id=([0-9a-f]{2})/.exec(fmtp || '')?.[1];
      const rank = (profile) => ({ 64: 3, '4d': 2 })[profile] ?? 1;
      const best = offered.map(profileOf).sort((a, b) => rank(b) - rank(a))[0];
      t.check('agreed on the best profile the browser offered', profileOf(second.fmtp) === best, `agreed ${profileOf(second.fmtp)}, browser offered ${offered.map(profileOf).join('/')}`);
      t.check('decoded a keyframe', second.keyFramesDecoded >= 1, `keyFramesDecoded=${second.keyFramesDecoded}`);
      t.check('sustains at least 20 fps', fps >= 20, `${fps.toFixed(1)} fps`);
      const lossPercent = (100 * second.packetsLost) / Math.max(1, second.packetsReceived + second.packetsLost);
      t.check('under 2% packet loss on loopback', lossPercent < 2, `${lossPercent.toFixed(2)}%`);
      // Decoding frames is not enough: they could all be blank. The test pattern is colourful and
      // detailed, so a real picture varies a lot, while an empty or black screen barely does.
      const picture = await call(page, 'pictureStats', handle);
      t.check('picture shows real content, not a blank frame', picture.stddev > 25 && picture.distinctColours >= 6, `stddev ${picture.stddev.toFixed(1)}, ${picture.distinctColours} colours, mean ${picture.mean.toFixed(0)}`);
      t.check('data channel echoes', await call(page, 'echo', handle, 'ping-from-browser'));
      t.check('host started the stream', host.countEvents('stream_started', mark) === 1);
      if (host.can('keyframe_event')) {
        t.check('host was asked for a keyframe', host.countEvents('keyframe_requested', mark) >= 1);
      }
      t.info(`${fps.toFixed(1)} fps, ${second.bytesReceived} bytes, nack=${second.nackCount} pli=${second.pliCount}`);

      await call(page, 'hangUp', handle);
    },
    variants: true,
  },
  {
    name: 'lifecycle: closing the tab stops the host stream',
    async run({ page, host, t }) {
      const mark = host.mark();
      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const up = await until(() => call(page, 'stats', session.handle), (s) => s.framesDecoded >= 5, 20000);
      t.check('stream was flowing', up.ok);

      // This is what closing a tab does: the connection just ends, with no request to the host.
      await call(page, 'abandon', session.handle);
      const stopped = await host.waitEvent('stream_stopped', mark, 20000);
      t.check('host stopped the stream by itself', Boolean(stopped));
    },
  },
  {
    name: 'lifecycle: hanging up explicitly stops the host stream',
    async run({ page, host, t }) {
      const mark = host.mark();
      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      await until(() => call(page, 'stats', session.handle), (s) => s.framesDecoded >= 5, 20000);

      t.check('DELETE returns 200', (await call(page, 'hangUp', session.handle)) === 200);
      t.check('host stopped the stream', Boolean(await host.waitEvent('stream_stopped', mark, 5000)));
      if (host.can('count')) {
        t.check('no sessions remain', (await host.request({ cmd: 'count' })).count === 0);
      }
    },
  },
  {
    name: 'lifecycle: a stream that dies takes the browser connection with it',
    requires: ['die_after'],
    async run({ page, host, t }) {
      const mark = host.mark();
      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500, die_after_s: 3 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      await until(() => call(page, 'stats', session.handle), (s) => s.framesDecoded >= 5, 20000);

      t.check('host noticed the stream ended', Boolean(await host.waitEvent('stream_stopped', mark, 15000)));
      const closed = await until(() => call(page, 'connectionState', session.handle), (s) => s !== 'connected', 15000);
      t.check('browser saw the connection end', closed.ok, closed.value);
    },
  },
  {
    name: 'negotiation: a browser without H.264 is refused cleanly',
    async run({ page, host, t }) {
      const session = await call(page, 'connect', { codecs: 'vp8' });
      t.check('offer rejected', !session.ok);
      t.check('says why', /H\.264/.test(session.error || ''), session.error);
      if (host.can('count')) {
        t.check('nothing left running', (await host.request({ cmd: 'count' })).count === 0);
      }
    },
  },
  {
    name: 'limits: the fifth simultaneous session is refused',
    requires: ['count'],
    async run({ page, host, t }) {
      const handles = [];
      for (let i = 0; i < 4; i++) {
        const session = await call(page, 'connect', { width: 320, height: 240, fps: 10, bitrate: 300 });
        t.check(`session ${i + 1} accepted`, session.ok, session.error);
        if (session.ok) {
          handles.push(session.handle);
        }
      }
      const fifth = await call(page, 'connect', { width: 320, height: 240, fps: 10, bitrate: 300 });
      t.check('fifth refused', !fifth.ok);
      t.check('says why', /Too many/.test(fifth.error || ''), fifth.error);

      for (const handle of handles) {
        await call(page, 'hangUp', handle);
      }
      t.check('all closed afterwards', (await until(async () => (await host.request({ cmd: 'count' })).count, (n) => n === 0, 5000)).ok);
    },
  },
];

