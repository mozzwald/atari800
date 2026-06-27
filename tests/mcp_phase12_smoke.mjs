#!/usr/bin/env node
import { spawn } from 'child_process';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
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

async function main() {
  let initialized = false;
  await request('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'mcp-phase12-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    const listed = await request('tools/list');
    const names = new Set(listed.tools.map((tool) => tool.name));
    for (const name of [
      'atari_screen_text',
      'atari_framebuffer_raw',
      'atari_key_down',
      'atari_key_up',
      'atari_press_key',
      'atari_type_text',
      'atari_press_console',
      'atari_input_status',
    ]) {
      if (!names.has(name)) throw new Error(`missing MCP tool: ${name}`);
    }

    await callTool('atari_start', { display_mode: 'headless', sound: false }, 30000);

    const text = parseToolJson(await callTool('atari_screen_text'));
    if (text.status !== 'ok' || typeof text.supported !== 'boolean' || !Array.isArray(text.lines)) {
      throw new Error(`bad screen text response: ${JSON.stringify(text)}`);
    }
    if (!Number.isFinite(text.confidence) || text.width <= 0 || text.height <= 0) {
      throw new Error(`screen text metadata missing: ${JSON.stringify(text)}`);
    }

    const raw = parseToolJson(await callTool('atari_framebuffer_raw'));
    if (raw.status !== 'ok' || raw.width <= 0 || raw.height <= 0 || typeof raw.data !== 'string' || raw.data.length === 0) {
      throw new Error(`bad framebuffer response: ${JSON.stringify({ ...raw, data: typeof raw.data })}`);
    }

    await callTool('atari_key_down', { key: 'a' });
    const downStatus = parseToolJson(await callTool('atari_input_status'));
    if (downStatus.key?.code !== 63) throw new Error(`key down was not visible: ${JSON.stringify(downStatus)}`);
    await callTool('atari_key_up');
    const upStatus = parseToolJson(await callTool('atari_input_status'));
    if (upStatus.key?.code !== -1) throw new Error(`key up was not visible: ${JSON.stringify(upStatus)}`);

    const pressed = parseToolJson(await callTool('atari_press_key', { key: 'b', frames: 1 }));
    if (pressed.status !== 'ok' || pressed.frames !== 1) throw new Error(`press key failed: ${JSON.stringify(pressed)}`);

    const typed = parseToolJson(await callTool('atari_type_text', { text: 'ab', frames_per_key: 1 }));
    if (typed.status !== 'ok' || typed.chars !== 2) throw new Error(`type text failed: ${JSON.stringify(typed)}`);

    const consolePressed = parseToolJson(await callTool('atari_press_console', { key: 'start', frames: 1 }));
    if (consolePressed.status !== 'ok' || consolePressed.active_low !== true) {
      throw new Error(`console press failed: ${JSON.stringify(consolePressed)}`);
    }

    console.log('mcp_phase12_smoke: ok');
  } finally {
    if (initialized) {
      try { await callTool('atari_stop', { force: true }); } catch {}
    }
    server.kill('SIGTERM');
  }
}

main().catch((error) => {
  console.error(error.stack || error.message);
  server.kill('SIGTERM');
  process.exitCode = 1;
});
