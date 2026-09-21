// Scenarios shared by both test tiers.
//
// A scenario drives the browser through `window.e2e` (see client.js) and reads the host through a
// small interface: `mark()`, `waitEvent(name, since, ms)`, `countEvents(name, since)`, and `can(feature)`.
// The fake host can force a stream to die and can count sessions; a real Sunshine cannot, so scenarios
// that need those declare it in `requires`, and individual checks ask `host.can(...)`.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const inputModulePath = join(here, '..', '..', 'src_assets', 'common', 'assets', 'web', 'stream', 'input.js');

/**
 * Load the real input capture module into a page.
 *
 * The module is an ES module written for the web UI's bundler. Here it is wrapped in a function
 * instead, so it can be evaluated into any page, including a real Sunshine's web UI whose security
 * policy might not allow a module script.
 *
 * @param {import('playwright-core').Page} page The page to load it into.
 */
export async function installInput(page) {
  const source = readFileSync(inputModulePath, 'utf8');
  const names = [...source.matchAll(/^export (?:function|class|const) (\w+)/gm)].map((match) => match[1]);
  const body = source.replace(/^export /gm, '');
  await page.evaluate(`window.e2eInputModule = (() => {\n${body}\nreturn { ${names.join(', ')} };\n})()`);
}

/**
 * Decode a Moonlight input packet, reading it straight from the wire layout.
 *
 * This is written separately from the host's translator, from the protocol's documented field
 * order, so that a mistake in one is unlikely to be repeated in the other.
 *
 * @param {string} hex The packet as hexadecimal.
 * @returns {object} What the packet says.
 */
export function decodeInput(hex) {
  const b = Buffer.from(hex, 'hex');
  if (b.length < 8 || b.readUInt32BE(0) !== b.length - 4) {
    return { kind: 'malformed', hex };
  }

  const magic = b.readUInt32LE(4);
  switch (magic) {
    case 3:
    case 4:
      return { kind: 'key', down: magic === 3, vk: b.readUInt16LE(9) & 0xff, flag: b.readUInt16LE(9) >> 8, mods: b[11] };
    case 7:
      return { kind: 'move', dx: b.readInt16BE(8), dy: b.readInt16BE(10) };
    case 5:
      return { kind: 'moveTo', x: b.readInt16BE(8), y: b.readInt16BE(10), w: b.readInt16BE(14), h: b.readInt16BE(16) };
    case 8:
    case 9:
      return { kind: 'button', down: magic === 8, button: b[8] };
    case 0x0a:
      return { kind: 'scroll', amount: b.readInt16BE(8) };
    case 0x55000001:
      return { kind: 'hscroll', amount: b.readInt16BE(8) };
    case 0x0c:
      return {
        kind: 'pad',
        n: b.readUInt16LE(10),
        mask: b.readUInt16LE(12),
        buttons: b.readUInt16LE(16) | (b.readUInt16LE(30) << 16),
        lt: b[18],
        rt: b[19],
        lx: b.readInt16LE(20),
        ly: b.readInt16LE(22),
        rx: b.readInt16LE(24),
        ry: b.readInt16LE(26),
      };
    case 0x55000004:
      return { kind: 'padArrival', n: b[8], type: b[9], caps: b.readUInt16LE(10), buttons: b.readUInt32LE(12) };
    default:
      return { kind: 'unknown', magic };
  }
}

/** Drop a packet that repeats the one before it, since a mouse can report the same position twice. */
const withoutRepeatedMoves = (packets) => packets.filter((packet, i) => !(packet.kind === 'moveTo' && i > 0 && JSON.stringify(packets[i - 1]) === JSON.stringify(packet)));

/** Wait until a host has logged at least `count` input packets since `mark`, and return them decoded. */
async function waitInputs(host, mark, count, timeoutMs = 6000) {
  return (await until(async () => host.inputs(mark), (packets) => packets.length >= count, timeoutMs)).value;
}

/** Compare two values by content, for a check that shows what it actually got. */
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

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
      if (host.can('audio')) {
        // Audio is checked the same way: not just that packets arrived, but that they are Opus, carry
        // sound, and play out without gaps. The host plays a steady tone.
        const audioBefore = (await until(() => call(page, 'stats', handle), (s) => s.audioPacketsReceived >= 50, 15000)).value;
        const audioStart = Date.now();
        await sleep(2000);
        const audioAfter = await call(page, 'stats', handle);
        const audioRate = ((audioAfter.audioPacketsReceived - audioBefore.audioPacketsReceived) * 1000) / (Date.now() - audioStart);
        t.check('audio codec is Opus', audioAfter.audioCodec === 'audio/opus', audioAfter.audioCodec);
        t.check('audio arrives at about 50 packets a second', audioRate >= 40 && audioRate <= 60, `${audioRate.toFixed(1)}/s`);
        t.check('audio carries sound, not silence', audioAfter.audioEnergy > 0, `energy ${audioAfter.audioEnergy}`);
        const concealed = audioAfter.audioConcealedSamples / Math.max(1, audioAfter.audioSamplesReceived);
        t.check('audio was actually played out', audioAfter.audioSamplesReceived > 0, `${audioAfter.audioSamplesReceived} samples`);
        t.check('audio plays out with under 5% concealed gaps', concealed < 0.05, `${(concealed * 100).toFixed(2)}%`);
        t.info(`audio ${audioRate.toFixed(0)} packets/s, ${audioAfter.audioPacketsLost} lost`);
      }
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
    name: 'audio: stays in sync with the video',
    requires: ['audio'],
    async run({ page, t }) {
      const session = await call(page, 'connect', { width: 640, height: 360, fps: 30, bitrate: 2000 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const handle = session.handle;
      await until(() => call(page, 'stats', handle), (s) => s.framesDecoded >= 30 && s.audioPacketsReceived >= 50, 20000);

      // The browser reports when it expects to play each track, derived from the sender reports. If
      // audio and video were timestamped inconsistently these would drift apart or sit far apart.
      const skews = [];
      for (let i = 0; i < 10; i++) {
        await sleep(1000);
        const s = await call(page, 'stats', handle);
        if (s.videoPlayoutMs !== null && s.audioPlayoutMs !== null) {
          skews.push(s.audioPlayoutMs - s.videoPlayoutMs);
        }
      }

      t.check('browser reported playout times for both tracks', skews.length >= 6, `${skews.length} of 10 samples`);
      if (skews.length >= 6) {
        const worst = Math.max(...skews.map(Math.abs));
        const drift = Math.max(...skews) - Math.min(...skews);
        t.check('audio and video within 150 ms of each other', worst < 150, `worst ${worst.toFixed(0)} ms`);
        t.check('offset stays steady, drifting under 80 ms', drift < 80, `drift ${drift.toFixed(0)} ms`);
        t.info(`skew ${skews.map((v) => v.toFixed(0)).join(', ')} ms`);
      }
      await call(page, 'hangUp', handle);
    },
  },
  {
    name: 'audio: a session with no audio still works',
    requires: ['audio'],
    async run({ page, t }) {
      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500, audio: 'none' });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const s = (await until(() => call(page, 'stats', session.handle), (x) => x.framesDecoded >= 10, 20000)).value;
      t.check('video still flows', s.framesDecoded >= 10, `framesDecoded=${s.framesDecoded}`);
      await sleep(1000);
      t.check('no audio was sent', (await call(page, 'stats', session.handle)).audioPacketsReceived === 0);
      await call(page, 'hangUp', session.handle);
    },
  },
  {
    name: 'audio: a browser without Opus is refused cleanly',
    requires: ['audio'],
    async run({ page, host, t }) {
      const session = await call(page, 'connect', { audio: 'pcmu' });
      t.check('offer rejected', !session.ok);
      t.check('says why', /Opus/.test(session.error || ''), session.error);
      t.check('nothing left running', (await host.request({ cmd: 'count' })).count === 0);
    },
  },
  {
    name: 'input: real keyboard and mouse events reach the host as the right packets',
    requires: ['input_packets'],
    async run({ page, host, t }) {
      const session = await call(page, 'connect', { width: 640, height: 360, fps: 30, bitrate: 2000 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const handle = session.handle;
      await until(() => call(page, 'stats', handle), (s) => s.framesDecoded >= 10, 20000);
      await call(page, 'startInput', handle);

      // Keyboard: a modified key, a navigation key, and one the browser would normally act on itself.
      let mark = host.mark();
      await page.keyboard.down('ShiftLeft');
      await page.keyboard.down('KeyB');
      await page.keyboard.up('KeyB');
      await page.keyboard.up('ShiftLeft');
      await page.keyboard.press('ArrowLeft');
      await page.keyboard.press('F5');
      let got = await waitInputs(host, mark, 8);
      const key = (down, vk, mods) => ({ kind: 'key', down, vk, flag: 0x80, mods });
      t.check(
        'keys arrive in order with the right codes and modifiers',
        same(got, [key(true, 0xa0, 1), key(true, 0x42, 1), key(false, 0x42, 1), key(false, 0xa0, 0), key(true, 0x25, 0), key(false, 0x25, 0), key(true, 0x74, 0), key(false, 0x74, 0)]),
        JSON.stringify(got),
      );
      t.check('the page was not reloaded by F5', (await call(page, 'connectionState', handle)) === 'connected');

      // Mouse: the position is measured within the picture, and a click stays after the move that led to it.
      mark = host.mark();
      await page.mouse.move(340, 200);
      await page.mouse.click(340, 200);
      await page.mouse.click(100, 60, { button: 'right' });
      got = withoutRepeatedMoves(await waitInputs(host, mark, 6));
      t.check(
        'mouse moves and clicks arrive in order, measured within the picture',
        same(got, [
          { kind: 'moveTo', x: 320, y: 180, w: 640, h: 360 },
          { kind: 'button', down: true, button: 1 },
          { kind: 'button', down: false, button: 1 },
          { kind: 'moveTo', x: 80, y: 40, w: 640, h: 360 },
          { kind: 'button', down: true, button: 3 },
          { kind: 'button', down: false, button: 3 },
        ]),
        JSON.stringify(got),
      );

      // Wheel: down is negative to the host, and sideways is its own packet.
      mark = host.mark();
      await page.mouse.wheel(0, 100);
      await page.mouse.wheel(0, -200);
      await page.mouse.wheel(100, 0);
      got = await waitInputs(host, mark, 3);
      t.check('wheel events arrive as scroll packets', same(got, [{ kind: 'scroll', amount: -120 }, { kind: 'scroll', amount: 240 }, { kind: 'hscroll', amount: 120 }]), JSON.stringify(got));

      // Relative movement, as a locked pointer produces.
      mark = host.mark();
      await call(page, 'setRelative', handle, true);
      await call(page, 'moveMouseBy', handle, 7, -3);
      got = await waitInputs(host, mark, 1);
      await sleep(300);
      got = host.inputs(mark);
      t.check('relative movement arrives as a single relative packet', same(got, [{ kind: 'move', dx: 7, dy: -3 }]), JSON.stringify(got));
      await call(page, 'setRelative', handle, false);

      t.check('the connection is still healthy afterwards', await call(page, 'echo', handle, 'still-alive'));
      await call(page, 'hangUp', handle);
    },
  },
  {
    name: 'input: a controller is announced, reported, and removed',
    requires: ['input_packets'],
    async run({ page, host, t }) {
      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const handle = session.handle;
      await until(() => call(page, 'stats', handle), (s) => s.framesDecoded >= 5, 20000);
      await call(page, 'startInput', handle);

      const mark = host.mark();
      // A and d-pad up held, left trigger fully pulled, right trigger half, left stick right and up, right stick half down.
      await call(page, 'setGamepad', 0, { buttons: { 0: true, 12: true, 6: 1, 7: 0.5 }, axes: [1, -1, 0, 0.5] });
      let got = await waitInputs(host, mark, 2);
      t.check('the controller is announced before it is reported on', got[0]?.kind === 'padArrival' && got[0].n === 0, JSON.stringify(got[0]));
      t.check(
        'buttons, triggers and sticks arrive as pressed, with up positive',
        same(got[1], { kind: 'pad', n: 0, mask: 1, buttons: 0x1001, lt: 255, rt: 128, lx: 32767, ly: 32767, rx: 0, ry: -16384 }),
        JSON.stringify(got[1]),
      );

      // Releasing A is reported as a change.
      await call(page, 'setGamepad', 0, { buttons: { 12: true, 6: 1, 7: 0.5 }, axes: [1, -1, 0, 0.5] });
      got = await waitInputs(host, mark, 3);
      t.check('a released button is reported', got[2]?.kind === 'pad' && got[2].buttons === 0x0001, JSON.stringify(got[2]));

      // Nothing changing means nothing is sent.
      await sleep(500);
      t.check('an unchanged controller is not re-sent', host.inputs(mark).length === 3, `${host.inputs(mark).length} packets`);

      // Unplugging marks it as gone.
      await call(page, 'setGamepad', 0, null);
      got = await waitInputs(host, mark, 4);
      t.check('unplugging marks the controller removed', got[3]?.kind === 'pad' && got[3].n === 0 && got[3].mask === 0, JSON.stringify(got[3]));
      await call(page, 'hangUp', handle);
    },
  },
  {
    name: 'input: nothing is left stuck down, and junk is ignored',
    requires: ['input_packets'],
    async run({ page, host, t }) {
      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const handle = session.handle;
      await until(() => call(page, 'stats', handle), (s) => s.framesDecoded >= 5, 20000);
      await call(page, 'startInput', handle);
      const key = (down, vk) => ({ kind: 'key', down, vk, flag: 0x80, mods: 0 });

      // Losing focus while a key is held must release it, or it would repeat on the host forever.
      let mark = host.mark();
      await page.keyboard.down('KeyA');
      await page.evaluate(() => window.dispatchEvent(new Event('blur')));
      await page.keyboard.up('KeyA');
      let got = await waitInputs(host, mark, 2);
      await sleep(300);
      got = host.inputs(mark);
      t.check('a held key is released when focus is lost, and only once', same(got, [key(true, 0x41), key(false, 0x41)]), JSON.stringify(got));

      // Whatever a broken or hostile page sends, it must not reach the host or disturb the session.
      mark = host.mark();
      for (const junk of ['not json', '{"t":"fly"}', '{"t":"kd","k":9999}', '{"t":"mr"}', 'x'.repeat(3000)]) {
        await call(page, 'sendRawInput', handle, junk);
      }
      await page.keyboard.press('KeyC');
      got = await waitInputs(host, mark, 2);
      await sleep(300);
      got = host.inputs(mark);
      t.check('junk is dropped and the real keys still arrive', same(got, [key(true, 0x43), key(false, 0x43)]), JSON.stringify(got));
      t.check('the session survived the junk', await call(page, 'echo', handle, 'alive'));

      // Stopping releases what is held, and then the page is not listened to any more.
      mark = host.mark();
      await page.keyboard.down('KeyE');
      await waitInputs(host, mark, 1);
      await call(page, 'stopInput', handle);
      got = await waitInputs(host, mark, 2);
      t.check('stopping releases the held key', same(got, [key(true, 0x45), key(false, 0x45)]), JSON.stringify(got));

      mark = host.mark();
      await page.keyboard.press('KeyD');
      await page.mouse.move(200, 100);
      await sleep(500);
      t.check('nothing is sent once stopped', host.inputs(mark).length === 0, JSON.stringify(host.inputs(mark)));
      await call(page, 'hangUp', handle);
    },
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

