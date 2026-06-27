#!/usr/bin/env node
import { spawn, spawnSync } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-phase14-'));
const fixturesDir = path.join(testDir, 'fixtures');
const fujinetPath = process.argv[2] || process.env.FUJINET_PATH || '';

const generated = spawnSync('python3', [
  path.join(repoRoot, 'tests', 'fixtures', 'mcp_test_programs', 'generate.py'),
  fixturesDir,
], { encoding: 'utf8' });
if (generated.status !== 0) {
  throw new Error(`fixture generation failed: ${generated.stderr || generated.stdout}`);
}

const server = spawn('node', [path.join(repoRoot, 'mcp-server', 'index.js')], {
  cwd: repoRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
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

async function callTool(name, args = {}, timeoutMs = 30000) {
  const result = await request('tools/call', { name, arguments: args }, timeoutMs);
  const text = result.content?.[0]?.text || '';
  if (result.isError) throw new Error(`${name} failed: ${text}`);
  return text;
}

function parseToolJson(text) {
  const start = text.indexOf('{');
  if (start < 0) throw new Error(`tool response did not contain JSON: ${text}`);
  return JSON.parse(text.slice(start));
}

async function runUntilDebug(text, maxFrames = 180) {
  const resp = parseToolJson(await callTool('atari_run_until', {
    predicates: [{ type: 'debug_contains', text }],
    max_frames: maxFrames,
    max_ms_wallclock: 10000,
    poll_interval_frames: 5,
    include_debug_tail: 4,
  }, 30000));
  if (resp.status !== 'ok') throw new Error(`debug marker ${text} not observed: ${JSON.stringify(resp)}`);
  return resp;
}

function fixture(name) {
  return path.join(fixturesDir, name);
}

async function loadAndExpectDebug(name, marker) {
  await callTool('atari_debug_enable', { addr: 0xd7ff });
  await callTool('atari_load', { path: fixture(name) });
  await runUntilDebug(marker);
}

async function main() {
  let initialized = false;
  await request('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'mcp-phase14-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    await callTool('atari_start', { display_mode: 'headless', sound: false }, 30000);

    await loadAndExpectDebug('hello_debug.xex', 'PHASE14');
    const helloScreen = parseToolJson(await callTool('atari_screen_text'));
    if (!helloScreen.lines?.join('\n').includes('PHASE14 READY')) {
      throw new Error(`hello_debug screen marker missing: ${JSON.stringify(helloScreen)}`);
    }

    await loadAndExpectDebug('screen_text.xex', 'SCREEN14');
    const screen = parseToolJson(await callTool('atari_screen_text'));
    if (!screen.lines?.join('\n').includes('SCREEN14 READY')) {
      throw new Error(`screen_text marker missing: ${JSON.stringify(screen)}`);
    }

    await callTool('atari_debug_enable', { addr: 0xd7ff });
    await callTool('atari_load', { path: fixture('joystick_test.xex') });
    await callTool('atari_joystick', { direction: 'right', fire: true });
    await callTool('atari_run', { frames: 10 });
    const joystick = parseToolJson(await callTool('atari_peek', { address: 0x0600, length: 2 }));
    if (joystick.data?.[0] === 15 || joystick.data?.[1] !== 0) {
      throw new Error(`joystick fixture did not observe right/fire: ${JSON.stringify(joystick)}`);
    }
    await callTool('atari_joystick', { direction: 'center', fire: false });

    await loadAndExpectDebug('netstream_speed.xex', 'NETSPEED14');
    const netstream = parseToolJson(await callTool('atari_peek', { address: 0x0608, length: 3 }));
    if (JSON.stringify(netstream.data) !== JSON.stringify([0x14, 0x28, 0x40])) {
      throw new Error(`netstream fixture markers wrong: ${JSON.stringify(netstream)}`);
    }

    await callTool('atari_disk_insert', { source_path: fixture('disk_boot.atr'), drive: 1 });
    await callTool('atari_debug_enable', { addr: 0xd7ff });
    await callTool('atari_reset');
    await runUntilDebug('DISK14', 360);

    if (fujinetPath && fs.existsSync(fujinetPath)) {
      await callTool('atari_stop', { force: true });
      await callTool('fujinet_set_local_path', { path: fujinetPath });
      await callTool('fujinet_boot', {
        source_path: fixture('fujinet_boot.atr'),
        display_mode: 'headless',
        timeout_ms: 15000,
      }, 45000);
      await callTool('atari_debug_enable', { addr: 0xd7ff });
      await runUntilDebug('FUJI14', 480);
    }

    console.log('mcp_phase14_smoke: ok');
  } finally {
    if (initialized) {
      try { await callTool('atari_stop', { force: true }); } catch {}
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
