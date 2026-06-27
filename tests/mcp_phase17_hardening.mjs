#!/usr/bin/env node
import { spawn } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-phase17-'));
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

async function expectToolError(name, args, code) {
  const response = await callTool(name, args, 30000, true);
  if (!response.result.isError) throw new Error(`${name} unexpectedly succeeded`);
  const payload = parseToolJson(response.text);
  if (payload.code !== code) throw new Error(`${name} returned ${payload.code}, expected ${code}: ${response.text}`);
}

async function main() {
  let initialized = false;
  await request('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'mcp-phase17-hardening', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    await expectToolError('fujinet_logs', { regex: '(a+)+$' }, 'BAD_ARGUMENT');
    await expectToolError('fujinet_logs', { regex: 'x'.repeat(257) }, 'BAD_ARGUMENT');

    await callTool('atari_start', { display_mode: 'headless', sound: false }, 30000);
    const status = parseToolJson((await callTool('atari_status')).text);
    fs.mkdirSync(status.session.disk_workspace, { recursive: true });
    await expectToolError('atari_artifact_delete', { root: 'artifacts', path: '.' }, 'PATH_DENIED');
    await expectToolError('atari_artifact_delete', { root: 'native_disks', path: '.' }, 'PATH_DENIED');

    console.log('mcp_phase17_hardening: ok');
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
