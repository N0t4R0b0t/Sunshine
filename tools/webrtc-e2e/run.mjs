#!/usr/bin/env node
// Unattended end-to-end tests for the browser streaming client.
//
// It launches headless Chrome, streams real H.264 to it over WebRTC, and checks what the browser
// actually decodes. Nothing here needs a person, a display, or a monitor to be touched.
//
//   node run.mjs                     run everything against the fake host (no Sunshine needed)
//   node run.mjs --only=basic        run scenarios whose name contains "basic"
//   node run.mjs --chrome=/path      use a specific browser
//   node run.mjs --host=/path        use a specific host binary (see build.sh, MEDIA_SRC)
//   node run.mjs --verbose           show the host's log output
//
// Exit status is 0 only if every check passed.

import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';
import { call, decodeInput, installInput, scenarios, sleep, until } from './scenarios.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const buildDir = join(here, 'build');
const args = Object.fromEntries(process.argv.slice(2).filter((a) => a.startsWith('--')).map((a) => {
  const [key, value = 'true'] = a.slice(2).split('=');
  return [key, value];
}));

/**
 * The fake host: Sunshine's real browser session layer, streaming an ffmpeg test pattern.
 */
class FakeHost {
  constructor() {
    this.events = [];
    this.pending = new Map();
    this.nextReq = 1;
  }

  async start() {
    const binary = args.host || join(buildDir, 'e2e_host');
    if (!existsSync(binary)) {
      const built = spawnSync(join(here, 'build.sh'), { stdio: 'inherit' });
      if (built.status !== 0) {
        throw new Error('could not build the end-to-end host');
      }
    }

    this.process = spawn(binary, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    process.once('exit', () => this.process.kill());
    this.exit = null;
    this.process.once('exit', (code, signal) => {
      // A dead host must never leave a caller waiting for a reply that cannot come.
      this.exit = { code, signal };
      for (const resolve of this.pending.values()) {
        resolve({ ok: false, error: `host process exited (${signal || `code ${code}`})` });
      }
      this.pending.clear();
    });
    createInterface({ input: this.process.stdout }).on('line', (line) => {
      const reply = JSON.parse(line);
      this.pending.get(reply.req)?.(reply);
      this.pending.delete(reply.req);
    });
    createInterface({ input: this.process.stderr }).on('line', (line) => {
      const match = /^EVENT (\S+) ?(.*)$/.exec(line);
      if (match) {
        this.events.push({ time: Date.now(), name: match[1], detail: match[2] });
      } else if (args.verbose) {
        console.error(`  host: ${line}`);
      }
    });

    await this.waitEvent('ready', 0, 10000);
  }

  /** Send one request line and wait for its reply. */
  request(command, timeoutMs = 30000) {
    if (this.exit) {
      return Promise.resolve({ ok: false, error: `host process exited (${this.exit.signal || `code ${this.exit.code}`})` });
    }
    const req = this.nextReq++;
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(req);
        resolve({ ok: false, error: `host did not reply within ${timeoutMs}ms` });
      }, timeoutMs);
      this.pending.set(req, (reply) => {
        clearTimeout(timer);
        resolve(reply);
      });
      this.process.stdin.write(`${JSON.stringify({ ...command, req })}\n`);
    });
  }

  /** What this host can do, so scenarios only rely on what is available. */
  can(feature) {
    return ['count', 'die_after', 'keyframe_event', 'audio', 'input_packets'].includes(feature);
  }

  /** The input packets the host received since `since`, decoded. */
  inputs(since) {
    return this.events
      .slice(since)
      .filter((event) => event.name === 'input')
      .map((event) => decodeInput(event.detail));
  }

  /** Index to pass as `since`, so a scenario only sees events from after it started. */
  mark() {
    return this.events.length;
  }

  /** Wait for a named event that happened after `since`. */
  async waitEvent(name, since, timeoutMs) {
    const { value } = await until(async () => this.events.slice(since).find((e) => e.name === name), Boolean, timeoutMs);
    return value;
  }

  countEvents(name, since) {
    return this.events.slice(since).filter((e) => e.name === name).length;
  }

  async stop() {
    if (this.exit) {
      return;
    }
    this.process.stdin.end();
    await Promise.race([new Promise((resolve) => this.process.once('exit', resolve)), sleep(5000)]);
    this.process.kill();
  }
}

/**
 * Serve the test page, and forward signaling to the host in the shape Sunshine's real endpoint uses.
 */
function startServer(host) {
  const page = readFileSync(join(here, 'page.html'));
  const client = readFileSync(join(here, 'client.js'));

  const server = createServer(async (req, res) => {
    const send = (status, body, type = 'application/json') => {
      res.writeHead(status, { 'content-type': type });
      res.end(typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body));
    };

    if (req.method === 'GET' && req.url === '/') {
      return send(200, page, 'text/html');
    }
    if (req.method === 'GET' && req.url === '/client.js') {
      return send(200, client, 'text/javascript');
    }

    if (req.method === 'POST' && req.url === '/api/webrtc/session') {
      const chunks = [];
      for await (const chunk of req) {
        chunks.push(chunk);
      }
      const body = JSON.parse(Buffer.concat(chunks).toString());
      const reply = await host.request({ cmd: 'offer', ...body, stun: '' });
      return reply.ok ?
        send(200, { status: true, id: reply.id, type: 'answer', sdp: reply.sdp }) :
        send(400, { status: false, error: reply.error });
    }

    const close = /^\/api\/webrtc\/session\/([a-z0-9]{16})$/.exec(req.url);
    if (req.method === 'DELETE' && close) {
      const reply = await host.request({ cmd: 'close', id: close[1] });
      return reply.ok ? send(200, { status: true }) : send(404, { status: false, error: 'No such browser session' });
    }

    return send(404, { status: false });
  });

  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

/** Find a browser that can decode H.264. Playwright's bundled Chromium cannot, so it is not used. */
function findChrome() {
  const candidates = [args.chrome, process.env.CHROME_PATH, '/usr/bin/google-chrome-stable', '/usr/bin/google-chrome', '/usr/bin/chromium'];
  return candidates.find((path) => path && existsSync(path));
}

// ---------------------------------------------------------------------------------------------
// Scenarios. Each gets a fresh page and a `t` for recording checks.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------

/** Chrome configurations worth running. Real users have mDNS candidate hiding on; some setups do not. */
const browserVariants = [
  { name: 'default', flags: [] },
  { name: 'no-mdns', flags: ['--disable-features=WebRtcHideLocalIpsWithMdns'] },
];

async function main() {
  const chrome = findChrome();
  if (!chrome) {
    console.error('No Chrome or Chromium found. Set CHROME_PATH.');
    process.exit(2);
  }

  const host = new FakeHost();
  await host.start();
  const server = await startServer(host);
  const baseUrl = `http://127.0.0.1:${server.address().port}/`;

  const results = [];
  let failures = 0;

  for (const variant of browserVariants) {
    const browser = await chromium.launch({
      executablePath: chrome,
      headless: true,
      args: ['--autoplay-policy=no-user-gesture-required', ...variant.flags],
    });

    // Fail early and clearly if this browser cannot decode H.264 at all.
    {
      const page = await (await browser.newContext()).newPage();
      await page.goto(baseUrl);
      const codecs = await call(page, 'receiverCodecs');
      if (!codecs.includes('video/H264')) {
        console.error(`\nThis browser cannot receive H.264 (${chrome}); its codecs are: ${[...new Set(codecs)].join(', ')}`);
        await browser.close();
        process.exit(2);
      }
      await page.close();
    }

    for (const scenario of scenarios) {
      if (scenario.variants !== true && variant !== browserVariants[0]) {
        continue;
      }
      if ((scenario.requires || []).some((feature) => !host.can(feature))) {
        continue;
      }
      if (args.only && !scenario.name.includes(args.only)) {
        continue;
      }

      const label = scenario.variants === true ? `${scenario.name} [${variant.name}]` : scenario.name;
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

      const context = await browser.newContext();
      const page = await context.newPage();
      page.on('pageerror', (error) => t.check('no page errors', false, error.message));
      await page.goto(baseUrl);
      await installInput(page);

      const started = Date.now();
      try {
        await Promise.race([
          scenario.run({ page, host, t }),
          sleep(90000).then(() => {
            throw new Error('scenario timed out after 90s');
          }),
        ]);
      } catch (error) {
        t.check('scenario completed', false, error.message);
      }
      await context.close();

      // Give the host a moment to settle so one scenario's leftovers do not leak into the next.
      await host.request({ cmd: 'count' });
      t.check('host process survived the scenario', !host.exit, host.exit ? `exited with ${host.exit.signal || `code ${host.exit.code}`}` : '');

      const failed = checks.filter((c) => !c.ok);
      failures += failed.length;
      results.push({ scenario: label, seconds: (Date.now() - started) / 1000, checks, notes });

      console.log(`${failed.length === 0 ? 'PASS' : 'FAIL'}  ${label}  (${((Date.now() - started) / 1000).toFixed(1)}s)`);
      for (const note of notes) {
        console.log(`        ${note}`);
      }
      for (const check of failed) {
        console.log(`      x ${check.name}${check.detail ? ` - ${check.detail}` : ''}`);
      }

      if (host.exit) {
        console.log('\nThe host process died, so the remaining scenarios cannot run.');
        break;
      }
    }

    if (host.exit) {
      await browser.close();
      break;
    }

    await browser.close();
  }

  await host.stop();
  server.close();

  mkdirSync(buildDir, { recursive: true });
  writeFileSync(join(buildDir, 'report.json'), JSON.stringify({ chrome, results }, null, 2));

  const total = results.reduce((n, r) => n + r.checks.length, 0);
  console.log(`\n${total - failures}/${total} checks passed. Report: ${join(buildDir, 'report.json')}`);
  process.exit(failures === 0 ? 0 : 1);
}

// Nothing may hang forever: a CI job or an unattended run must always come back.
const watchdog = setTimeout(() => {
  console.error('\nThe whole run exceeded 10 minutes; giving up.');
  process.exit(3);
}, 10 * 60 * 1000);
watchdog.unref();

main().catch((error) => {
  console.error(error);
  process.exit(2);
});
