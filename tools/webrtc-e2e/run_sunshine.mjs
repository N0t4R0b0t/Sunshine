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
import { call, scenarios, sleep, until } from './scenarios.mjs';

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
  for (const child of children.splice(0).reverse()) {
    try {
      child.kill('SIGTERM');
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

  const creds = spawnSync(binary, [...options, '--creds', username, password], { env, encoding: 'utf8' });
  if (creds.status !== 0) {
    console.error(`could not set credentials (exit ${creds.status})\n${creds.stdout}\n${creds.stderr}`);
    process.exit(2);
  }

  const host = new LogHost();
  const sunshine = launch(binary, options, { env, stdio: ['ignore', 'pipe', 'pipe'] });
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

  for (const scenario of scenarios) {
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
