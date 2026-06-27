#!/usr/bin/env node
import { spawn, spawnSync } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const fujinetPath = process.argv[2] || path.join(
  repoRoot,
  'fujinet-pc-ATARI_v1.6.2-dev+git-01d8a27e1_ubuntu-24.04-amd64.tar.gz'
);
if (!fs.existsSync(fujinetPath)) throw new Error(`FujiNet-PC path not found: ${fujinetPath}`);

const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-fujinet-phase10-'));
const diskPath = path.join(testDir, 'phase10-test.atr');
if (fs.statSync(fujinetPath).isDirectory()) {
  const candidates = [
    path.join(fujinetPath, 'data', 'autorun-cng.atr'),
    path.join(fujinetPath, 'fujinet-pc', 'data', 'autorun-cng.atr'),
  ];
  const sourceAtr = candidates.find((candidate) => fs.existsSync(candidate));
  if (!sourceAtr) throw new Error(`autorun-cng.atr not found under FujiNet-PC directory: ${fujinetPath}`);
  fs.copyFileSync(sourceAtr, diskPath);
} else {
  const extracted = spawnSync(
    'tar',
    ['-xOzf', fujinetPath, 'fujinet-pc-ATARI/data/autorun-cng.atr'],
    { encoding: null, maxBuffer: 4 * 1024 * 1024 },
  );
  if (extracted.status !== 0) throw new Error(`failed to extract test ATR: ${extracted.stderr}`);
  fs.writeFileSync(diskPath, extracted.stdout);
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

async function main() {
  let initialized = false;
  await request('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'mcp-fujinet-phase10-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    const listed = await request('tools/list');
    const names = new Set(listed.tools.map((tool) => tool.name));
    for (const name of [
      'atari_netsio_status',
      'atari_netsio_trace_status',
      'atari_netsio_trace_read',
      'atari_netsio_trace_clear',
      'atari_netsio_trace_enable',
      'atari_netsio_trace_disable',
    ]) {
      if (!names.has(name)) throw new Error(`missing MCP tool: ${name}`);
    }

    await callTool('fujinet_set_local_path', { path: fujinetPath });
    await callTool('fujinet_boot', {
      source_path: diskPath,
      display_mode: 'headless',
      timeout_ms: 15000,
    }, 45000);

    const status = parseToolJson(await callTool('atari_netsio_status'));
    if (status.status !== 'ok') throw new Error(`unexpected NetSIO status: ${JSON.stringify(status)}`);
    if (status.compiled !== true) throw new Error('NetSIO was not compiled in');
    if (status.initialized !== true || status.fujinet_known !== true || status.counters?.rx_datagrams < 1) {
      throw new Error(`NetSIO was not initialized or did not observe FujiNet traffic: ${JSON.stringify(status)}`);
    }
    if (!status.pins || status.pins.proceed_ids?.off !== 0x30 || status.pins.interrupt_ids?.on !== 0x41) {
      throw new Error(`Proceed/Interrupt constants missing or wrong: ${JSON.stringify(status.pins)}`);
    }
    if (!status.handler_fields_note || status.netstream?.requested_flags !== null) {
      throw new Error(`handler-side field boundary was not reported: ${JSON.stringify(status.netstream)}`);
    }

    const enabled = parseToolJson(await callTool('atari_netsio_trace_enable'));
    if (enabled.enabled !== true) throw new Error(`trace did not enable: ${JSON.stringify(enabled)}`);
    await callTool('atari_run', { frames: 30 });
    const trace = parseToolJson(await callTool('atari_netsio_trace_read', { since_seq: 0, limit: 32 }));
    if (trace.status !== 'ok' || !Array.isArray(trace.entries)) {
      throw new Error(`bad trace response: ${JSON.stringify(trace)}`);
    }
    if (trace.entries.some((entry) => !entry.direction || !entry.type || entry.decoded === undefined)) {
      throw new Error(`trace entries were not decoded consistently: ${JSON.stringify(trace.entries)}`);
    }
    const traceStatus = parseToolJson(await callTool('atari_netsio_trace_status'));
    if (traceStatus.enabled !== true || traceStatus.capacity < traceStatus.count) {
      throw new Error(`bad trace status: ${JSON.stringify(traceStatus)}`);
    }
    const cleared = parseToolJson(await callTool('atari_netsio_trace_clear'));
    if (cleared.count !== 0 || cleared.dropped !== 0) {
      throw new Error(`trace clear failed: ${JSON.stringify(cleared)}`);
    }
    console.log('mcp_fujinet_phase10_smoke: ok');
  } finally {
    if (initialized) {
      try { await callTool('atari_stop', { force: true }); } catch {}
    }
    fs.rmSync(testDir, { recursive: true, force: true });
  }
}

main()
  .catch((error) => {
    console.error(error.stack || error.message);
    process.exitCode = 1;
  })
  .finally(() => server.kill('SIGTERM'));
