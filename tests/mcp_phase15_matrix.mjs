#!/usr/bin/env node
import { spawn, spawnSync } from 'child_process';
import fs from 'fs';
import net from 'net';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-phase15-'));
const fixturesDir = path.join(testDir, 'fixtures');
const fujinetPath = process.argv[2] || process.env.FUJINET_PATH || '/home/mozzwald/fujinet-pc-ATARI';
const generated = spawnSync('python3', [
  path.join(repoRoot, 'tests', 'fixtures', 'mcp_test_programs', 'generate.py'),
  fixturesDir,
], { encoding: 'utf8' });
if (generated.status !== 0) throw new Error(`fixture generation failed: ${generated.stderr || generated.stdout}`);

const server = spawn('node', [path.join(repoRoot, 'mcp-server', 'index.js')], {
  cwd: repoRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
  env: { ...process.env, DISPLAY: '', WAYLAND_DISPLAY: '' },
});

let nextId = 1;
let stdout = '';
const pending = new Map();
server.stdout.setEncoding('utf8');
server.stdout.on('data', (chunk) => {
  stdout += chunk;
  for (;;) {
    const idx = stdout.indexOf('\n');
    if (idx < 0) break;
    const line = stdout.slice(0, idx).trim();
    stdout = stdout.slice(idx + 1);
    if (!line) continue;
    const msg = JSON.parse(line);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(JSON.stringify(msg.error)));
      else resolve(msg.result);
    }
  }
});
server.stderr.setEncoding('utf8');
server.stderr.on('data', (chunk) => process.stderr.write(chunk));

function request(method, params = {}, timeoutMs = 30000) {
  const id = nextId++;
  server.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id, method, params })}\n`);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`timeout waiting for ${method}`));
    }, timeoutMs);
    pending.set(id, {
      resolve: (value) => { clearTimeout(timer); resolve(value); },
      reject: (error) => { clearTimeout(timer); reject(error); },
    });
  });
}

function notify(method, params = {}) {
  server.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', method, params })}\n`);
}

async function callTool(name, args = {}, timeoutMs = 30000, allowError = false) {
  const result = await request('tools/call', { name, arguments: args }, timeoutMs);
  const text = result.content?.[0]?.text || '';
  if (result.isError && !allowError) throw new Error(`${name} failed: ${text}`);
  return { result, text };
}

function parseToolJson(text) {
  const start = text.indexOf('{');
  if (start < 0) throw new Error(`tool response did not contain JSON: ${text}`);
  return JSON.parse(text.slice(start));
}

function sendRaw(socketPath, body, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    const client = net.createConnection(socketPath);
    let data = Buffer.alloc(0);
    let expected = null;
    const timer = setTimeout(() => {
      client.destroy();
      reject(new Error('raw socket timeout'));
    }, timeoutMs);
    client.on('connect', () => {
      const payload = Buffer.from(body);
      client.write(`${payload.length}\n`);
      client.write(payload);
    });
    client.on('data', (chunk) => {
      data = Buffer.concat([data, chunk]);
      if (expected === null) {
        const idx = data.indexOf(10);
        if (idx >= 0) {
          expected = Number.parseInt(data.subarray(0, idx).toString(), 10);
          data = data.subarray(idx + 1);
        }
      }
      if (expected !== null && data.length >= expected) {
        clearTimeout(timer);
        client.end();
        resolve(JSON.parse(data.subarray(0, expected).toString()));
      }
    });
    client.on('error', (error) => {
      clearTimeout(timer);
      reject(error);
    });
  });
}

async function main() {
  let initialized = false;
  let external = null;
  await request('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'mcp-phase15-matrix', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    if (!fs.existsSync(fujinetPath)) {
      throw new Error(`real FujiNet-PC path is required for phase 15: ${fujinetPath}`);
    }
    const listed = await request('tools/list');
    const toolNames = new Set(listed.tools.map((tool) => tool.name));
    const readme = fs.readFileSync(path.join(repoRoot, 'mcp-server', 'README.md'), 'utf8');
    for (const line of readme.split(/\r?\n/)) {
      const match = line.match(/^\| `([^`]+)` \|/);
      if (match && (match[1].startsWith('atari_') || match[1].startsWith('fujinet_'))) {
        if (!toolNames.has(match[1])) throw new Error(`README documents missing MCP tool: ${match[1]}`);
      }
    }

    let visibleFailed = false;
    try {
      const visible = await callTool('atari_start', { display_mode: 'visible', sound: false }, 30000, true);
      visibleFailed = Boolean(visible.result.isError);
    } catch (error) {
      visibleFailed = error.message.includes('visible display requested');
    }
    if (!visibleFailed) throw new Error('visible mode unexpectedly succeeded without DISPLAY/WAYLAND_DISPLAY');

    const extDir = path.join(testDir, 'external');
    fs.mkdirSync(extDir);
    const extSock = path.join(extDir, 'ai.sock');
    external = spawn(path.join(repoRoot, 'src', 'atari800'), [
      '-ai', '-ai-socket', extSock,
      '-ai-video-push-socket', path.join(extDir, 'push.sock'),
      '-ai-video-pull-socket', path.join(extDir, 'pull.sock'),
      '-ai-artifact-dir', path.join(extDir, 'artifacts'),
      '-xl', '-nosound',
    ], {
      cwd: repoRoot,
      stdio: ['ignore', 'ignore', 'ignore'],
      env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy' },
    });

    await callTool('atari_start', { display_mode: 'headless', sound: false }, 30000);
    const status = parseToolJson((await callTool('atari_status')).text);
    const session = status.session;
    if (!session.runtime_dir || !session.emulator?.socket || !fs.existsSync(session.emulator.socket)) {
      throw new Error(`session runtime/socket missing: ${JSON.stringify(session)}`);
    }
    if (!session.xvfb?.pid) throw new Error(`headless Xvfb was not started: ${JSON.stringify(session.xvfb)}`);

    const invalid = await sendRaw(session.emulator.socket, '{not json');
    if (invalid.status !== 'error' || invalid.code !== 'BAD_JSON') throw new Error(`bad invalid JSON response: ${JSON.stringify(invalid)}`);
    const unknown = await sendRaw(session.emulator.socket, JSON.stringify({ cmd: 'does.not.exist' }));
    if (unknown.status !== 'error' || unknown.code !== 'UNKNOWN_COMMAND') throw new Error(`bad unknown command response: ${JSON.stringify(unknown)}`);

    const unsafe = parseToolJson((await callTool('atari_screenshot', { path: '/tmp/phase15-denied.png' }, 30000, true)).text);
    if (unsafe.code !== 'PATH_DENIED') throw new Error(`unsafe path was not rejected: ${JSON.stringify(unsafe)}`);
    const escapedPath = path.join(session.artifact_dir, 'phase15 "quoted".png');
    const escaped = parseToolJson((await callTool('atari_screenshot', { path: escapedPath })).text);
    if (escaped.status !== 'ok' || !escaped.path.includes('"quoted"')) throw new Error(`escaped path response invalid: ${JSON.stringify(escaped)}`);

    const run = parseToolJson((await callTool('atari_run', { frames: 2 })).text);
    if (run.frames_run !== 2) throw new Error(`run did not execute 2 frames: ${JSON.stringify(run)}`);
    const screen = (await callTool('atari_screen')).text;
    if (!screen.includes('"status": "ok"') && !screen.includes('"status":"ok"')) throw new Error(`screen response missing ok JSON: ${screen}`);
    const cap = parseToolJson((await callTool('atari_peek', { address: 0, length: 999 }, 30000, true)).text);
    if (cap.code !== 'BAD_ARGUMENT') throw new Error(`peek length cap did not reject: ${JSON.stringify(cap)}`);
    await callTool('atari_poke', { address: 0x0602, values: [1, 2, 3] });
    const peek = parseToolJson((await callTool('atari_peek', { address: 0x0602, length: 3 })).text);
    if (JSON.stringify(peek.data) !== JSON.stringify([1, 2, 3])) throw new Error(`poke/peek mismatch: ${JSON.stringify(peek)}`);

    await callTool('atari_debug_enable', { addr: 0xd7ff });
    await callTool('atari_load', { path: path.join(fixturesDir, 'hello_debug.xex') });
    await callTool('atari_run', { frames: 120 });
    const debug = parseToolJson((await callTool('atari_debug_read')).text);
    if (!debug.ascii.includes('PHASE14')) throw new Error(`debug read missing marker: ${JSON.stringify(debug)}`);

    await callTool('atari_reset');
    await callTool('atari_debug_enable', { addr: 0xd7ff });
    await callTool('atari_load', { path: path.join(fixturesDir, 'hello_debug.xex') });
    const debugWait = parseToolJson((await callTool('atari_run_until', {
      predicates: [{ type: 'debug_contains', text: 'PHASE15_SHOULD_TIMEOUT' }],
      max_frames: 1,
      max_ms_wallclock: 5000,
      poll_interval_frames: 1,
      include_debug_tail: 1,
    })).text);
    if (debugWait.status !== 'timeout' || !debugWait.diagnostics?.session) throw new Error(`run_until timeout diagnostics missing: ${JSON.stringify(debugWait)}`);
    const success = parseToolJson((await callTool('atari_run_until', {
      predicates: [{ type: 'debug_contains', text: 'PHASE14' }],
      max_frames: 180,
      max_ms_wallclock: 10000,
      poll_interval_frames: 5,
      include_debug_tail: 2,
    })).text);
    if (success.status !== 'ok') throw new Error(`run_until success failed: ${JSON.stringify(success)}`);

    const step = parseToolJson((await callTool('atari_step_instruction', { instructions: 1 })).text);
    if (step.status !== 'ok' || step.debugger?.stopped_reason !== 'instruction_limit') throw new Error(`instruction step failed: ${JSON.stringify(step)}`);
    const pc = step.cpu.pc;
    await callTool('atari_break_on_pc', { addr: pc, enabled: true });
    const hit = parseToolJson((await callTool('atari_run', { frames: 5 })).text);
    if (hit.debugger?.stopped_reason !== 'breakpoint_pc' || hit.debugger?.paused !== true) {
      throw new Error(`breakpoint did not pause via JSON: ${JSON.stringify(hit)}`);
    }

    const netsio = parseToolJson((await callTool('atari_netsio_status')).text);
    if (netsio.status !== 'ok' || netsio.compiled !== true) throw new Error(`NetSIO status unavailable: ${JSON.stringify(netsio)}`);
    const traceStatus = parseToolJson((await callTool('atari_netsio_trace_status')).text);
    if (traceStatus.status !== 'ok' || traceStatus.capacity < traceStatus.count) {
      throw new Error(`bad NetSIO trace status: ${JSON.stringify(traceStatus)}`);
    }

    await callTool('atari_stop', { force: true });
    if (fs.existsSync(session.emulator.socket)) throw new Error('atari_stop did not clean up command socket');
    if (external.exitCode !== null || external.signalCode !== null) throw new Error('atari_stop killed unrelated emulator process');

    await callTool('fujinet_set_local_path', { path: fujinetPath });
    const boot = parseToolJson((await callTool('fujinet_boot', {
      source_path: path.join(fixturesDir, 'fujinet_boot.atr'),
      display_mode: 'headless',
      timeout_ms: 15000,
    }, 45000)).text);
    if (boot.status !== 'ok') throw new Error(`real FujiNet boot failed: ${JSON.stringify(boot)}`);
    const fujinetStatus = parseToolJson((await callTool('fujinet_status')).text);
    const fujinetPort = fujinetStatus.fujinet?.udp_port;
    if (!fujinetPort || fujinetPort === 9997) throw new Error(`FujiNet sidecar did not use a non-default port: ${JSON.stringify(fujinetStatus)}`);
    const mountStatus = parseToolJson((await callTool('fujinet_mount_status')).text);
    if (!mountStatus.config_path || !mountStatus.sd_path || !mountStatus.configured?.some((mount) => mount.drive === 1)) {
      throw new Error(`managed fnconfig.ini was not generated with D1 mount: ${JSON.stringify(mountStatus)}`);
    }
    const realNetsio = parseToolJson((await callTool('atari_netsio_status')).text);
    if (realNetsio.status !== 'ok' || realNetsio.initialized !== true || realNetsio.port !== fujinetPort || realNetsio.counters?.rx_datagrams < 1) {
      throw new Error(`real NetSIO status did not show FujiNet traffic: ${JSON.stringify(realNetsio)}`);
    }
    await callTool('atari_netsio_trace_enable');
    await callTool('atari_run', { frames: 30 });
    const realTrace = parseToolJson((await callTool('atari_netsio_trace_read', { since_seq: 0, limit: 32 })).text);
    if (realTrace.status !== 'ok' || !Array.isArray(realTrace.entries) || realTrace.entries.length === 0) {
      throw new Error(`real NetSIO trace did not contain entries: ${JSON.stringify(realTrace)}`);
    }
    const realTraceStatus = parseToolJson((await callTool('atari_netsio_trace_status')).text);
    if (realTraceStatus.capacity < realTraceStatus.count || realTraceStatus.dropped < 0) {
      throw new Error(`real NetSIO trace ring accounting invalid: ${JSON.stringify(realTraceStatus)}`);
    }
    await callTool('atari_debug_enable', { addr: 0xd7ff });
    const fujiDebug = parseToolJson((await callTool('atari_run_until', {
      predicates: [{ type: 'debug_contains', text: 'FUJI14' }],
      max_frames: 480,
      max_ms_wallclock: 15000,
      poll_interval_frames: 10,
      include_fujinet_log_tail: 5,
      include_netsio_trace_tail: 5,
    }, 30000)).text);
    if (fujiDebug.status !== 'ok') throw new Error(`FujiNet boot fixture marker missing: ${JSON.stringify(fujiDebug)}`);

    console.log('mcp_phase15_matrix: ok');
  } finally {
    if (initialized) {
      try { await callTool('atari_stop', { force: true }); } catch {}
    }
    if (external && external.exitCode === null && external.signalCode === null) {
      external.kill('SIGTERM');
      await new Promise((resolve) => setTimeout(resolve, 100));
      if (external.exitCode === null && external.signalCode === null) external.kill('SIGKILL');
    }
    fs.rmSync(testDir, { recursive: true, force: true });
    server.kill('SIGTERM');
  }
}

main().catch((error) => {
  console.error(error.stack || error.message);
  server.kill('SIGTERM');
  process.exitCode = 1;
});
