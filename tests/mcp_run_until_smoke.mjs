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
    clientInfo: { name: 'mcp-run-until-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    const listed = await request('tools/list');
    if (!listed.tools.some((tool) => tool.name === 'atari_run_until')) {
      throw new Error('missing MCP tool: atari_run_until');
    }

    await callTool('atari_start', { display_mode: 'headless', sound: false }, 30000);
    const peek = parseToolJson(await callTool('atari_peek', { address: 0, length: 1 }));
    const memoryValue = peek.data[0];

    const frames = parseToolJson(await callTool('atari_run_until', {
      predicates: [{ type: 'frames_elapsed', frames: 3 }],
      max_frames: 5,
      max_ms_wallclock: 5000,
      poll_interval_frames: 1,
    }));
    if (frames.status !== 'ok' || frames.diagnostics.elapsed_frames < 3) {
      throw new Error(`frames_elapsed predicate failed: ${JSON.stringify(frames)}`);
    }

    const memory = parseToolJson(await callTool('atari_run_until', {
      predicates: [{ type: 'memory_equals', addr: 0, data: [memoryValue] }],
      max_frames: 1,
      max_ms_wallclock: 5000,
      poll_interval_frames: 1,
    }));
    if (memory.status !== 'ok' || memory.predicates[0].matched !== true) {
      throw new Error(`memory_equals predicate failed: ${JSON.stringify(memory)}`);
    }

    const timeout = parseToolJson(await callTool('atari_run_until', {
      predicates: [{ type: 'screen_contains', text: 'THIS TEXT SHOULD NOT APPEAR' }],
      max_frames: 1,
      max_ms_wallclock: 5000,
      poll_interval_frames: 1,
      include_debug_tail: 1,
      include_fujinet_log_tail: 1,
      include_netsio_trace_tail: 1,
    }));
    if (timeout.status !== 'timeout' || !timeout.diagnostics || !timeout.diagnostics.session) {
      throw new Error(`timeout diagnostics missing: ${JSON.stringify(timeout)}`);
    }

    console.log('mcp_run_until_smoke: ok');
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
