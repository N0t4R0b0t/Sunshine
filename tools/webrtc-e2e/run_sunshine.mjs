#!/usr/bin/env node
// Unattended end-to-end tests against a real Sunshine build.
//
// Where run.mjs uses a fake host, this starts the actual `sunshine` binary on a private virtual display
// and streams that display to headless Chrome. It therefore exercises what the fake host cannot: the
// real capture, the real encoder, display preparation, and the media tap in stream.cpp.
//
//   node run_sunshine.mjs                          uses ../../cmake-build-webrtc/sunshine
//   node run_sunshine.mjs --sunshine=/path/binary
//   node run_sunshine.mjs --keep                   keep the temporary directory and print its path
//   node run_sunshine.mjs --verbose                echo Sunshine's log
//   node run_sunshine.mjs --no-content             leave the display empty (the picture check should then fail)
//
// Nothing here touches the machine's real desktop, audio, or input devices. Sunshine gets:
//   - its own X server (Xvfb), with the Wayland and D-Bus environment removed, so it cannot reach KWin;
//   - a private home and runtime directory, so it finds none of the user's settings or session bus;
//   - a PulseAudio address that goes nowhere, and audio streaming turned off;
//   - ports chosen at random, so it cannot collide with a Sunshine already running.

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';
import { call, installInput, scenarios, sleep, until } from './scenarios.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const args = Object.fromEntries(process.argv.slice(2).filter((a) => a.startsWith('--')).map((a) => {
  const [key, value = 'true'] = a.slice(2).split('=');
  return [key, value];
}));

const username = 'e2e';
const password = 'e2e-password';
const displaySize = { width: 1280, height: 720 };

const children = [];
let workDir = null;

/** Stop everything this run started, whatever way it ends. */
function cleanup() {
  // Everything here is a test process on a private display, so there is no reason to be polite. A polite
  // signal is also not enough: Sunshine can ignore it while it is busy shutting down.
  for (const child of children.splice(0).reverse()) {
    try {
      child.kill('SIGKILL');
    } catch (_) {
      // Already gone.
    }
  }
  if (workDir && !args.keep) {
    rmSync(workDir, { recursive: true, force: true });
  }
}
process.on('exit', cleanup);
for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => process.exit(130));
}

/** Start a child process and remember it for cleanup. */
function launch(command, commandArgs, options) {
  const child = spawn(command, commandArgs, options);
  children.push(child);
  return child;
}

/** Start a private X server and return its display name. */
async function startXvfb() {
  const child = launch('Xvfb', ['-displayfd', '3', '-screen', '0', `${displaySize.width}x${displaySize.height}x24`, '-nolisten', 'tcp'], {
    stdio: ['ignore', 'ignore', 'inherit', 'pipe'],
  });
  const display = await new Promise((resolve, reject) => {
    child.stdio[3].once('data', (data) => resolve(`:${data.toString().trim()}`));
    child.once('exit', () => reject(new Error('Xvfb exited before it was ready')));
    setTimeout(() => reject(new Error('Xvfb did not report a display in time')), 10000);
  });
  return display;
}

/**
 * The environment Sunshine runs in. It is built from scratch rather than inherited, so nothing from
 * the real session (Wayland, D-Bus, PulseAudio, the user's home) can leak in.
 */
function sunshineEnvironment(display) {
  const home = join(workDir, 'home');
  const runtime = join(workDir, 'runtime');
  mkdirSync(home, { recursive: true });
  mkdirSync(runtime, { recursive: true, mode: 0o700 });
  return {
    PATH: process.env.PATH,
    LD_LIBRARY_PATH: process.env.LD_LIBRARY_PATH || '',
    HOME: home,
    XDG_CONFIG_HOME: join(home, '.config'),
    XDG_RUNTIME_DIR: runtime,
    XDG_SESSION_TYPE: 'x11',
    DISPLAY: display,
    PULSE_SERVER: 'unix:/nonexistent',
  };
}

/** Turn Sunshine's log lines into the events scenarios wait for. */
class LogHost {
  constructor() {
    this.events = [];
    this.lines = [];
  }

  attach(stream) {
    createInterface({ input: stream }).on('line', (line) => {
      this.lines.push(line);
      if (args.verbose) {
        console.error(`  sunshine: ${line}`);
      }
      const events = [
        [/WebRTC: accepted browser session/, 'session_accepted'],
        [/Start capturing Video/, 'stream_started'],
        [/Session ended/, 'stream_stopped'],
        [/WebRTC: closed browser session/, 'session_closed'],
      ];
      for (const [pattern, name] of events) {
        if (pattern.test(line)) {
          this.events.push({ time: Date.now(), name, detail: line });
        }
      }
    });
  }

  /** A real Sunshine offers no way to count sessions, force a stream to die, or report keyframe requests. */
  can() {
    return false;
  }

  /** Position in the log, to pass to textSince() so a scenario only reads what happened after it began. */
  lineMark() {
    return this.lines.length;
  }

  /** The log since a mark. Sunshine writes one record across several lines, so records are matched on this text. */
  textSince(mark) {
    return this.lines.slice(mark).join('\n');
  }

  mark() {
    return this.events.length;
  }

  async waitEvent(name, since, timeoutMs) {
    const { value } = await until(async () => this.events.slice(since).find((e) => e.name === name), Boolean, timeoutMs);
    return value;
  }

  countEvents(name, since) {
    return this.events.slice(since).filter((e) => e.name === name).length;
  }

  request() {
    return Promise.resolve({ ok: false, error: 'not supported by a real Sunshine' });
  }
}

/** Ask the OS for a free base port. Sunshine also uses a handful of ports above and below it. */
function randomBasePort() {
  return 30000 + Math.floor(Math.random() * 20000);
}

/**
 * Scenarios only this tier can run, because they read Sunshine's own log to see what it made of
 * the browser's packets. Sunshine decodes them with its own code, so this is a check that does not
 * share any code with the translator that built them.
 */
const sunshineScenarios = [
  {
    name: "input: Sunshine's own parser reads the browser's packets the way they were meant",
    async run({ page, host, t }) {
      const session = await call(page, 'connect', { width: 640, height: 360, fps: 30, bitrate: 2000 });
      t.check('session accepted', session.ok, session.error);
      if (!session.ok) {
        return;
      }
      const handle = session.handle;
      await until(() => call(page, 'stats', handle), (s) => s.framesDecoded >= 10, 20000);
      await call(page, 'startInput', handle);

      const mark = host.lineMark();
      await page.keyboard.press('KeyA');
      await call(page, 'setRelative', handle, true);
      await call(page, 'moveMouseBy', handle, 7, -3);
      await call(page, 'setRelative', handle, false);
      await page.mouse.move(100, 100);
      await page.mouse.down();
      await page.mouse.up();
      await page.mouse.wheel(0, 100);

      // Wait for the last of them to be logged.
      await until(async () => host.textSince(mark), (text) => /--begin mouse scroll packet--/.test(text), 8000);
      const log = host.textSince(mark);

      const keys = [...log.matchAll(/--begin keyboard packet--\s+keyAction \[(\w+)\]\s+keyCode \[(\w+)\]/g)];
      // Action 3 is a press and 4 a release. The code is the Windows virtual key for A, 0x41, with the
      // high bit Moonlight always sets.
      t.check(
        'Sunshine read the key as A, pressed and then released',
        keys.length === 2 && keys[0][1] === '00000003' && keys[0][2] === '8041' && keys[1][1] === '00000004' && keys[1][2] === '8041',
        JSON.stringify(keys.map((k) => k.slice(1))),
      );

      const moves = [...log.matchAll(/--begin relative mouse move packet--\s+deltaX \[(-?\d+)\]\s+deltaY \[(-?\d+)\]/g)];
      t.check('Sunshine read the relative move as 7 across and 3 up', moves.length === 1 && moves[0][1] === '7' && moves[0][2] === '-3', JSON.stringify(moves.map((m) => m.slice(1))));

      const buttons = [...log.matchAll(/--begin mouse button packet--\s+action \[(\w+)\]\s+button \[(\w+)\]/g)];
      t.check('Sunshine parsed a button press and release of the left button', buttons.length === 2 && buttons.every((b) => Number.parseInt(b[2], 16) === 1), JSON.stringify(buttons.map((b) => b.slice(1))));

      // One notch down is 100 pixels in the browser, which is -120 to the host.
      const scrolls = [...log.matchAll(/--begin mouse scroll packet--\s+scrollAmt1 \[(-?\d+)\]/g)];
      t.check('Sunshine read the scroll as one notch down', scrolls.length === 1 && scrolls[0][1] === '-120', JSON.stringify(scrolls.map((m) => m[1])));

      // The host logs this line whenever it refuses something the browser sent.
      t.check('nothing the browser sent was refused', !/WebRTC: ignoring browser input/.test(log), (log.match(/.*WebRTC: ignoring browser input.*/) || [''])[0]);

      await call(page, 'hangUp', handle);
    },
  },
];

async function main() {
  const binary = args.sunshine || join(here, '..', '..', 'cmake-build-webrtc', 'sunshine');
  if (!existsSync(binary)) {
    console.error(`No Sunshine binary at ${binary}. Build it, or pass --sunshine=<path>.`);
    process.exit(2);
  }
  if (spawnSync('grep', ['-q', 'api/webrtc/session', binary]).status !== 0) {
    console.error(`${binary} has no browser streaming support; it was built before that feature.`);
    process.exit(2);
  }
  const chrome = [args.chrome, process.env.CHROME_PATH, '/usr/bin/google-chrome-stable', '/usr/bin/google-chrome', '/usr/bin/chromium'].find((p) => p && existsSync(p));
  if (!chrome) {
    console.error('No Chrome or Chromium found. Set CHROME_PATH.');
    process.exit(2);
  }

  workDir = mkdtempSync(join(tmpdir(), 'sunshine-e2e-'));
  const basePort = randomBasePort();
  const webPort = basePort + 1;

  const display = await startXvfb();
  const env = sunshineEnvironment(display);
  console.log(`virtual display ${display}, web UI on port ${webPort}, working in ${workDir}`);

  // Something has to be on screen, or the capture has nothing to show. `--no-content` leaves the
  // desktop empty on purpose, to prove the picture check fails when it should.
  if (!args['no-content']) {
    launch('ffplay', ['-loglevel', 'quiet', '-f', 'lavfi', '-i', `testsrc2=size=${displaySize.width}x${displaySize.height}:rate=30`, '-noborder', '-x', `${displaySize.width}`, '-y', `${displaySize.height}`, '-left', '0', '-top', '0'], {
      env: { ...env },
      stdio: 'ignore',
    });
  }

  const options = [
    `port=${basePort}`,
    'webrtc_enabled=enabled',
    'stream_audio=disabled',
    'capture=x11',
    'encoder=software',
    'sw_preset=ultrafast',
    'sw_tune=zerolatency',
    'min_log_level=debug',
    'upnp=disabled',
    'system_tray=disabled',
    'notify_pre_releases=disabled',
    'origin_web_ui_allowed=pc',
    'gamepad=disabled',
    'controller=disabled',
    'keyboard=disabled',
    'mouse=disabled',
    `sunshine_name=e2e`,
    `file_state=${join(workDir, 'state.json')}`,
    `credentials_file=${join(workDir, 'state.json')}`,
    `file_apps=${join(workDir, 'apps.json')}`,
    `pkey=${join(workDir, 'key.pem')}`,
    `cert=${join(workDir, 'cert.pem')}`,
    `log_path=${join(workDir, 'sunshine.log')}`,
  ];
  writeFileSync(join(workDir, 'apps.json'), JSON.stringify({ env: {}, apps: [] }));

  // GET /api/config reads config::sunshine.config_file straight off disk (see confighttp::getConfig),
  // not the in-memory settings these key=value CLI arguments already apply, so it would otherwise
  // report every one of them as absent - which the real Stream.vue page reads as "feature disabled".
  // Writing the same values into a real config file, the way a user's own config always exists, keeps
  // that endpoint honest without duplicating the meaning of any setting here.
  const configFile = join(workDir, 'sunshine.conf');
  writeFileSync(configFile, options.map((option) => option.replace('=', ' = ')).join('\n') + '\n');

  const creds = spawnSync(binary, [configFile, ...options, '--creds', username, password], { env, encoding: 'utf8' });
  if (creds.status !== 0) {
    console.error(`could not set credentials (exit ${creds.status})\n${creds.stdout}\n${creds.stderr}`);
    process.exit(2);
  }

  const host = new LogHost();
  const sunshine = launch(binary, [configFile, ...options], { env, stdio: ['ignore', 'pipe', 'pipe'] });
  host.attach(sunshine.stdout);
  host.attach(sunshine.stderr);
  let exited = null;
  sunshine.once('exit', (code, signal) => {
    exited = signal || `code ${code}`;
  });

  const ready = await until(async () => host.lines.some((l) => /Configuration UI available/.test(l)) || exited, Boolean, 60000);
  if (exited || !ready.ok) {
    console.error(`Sunshine ${exited ? `exited (${exited})` : 'did not become ready'}. Last log lines:\n${host.lines.slice(-25).join('\n')}`);
    process.exit(2);
  }

  const baseUrl = `https://127.0.0.1:${webPort}/`;
  const clientSource = readFileSync(join(here, 'client.js'), 'utf8');

  const browser = await chromium.launch({
    executablePath: chrome,
    headless: true,
    args: ['--autoplay-policy=no-user-gesture-required', '--ignore-certificate-errors'],
  });

  let failures = 0;
  const results = [];

  for (const scenario of [...scenarios, ...sunshineScenarios]) {
    if ((scenario.requires || []).length > 0) {
      continue;
    }
    if (args.only && !scenario.name.includes(args.only)) {
      continue;
    }

    const checks = [];
    const notes = [];
    const t = {
      check(name, ok, detail) {
        checks.push({ name, ok: Boolean(ok), detail: detail === undefined ? '' : String(detail) });
      },
      info(text) {
        notes.push(text);
      },
    };

    const context = await browser.newContext({ ignoreHTTPSErrors: true, httpCredentials: { username, password } });
    const page = await context.newPage();
    page.on('pageerror', (error) => t.check('no page errors', false, error.message));

    const started = Date.now();
    try {
      await page.goto(baseUrl);
      // Evaluated rather than injected as a script tag, so a page Content-Security-Policy cannot block it.
      await page.evaluate(clientSource);
      await installInput(page);

      await Promise.race([
        scenario.run({ page, host, t }),
        sleep(120000).then(() => {
          throw new Error('scenario timed out after 120s');
        }),
      ]);
    } catch (error) {
      t.check('scenario completed', false, error.message);
    }
    await context.close();

    t.check('Sunshine survived the scenario', exited === null, exited || '');

    const failed = checks.filter((c) => !c.ok);
    failures += failed.length;
    results.push({ scenario: scenario.name, seconds: (Date.now() - started) / 1000, checks, notes });

    console.log(`${failed.length === 0 ? 'PASS' : 'FAIL'}  ${scenario.name}  (${((Date.now() - started) / 1000).toFixed(1)}s)`);
    for (const note of notes) {
      console.log(`        ${note}`);
    }
    for (const check of failed) {
      console.log(`      x ${check.name}${check.detail ? ` - ${check.detail}` : ''}`);
    }

    if (exited) {
      console.log('\nSunshine died, so the remaining scenarios cannot run.');
      break;
    }
  }

  // Exercise the real page that ships in the product (Stream.vue), not the synthetic client the
  // scenarios above use, so a mistake in the page itself - not just in the modules it is built from -
  // would be caught.
  if (exited === null) {
    const checks = [];
    const started = Date.now();
    const context = await browser.newContext({ ignoreHTTPSErrors: true, httpCredentials: { username, password } });
    const page = await context.newPage();
    page.on('pageerror', (err) => checks.push({ name: 'the real page had no script errors', ok: false, detail: err.message }));
    try {
      await page.goto(`${baseUrl}stream`);

      const connectButton = page.getByRole('button', { name: 'Connect' });
      const formVisible = await connectButton.isVisible({ timeout: 10000 }).catch(() => false);
      checks.push({ name: 'the real streaming page loaded with the feature enabled', ok: formVisible });

      if (formVisible) {
        await connectButton.click();

        const decoding = await page
          .waitForFunction(
            () => {
              const el = document.querySelector('video.stream-video');
              return el && el.videoWidth > 0;
            },
            { timeout: 20000 },
          )
          .then(() => true)
          .catch(() => false);
        checks.push({ name: 'the real page decodes real video', ok: decoding });

        if (decoding) {
          const mark = host.lineMark();
          await page.click('.stream-overlay-click');

          // Headless Chrome has no real pointing device, so real Pointer Lock free-runs with jitter
          // once engaged, flooding the ordered data channel and starving out the key packets below.
          // Releasing it, exactly as a real Escape press would, is what a genuine session recovers
          // with, and clicking to control already proved the pointer lock call path did not throw.
          await page.evaluate(() => document.exitPointerLock?.());
          await sleep(200);

          await page.keyboard.press('KeyA');

          // 0x8041 is the A key with Moonlight's high bit set, read from Sunshine's own log, the same
          // way the synthetic-page input scenario above verifies it.
          const sawKey = await until(async () => /keyCode \[8041\]/.test(host.textSince(mark)), Boolean, 5000);
          checks.push({ name: 'a real key press from the real page reaches Sunshine', ok: sawKey.ok });

          await page.getByTitle('Disconnect').click();
          const sawClose = await until(async () => /WebRTC: closed browser session/.test(host.textSince(mark)), Boolean, 5000);
          checks.push({ name: 'disconnecting from the real page ends the session', ok: sawClose.ok });
        }
      }
    } catch (error) {
      checks.push({ name: 'real page scenario completed', ok: false, detail: error.message });
    }
    await context.close();

    const failed = checks.filter((c) => !c.ok);
    failures += failed.length;
    results.push({ scenario: 'the real Stream.vue page', seconds: (Date.now() - started) / 1000, checks, notes: [] });
    console.log(`${failed.length === 0 ? 'PASS' : 'FAIL'}  the real Stream.vue page  (${((Date.now() - started) / 1000).toFixed(1)}s)`);
    for (const check of failed) {
      console.log(`      x ${check.name}${check.detail ? ` - ${check.detail}` : ''}`);
    }
  }

  // The last thing to check is shutting down. A host that hangs on `systemctl stop` whenever someone is
  // streaming would be a real defect, so stop Sunshine while a browser is still connected and watch how
  // long it takes. The display stays up until it has gone, which is how a real stop happens.
  if (exited === null) {
    const checks = [];
    const context = await browser.newContext({ ignoreHTTPSErrors: true, httpCredentials: { username, password } });
    const page = await context.newPage();
    try {
      await page.goto(baseUrl);
      await page.evaluate(clientSource);
      await installInput(page);

      const session = await call(page, 'connect', { width: 320, height: 240, fps: 15, bitrate: 500 });
      checks.push({ name: 'a session was streaming when Sunshine was stopped', ok: session.ok, detail: session.error || '' });
      if (session.ok) {
        await until(() => call(page, 'stats', session.handle), (s) => s.framesDecoded >= 5, 20000);
        const stopped = Date.now();
        sunshine.kill('SIGTERM');
        const gone = await until(async () => exited !== null, Boolean, 20000);
        const seconds = (Date.now() - stopped) / 1000;
        checks.push({ name: 'Sunshine exits within 15 seconds while a browser is streaming', ok: gone.ok && seconds < 15, detail: gone.ok ? `${seconds.toFixed(1)} s` : 'still running after 20 s' });
      }
    } catch (error) {
      checks.push({ name: 'shutdown scenario completed', ok: false, detail: error.message });
    }
    await context.close();

    const failed = checks.filter((c) => !c.ok);
    failures += failed.length;
    results.push({ scenario: 'shutdown: Sunshine stops promptly with a browser connected', seconds: 0, checks, notes: [] });
    console.log(`${failed.length === 0 ? 'PASS' : 'FAIL'}  shutdown: Sunshine stops promptly with a browser connected`);
    for (const check of failed) {
      console.log(`      x ${check.name}${check.detail ? ` - ${check.detail}` : ''}`);
    }
  }

  await browser.close();

  // Proof the real path was used: the software encoder was created, and the tap delivered frames.
  const encoderLine = host.lines.find((l) => /Creating encoder/.test(l));
  console.log(`\nencoder: ${encoderLine ? encoderLine.replace(/^.*Creating encoder/, 'Creating encoder') : 'no encoder was created'}`);
  const errors = host.lines.filter((l) => /\bError:|\bFatal:/.test(l));
  console.log(`Sunshine logged ${errors.length} error line(s)${errors.length ? ':' : ''}`);
  for (const line of errors.slice(0, 8)) {
    console.log(`      ${line}`);
  }

  mkdirSync(join(here, 'build'), { recursive: true });
  writeFileSync(join(here, 'build', 'report-sunshine.json'), JSON.stringify({ chrome, binary, encoder: encoderLine, errors, results }, null, 2));
  if (args.keep) {
    console.log(`kept ${workDir}`);
  }

  const total = results.reduce((n, r) => n + r.checks.length, 0);
  console.log(`\n${total - failures}/${total} checks passed.`);
  process.exit(failures === 0 ? 0 : 1);
}

const watchdog = setTimeout(() => {
  console.error('\nThe whole run exceeded 10 minutes; giving up.');
  process.exit(3);
}, 10 * 60 * 1000);
watchdog.unref();

main().catch((error) => {
  console.error(error);
  process.exit(2);
});
