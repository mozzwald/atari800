#!/usr/bin/env node
import { spawn, spawnSync } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const archivePath = process.argv[2] || path.join(
  repoRoot,
  'fujinet-pc-ATARI_v1.6.2-dev+git-01d8a27e1_ubuntu-24.04-amd64.tar.gz'
);
if (!fs.existsSync(archivePath)) throw new Error(`FujiNet-PC archive not found: ${archivePath}`);

const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-fujinet-phase9-'));
const diskPath = path.join(testDir, 'phase9-test.atr');
const extracted = spawnSync(
  'tar',
  ['-xOzf', archivePath, 'fujinet-pc-ATARI/data/autorun-cng.atr'],
  { encoding: null, maxBuffer: 4 * 1024 * 1024 }
);
if (extracted.status !== 0) throw new Error(`failed to extract test ATR: ${extracted.stderr}`);
fs.writeFileSync(diskPath, extracted.stdout);

const server = spawn('node', [path.join(repoRoot, 'mcp-server', 'index.js')], {
  cwd: repoRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
});
let nextId = 1;
let stdout = '';
let preservedPath = null;
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
    }, 30000);
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
    clientInfo: { name: 'mcp-fujinet-phase9-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    await callTool('fujinet_set_local_path', { path: archivePath });
    const initial = await callTool('fujinet_config_get', { section: 'General', key: 'boot_mode' });
    if (!initial.includes('"value": "0"')) throw new Error(`unexpected initial boot mode: ${initial}`);

    const changed = await callTool('fujinet_config_set', {
      section: 'General',
      key: 'status_wait_enabled',
      value: 0,
    });
    if (!changed.includes('"backup_path"')) throw new Error(`config backup was not reported: ${changed}`);

    const mounted = await callTool('fujinet_mount_disk', {
      source_path: diskPath,
      drive: 1,
      preserve_modified: true,
    });
    if (!mounted.includes('"copy_to_workspace": true') || !mounted.includes('"mode": "r"')) {
      throw new Error(`safe mount defaults missing: ${mounted}`);
    }

    const mountStatus = await callTool('fujinet_mount_status');
    if (!mountStatus.includes('"boot_mode": "1"') || !mountStatus.includes('"drive": 1')) {
      throw new Error(`mount status is incomplete: ${mountStatus}`);
    }

    const booted = await callTool('fujinet_boot', { display_mode: 'headless', timeout_ms: 15000 });
    if (!booted.includes('"atari_netsio_connected": true')) {
      throw new Error(`headless FujiNet boot did not reconnect: ${booted}`);
    }

    const remounted = await callTool('fujinet_remount', { timeout_ms: 15000 });
    if (!remounted.includes('"pending_remount": false')) {
      throw new Error(`explicit remount did not complete: ${remounted}`);
    }

    const unmounted = await callTool('fujinet_unmount_disk', { drive: 1 });
    const preservedMatch = /"preserved_path": "([^"]+)"/.exec(unmounted);
    preservedPath = preservedMatch?.[1] || null;
    if (!preservedPath || !fs.existsSync(preservedPath)) {
      throw new Error(`preserved disk output missing: ${unmounted}`);
    }
  } finally {
    if (initialized) {
      try { await callTool('atari_stop', { force: true }); } catch {}
    }
    fs.rmSync(testDir, { recursive: true, force: true });
    if (preservedPath) {
      fs.rmSync(path.resolve(preservedPath, '..', '..'), { recursive: true, force: true });
    }
  }
}

main()
  .catch((error) => {
    console.error(error.stack || error.message);
    process.exitCode = 1;
  })
  .finally(() => server.kill('SIGTERM'));
