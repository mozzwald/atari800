#!/usr/bin/env node
import { spawn } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const testDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcp-phase13-'));
const diskPath = path.join(testDir, 'blank.atr');

function writeBlankAtr(file) {
  const sectors = 720;
  const sectorSize = 128;
  const paragraphs = (sectors * sectorSize) / 16;
  const header = Buffer.alloc(16);
  header[0] = 0x96;
  header[1] = 0x02;
  header[2] = paragraphs & 0xff;
  header[3] = (paragraphs >> 8) & 0xff;
  header[4] = sectorSize & 0xff;
  header[5] = (sectorSize >> 8) & 0xff;
  fs.writeFileSync(file, Buffer.concat([header, Buffer.alloc(sectors * sectorSize)]));
}

writeBlankAtr(diskPath);

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
    clientInfo: { name: 'mcp-phase13-smoke', version: '1.0.0' },
  });
  notify('notifications/initialized');
  initialized = true;

  try {
    const listed = await request('tools/list');
    const names = new Set(listed.tools.map((tool) => tool.name));
    for (const name of [
      'atari_disk_insert',
      'atari_disk_eject',
      'atari_disk_status',
      'atari_artifact_list',
      'atari_artifact_info',
      'atari_artifact_read_text',
      'atari_artifact_delete',
    ]) {
      if (!names.has(name)) throw new Error(`missing MCP tool: ${name}`);
    }

    await callTool('atari_start', { display_mode: 'headless', sound: false }, 30000);
    const status = parseToolJson(await callTool('atari_status'));
    const notePath = path.join(status.session.artifact_dir, 'phase13-note.txt');
    fs.writeFileSync(notePath, 'phase13 artifact text\n');

    const mounted = parseToolJson(await callTool('atari_disk_insert', { source_path: diskPath, drive: 1 }));
    if (mounted.status !== 'ok' || mounted.disk?.read_only !== true || mounted.disk?.source_path !== diskPath) {
      throw new Error(`native disk did not mount read-only from a managed copy: ${JSON.stringify(mounted)}`);
    }
    if (mounted.disk.managed_path === diskPath) throw new Error('native disk mount reused the source path');

    const diskStatus = parseToolJson(await callTool('atari_disk_status', { drive: 1 }));
    if (diskStatus.c_status?.drives?.[0]?.state !== 'read_only') {
      throw new Error(`native disk status was not read-only: ${JSON.stringify(diskStatus)}`);
    }

    const artifacts = parseToolJson(await callTool('atari_artifact_list', { limit: 50 }));
    if (!artifacts.files.some((file) => file.root === 'native_disks' && file.path.endsWith('blank.atr'))) {
      throw new Error(`native disk artifact was not listed: ${JSON.stringify(artifacts)}`);
    }

    const text = parseToolJson(await callTool('atari_artifact_read_text', { path: 'phase13-note.txt' }));
    if (!text.content.includes('phase13 artifact text')) {
      throw new Error(`text artifact read failed: ${JSON.stringify(text)}`);
    }
    const deletedText = parseToolJson(await callTool('atari_artifact_delete', { path: 'phase13-note.txt' }));
    if (deletedText.status !== 'ok' || fs.existsSync(notePath)) {
      throw new Error(`text artifact delete failed: ${JSON.stringify(deletedText)}`);
    }

    await callTool('atari_disk_eject', { drive: 1 });
    const diskRel = path.relative(status.session.disk_workspace, mounted.disk.managed_path).split(path.sep).join('/');
    const diskInfo = parseToolJson(await callTool('atari_artifact_info', { root: 'native_disks', path: diskRel }));
    if (diskInfo.size <= 16) throw new Error(`native disk artifact info was wrong: ${JSON.stringify(diskInfo)}`);
    const deletedDisk = parseToolJson(await callTool('atari_artifact_delete', { root: 'native_disks', path: diskRel }));
    if (deletedDisk.status !== 'ok' || fs.existsSync(mounted.disk.managed_path)) {
      throw new Error(`native disk artifact delete failed: ${JSON.stringify(deletedDisk)}`);
    }

    console.log('mcp_phase13_smoke: ok');
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
