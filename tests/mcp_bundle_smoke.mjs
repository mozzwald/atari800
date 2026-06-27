#!/usr/bin/env node
import { spawn, spawnSync } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-bundle-smoke-'));
const outputDir = path.join(testDir, 'dist');
const bundleName = 'bundle-smoke';
const bundleDir = path.join(outputDir, bundleName);
const optionalProgram = process.argv[2] || null;

function runPackage() {
  const result = spawnSync('python3', [
    path.join(repoRoot, 'tools', 'package_mcp_bundle.py'),
    '--output-dir', outputDir,
    '--name', bundleName,
    '--no-tar',
  ], { cwd: repoRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  if (result.status !== 0) {
    throw new Error(`bundle packaging failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  }
  if (!fs.existsSync(path.join(bundleDir, 'bin', 'atari800'))) throw new Error('bundle missing bin/atari800');
  if (!fs.existsSync(path.join(bundleDir, 'src', 'atari800'))) throw new Error('bundle missing src/atari800 compatibility path');
  if (!fs.existsSync(path.join(bundleDir, 'mcp-server', 'node_modules', '@modelcontextprotocol', 'sdk'))) {
    throw new Error('bundle missing MCP node_modules');
  }
  if (!fs.existsSync(path.join(bundleDir, 'skills', 'atari800-mcp', 'SKILL.md'))) {
    throw new Error('bundle missing atari800-mcp skill');
  }
}

function createClient(command, args, cwd) {
  const server = spawn(command, args, {
    cwd,
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

  function request(method, params = {}, timeoutMs = 60000) {
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

  async function callTool(name, args = {}, timeoutMs = 60000) {
    const result = await request('tools/call', { name, arguments: args }, timeoutMs);
    const text = result.content?.[0]?.text || '';
    if (result.isError) throw new Error(`${name} failed: ${text}`);
    return text;
  }

  return { server, request, notify, callTool };
}

function parseToolJson(text) {
  const start = text.indexOf('{');
  if (start < 0) throw new Error(`tool response did not contain JSON: ${text}`);
  return JSON.parse(text.slice(start));
}

async function runServerSmoke(label, command, args, cwd) {
  const client = createClient(command, args, cwd);
  let initialized = false;
  try {
    await client.request('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: `mcp-bundle-smoke-${label}`, version: '1.0.0' },
    });
    client.notify('notifications/initialized');
    initialized = true;

    const preflight = parseToolJson(await client.callTool('atari_preflight'));
    if (!preflight.emulator?.ai_mcp_compatible) {
      throw new Error(`${label} preflight did not report MCP-compatible emulator: ${JSON.stringify(preflight.emulator)}`);
    }
    if (!preflight.emulator.path.endsWith('/bin/atari800')) {
      throw new Error(`${label} used unexpected emulator path: ${preflight.emulator.path}`);
    }

    const startArgs = { display_mode: 'headless', sound: false };
    if (optionalProgram) startArgs.program = optionalProgram;
    await client.callTool('atari_start', startArgs, 60000);
    await client.callTool('atari_run', { frames: 10 }, 30000);
    await client.callTool('atari_stop', { force: true }, 30000);
  } finally {
    if (initialized) {
      try { await client.callTool('atari_stop', { force: true }, 10000); } catch {}
    }
    client.server.kill('SIGTERM');
  }
}

async function main() {
  runPackage();
  await runServerSmoke('launcher', path.join(bundleDir, 'start-mcp.sh'), [], bundleDir);
  await runServerSmoke('direct-node', 'node', [path.join(bundleDir, 'mcp-server', 'index.js')], bundleDir);
  console.log('mcp_bundle_smoke: ok');
}

main().catch((error) => {
  console.error(error.stack || error.message);
  process.exitCode = 1;
}).finally(() => {
  fs.rmSync(testDir, { recursive: true, force: true });
});
