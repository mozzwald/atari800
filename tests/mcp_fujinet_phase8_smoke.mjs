#!/usr/bin/env node
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const archivePath = process.argv[2] || path.join(repoRoot, 'fujinet-pc-ATARI_v1.6.2-dev+git-01d8a27e1_ubuntu-24.04-amd64.tar.gz');

if (!fs.existsSync(archivePath)) {
  throw new Error(`FujiNet-PC archive not found: ${archivePath}`);
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

function request(method, params = {}) {
  const id = nextId++;
  server.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id, method, params })}\n`);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`timeout waiting for ${method}`));
    }, 20000);
    pending.set(id, {
      resolve: (value) => { clearTimeout(timer); resolve(value); },
      reject: (error) => { clearTimeout(timer); reject(error); },
    });
  });
}

function notify(method, params = {}) {
  server.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', method, params })}\n`);
}

async function callTool(name, args = {}) {
  const result = await request('tools/call', { name, arguments: args });
  const text = result.content?.[0]?.text || '';
  if (result.isError) throw new Error(`${name} failed: ${text}`);
  return text;
}

async function main() {
  let initialized = false;
  await request('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'mcp-fujinet-phase8-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    await callTool('fujinet_set_local_path', { path: archivePath });
    const started = await callTool('fujinet_start', { timeout_ms: 10000 });
    const portMatch = /"udp_port": (\d+)/.exec(started);
    if (!portMatch) throw new Error(`start output did not include selected port: ${started}`);
    const selectedPort = Number(portMatch[1]);
    if (selectedPort === 9997) throw new Error('FujiNet selected the default NetSIO port');

    const status = await callTool('fujinet_status');
    if (!status.includes('"running": true')) throw new Error(`FujiNet status not running: ${status}`);

    const logs = await callTool('fujinet_debug_read', { contains: 'NetSIO', limit: 20 });
    if (!logs.includes('Setting up NetSIO')) throw new Error(`NetSIO startup log not found: ${logs}`);

    const atariStarted = await callTool('atari_start', { display_mode: 'headless' });
    if (!atariStarted.includes('"display_mode": "headless"') || !atariStarted.includes('"xvfb"')) {
      throw new Error(`Atari did not start headless: ${atariStarted}`);
    }

    const atariStatus = await callTool('atari_status');
    if (!atariStatus.includes(`"netsio_port": ${selectedPort}`)) {
      throw new Error(`Atari did not inherit the FujiNet NetSIO port: ${atariStatus}`);
    }
    if (!atariStatus.includes('"-nosound"') || !atariStatus.includes('"-no-video-accel"')) {
      throw new Error(`Atari headless arguments are incomplete: ${atariStatus}`);
    }

    const reconnectDeadline = Date.now() + 10000;
    let fujinetAfterReset = '';
    while (Date.now() < reconnectDeadline) {
      fujinetAfterReset = await callTool('fujinet_status');
      if (fujinetAfterReset.includes('"atari_netsio_connected": true')) break;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    if (!fujinetAfterReset.includes('"atari_netsio_connected": true')) {
      throw new Error(`Atari NetSIO connection was not observed: ${fujinetAfterReset}`);
    }
    if (!fujinetAfterReset.includes('"running": true')) {
      throw new Error(`FujiNet did not survive its Atari-triggered reboot: ${fujinetAfterReset}`);
    }

    await callTool('fujinet_debug_clear');
    const debugStatus = await callTool('fujinet_debug_status');
    if (!debugStatus.includes('"log_count": 0')) throw new Error(`debug clear failed: ${debugStatus}`);
  } finally {
    if (initialized) {
      try {
        await callTool('atari_stop', { force: true });
      } catch {
        // The outer process cleanup remains a final fallback.
      }
    }
  }
}

main()
  .catch((error) => {
    console.error(error.stack || error.message);
    process.exitCode = 1;
  })
  .finally(() => {
    server.kill('SIGTERM');
  });
