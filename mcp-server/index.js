#!/usr/bin/env node
/**
 * Atari 800 MCP Server
 *
 * Provides MCP tools for controlling an MCP-owned Atari800 AI session.
 */

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import net from 'net';
import dgram from 'dgram';
import https from 'https';
import { spawn, spawnSync } from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const DEFAULT_SOCKET_PATH = '/tmp/atari800_ai.sock';
const RUNTIME_ROOT = process.env.ATARI800_MCP_RUNTIME_DIR || path.join(os.tmpdir(), 'atari800-mcp');
const EMULATOR_PATH = process.env.ATARI800_PATH || path.join(__dirname, '..', 'src', 'atari800');
const LOG_LIMIT = 200;
const FUJINET_LOG_LIMIT = 1000;
const FUJINET_PORT_START = Number(process.env.FUJINET_PORT_START || 19997);
const FUJINET_PORT_END = Number(process.env.FUJINET_PORT_END || 20097);
const FUJINET_CACHE_DIR = process.env.FUJINET_CACHE_DIR || path.join(RUNTIME_ROOT, 'cache', 'fujinet-pc');
const PROJECT_ROOT = path.resolve(__dirname, '..');

let session = null;
let fujinetSelection = { local_path: null, version: null, source: null };

function nowIso() {
  return new Date().toISOString();
}

function makeSessionId() {
  const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14);
  return `a8-${stamp}-${Math.random().toString(16).slice(2, 8)}`;
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
}

function createSession(program, artifactDirOverride = null) {
  ensureDir(RUNTIME_ROOT);
  const sessionId = makeSessionId();
  const runtimeDir = fs.mkdtempSync(path.join(RUNTIME_ROOT, `${sessionId}-`));
  const artifactDir = artifactDirOverride ? path.resolve(artifactDirOverride) : path.join(runtimeDir, 'artifacts');
  const logDir = path.join(runtimeDir, 'logs');
  ensureDir(artifactDir);
  ensureDir(logDir);

  const aiSocket = path.join(runtimeDir, 'ai.sock');
  const videoPushSocket = path.join(runtimeDir, 'video-push.sock');
  const videoPullSocket = path.join(runtimeDir, 'video-pull.sock');

  return {
    session_id: sessionId,
    state: 'starting',
    runtime_dir: runtimeDir,
    artifact_dir: artifactDir,
    log_dir: logDir,
    owned_paths: artifactDirOverride
      ? [runtimeDir, logDir, aiSocket, videoPushSocket, videoPullSocket]
      : [runtimeDir, artifactDir, logDir, aiSocket, videoPushSocket, videoPullSocket],
    logs: [],
    dropped_logs: 0,
    emulator: {
      pid: null,
      argv: [],
      env: {},
      socket: aiSocket,
      video_push_socket: videoPushSocket,
      video_pull_socket: videoPullSocket,
      program: program || null,
      started_at: null,
      exit_code: null,
      signal: null,
      display_mode: null,
      display: null,
      sound: null,
      netsio: null,
      netsio_port: null,
      netsio_connected: false,
    },
    display: null,
    xvfb: null,
    fujinet: null,
  };
}

function recordLog(stream, chunk) {
  if (!session) return;
  const text = chunk.toString();
  for (const line of text.split(/\r?\n/)) {
    if (line.length === 0) continue;
    if (session.logs.length >= LOG_LIMIT) {
      session.logs.shift();
      session.dropped_logs += 1;
    }
    session.logs.push({
      seq: session.dropped_logs + session.logs.length + 1,
      timestamp: nowIso(),
      stream,
      text: line,
    });
    if (stream === 'stdout' && line.includes('netsio connected after')) {
      session.emulator.netsio_connected = true;
    }
  }
}

function sessionSnapshot(includeLogs = true) {
  if (!session) {
    return { state: 'not_started' };
  }
  return {
    session_id: session.session_id,
    state: session.state,
    runtime_dir: session.runtime_dir,
    artifact_dir: session.artifact_dir,
    emulator: session.emulator,
    xvfb: session.xvfb,
    fujinet: session.fujinet ? {
      ...session.fujinet,
      logs: undefined,
      last_log_seq: session.fujinet.log_seq || 0,
      log_count: session.fujinet.logs?.length || 0,
    } : null,
    owned_paths: session.owned_paths,
    logs: includeLogs ? {
      lines: session.logs,
      dropped: session.dropped_logs,
      limit: LOG_LIMIT,
    } : undefined,
  };
}

function emulatorProcessRunning() {
  return Boolean(session?.process && session.process.exitCode === null && session.process.signalCode === null);
}

function fujinetProcessRunning() {
  return Boolean(session?.fujinet_process && session.fujinet_process.exitCode === null && session.fujinet_process.signalCode === null);
}

function formatJson(value) {
  return JSON.stringify(value, null, 2);
}

function currentSocketPath() {
  return session?.emulator?.socket || DEFAULT_SOCKET_PATH;
}

function makeError(code, message, details = {}) {
  const err = new Error(message);
  err.code = code;
  err.details = details;
  return err;
}

function executablePath(name) {
  const pathEnv = process.env.PATH || '';
  for (const dir of pathEnv.split(path.delimiter)) {
    if (!dir) continue;
    const candidate = path.join(dir, name);
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      return candidate;
    } catch {
      // Continue searching PATH.
    }
  }
  return null;
}

function fileExecutable(file) {
  try {
    fs.accessSync(file, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

async function runCapture(command, args = [], timeoutMs = 2000) {
  return new Promise((resolve) => {
    let stdout = '';
    let stderr = '';
    let timedOut = false;
    const proc = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    const timer = setTimeout(() => {
      timedOut = true;
      proc.kill('SIGKILL');
    }, timeoutMs);
    proc.stdout.on('data', (chunk) => {
      stdout += chunk.toString();
      if (stdout.length > 4096) stdout = stdout.slice(-4096);
    });
    proc.stderr.on('data', (chunk) => {
      stderr += chunk.toString();
      if (stderr.length > 4096) stderr = stderr.slice(-4096);
    });
    proc.on('error', (error) => {
      clearTimeout(timer);
      resolve({ ok: false, error: error.message, stdout, stderr, timed_out: timedOut });
    });
    proc.on('exit', (code, signal) => {
      clearTimeout(timer);
      resolve({ ok: code === 0, code, signal, stdout, stderr, timed_out: timedOut });
    });
  });
}

function recordFujiNetLog(stream, chunk) {
  if (!session?.fujinet) return;
  if (!session.fujinet.logs) session.fujinet.logs = [];
  if (!session.fujinet.dropped_logs) session.fujinet.dropped_logs = 0;
  const text = chunk.toString();
  for (const line of text.split(/\r?\n/)) {
    if (line.length === 0) continue;
    if (session.fujinet.logs.length >= FUJINET_LOG_LIMIT) {
      session.fujinet.logs.shift();
      session.fujinet.dropped_logs += 1;
    }
    session.fujinet.log_seq = (session.fujinet.log_seq || 0) + 1;
    const entry = {
      seq: session.fujinet.log_seq,
      timestamp: nowIso(),
      stream,
      text: line,
    };
    session.fujinet.logs.push(entry);
    recordLog(`fujinet.${stream}`, Buffer.from(line));
  }
}

function findLocalFujiNetArchives() {
  const entries = [];
  for (const dir of [PROJECT_ROOT, FUJINET_CACHE_DIR]) {
    if (!fs.existsSync(dir)) continue;
    for (const name of fs.readdirSync(dir)) {
      if (/^fujinet-pc-ATARI_.*\.(tar\.gz|tgz)$/i.test(name)) {
        entries.push(path.join(dir, name));
      }
    }
  }
  return entries.sort();
}

function describeFujiNetPath(file) {
  const name = path.basename(file);
  const match = /^fujinet-pc-ATARI_(.+?)_(.+)\.(?:tar\.gz|tgz)$/i.exec(name);
  return {
    path: file,
    name,
    version: match?.[1] || null,
    platform: match?.[2] || null,
    selected: fujinetSelection.local_path === file,
  };
}

function detectFujiNetAssetPattern() {
  if (process.platform === 'linux') {
    if (process.arch !== 'x64' || !fs.existsSync('/etc/os-release')) return null;
    const release = fs.readFileSync('/etc/os-release', 'utf8');
    const distro = /^ID=(?:"([^"]+)"|([^\n]+))$/m.exec(release);
    const version = /^VERSION_ID=(?:"([^"]+)"|([^\n]+))$/m.exec(release);
    const distroId = (distro?.[1] || distro?.[2] || '').trim();
    const versionId = (version?.[1] || version?.[2] || '').trim();
    if (distroId !== 'ubuntu' || !['22.04', '24.04'].includes(versionId)) return null;
    return `ubuntu-${versionId}-amd64`;
  }
  if (process.platform === 'darwin') {
    if (!['arm64', 'x64'].includes(process.arch)) return null;
    const macosByDarwin = { 23: '14', 24: '15' };
    const macos = macosByDarwin[Number(os.release().split('.')[0])];
    if (!macos) return null;
    return `macos-${macos}-${process.arch === 'arm64' ? 'arm64' : 'x64'}`;
  }
  return null;
}

function currentFujiNetSelection() {
  const localArchives = findLocalFujiNetArchives().map(describeFujiNetPath);
  return {
    selected: fujinetSelection,
    detected_asset_pattern: detectFujiNetAssetPattern(),
    local_archives: localArchives,
    cache_dir: FUJINET_CACHE_DIR,
    port_range: { start: FUJINET_PORT_START, end: FUJINET_PORT_END },
  };
}

function selectedOrDiscoveredFujiNetPath(explicitPath = null) {
  if (explicitPath) return path.resolve(explicitPath);
  if (fujinetSelection.local_path) return fujinetSelection.local_path;
  const archives = findLocalFujiNetArchives();
  const pattern = detectFujiNetAssetPattern();
  if (pattern) {
    const matched = archives.find((archive) => archive.includes(pattern));
    if (matched) return matched;
  }
  if (archives.length > 0) {
    throw makeError('BAD_ARGUMENT', 'FujiNet-PC host asset could not be selected automatically; select one explicitly', {
      detected_asset_pattern: pattern,
      archives: archives.map(describeFujiNetPath),
      hint: 'Use fujinet_set_local_path or pass asset_pattern to fujinet_fetch_latest.',
    });
  }
  throw makeError('CAPABILITY_UNAVAILABLE', 'FujiNet-PC is not selected and no local archive was found', {
    hint: 'Use fujinet_set_local_path with an unpacked FujiNet-PC directory or tar.gz archive.',
  });
}

function findFujiNetExecutable(root) {
  const candidates = [
    path.join(root, 'fujinet'),
    path.join(root, 'fujinet-pc-ATARI', 'fujinet'),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const nested = path.join(root, entry.name, 'fujinet');
    if (fs.existsSync(nested)) return nested;
  }
  return null;
}

function copyOrExtractFujiNet(sourcePath, targetRoot) {
  ensureDir(targetRoot);
  const stat = fs.statSync(sourcePath);
  if (stat.isDirectory()) {
    const target = path.join(targetRoot, 'fujinet-pc-ATARI');
    fs.cpSync(sourcePath, target, { recursive: true, force: true, dereference: true });
    return target;
  }
  if (!/\.(tar\.gz|tgz)$/i.test(sourcePath)) {
    throw makeError('BAD_ARGUMENT', 'FujiNet local path must be an unpacked directory or .tar.gz archive', { path: sourcePath });
  }
  const result = spawnSync('tar', ['-xzf', sourcePath, '-C', targetRoot], { encoding: 'utf8' });
  if (result.status !== 0) {
    throw makeError('IO_ERROR', 'failed to extract FujiNet-PC archive', { stderr: result.stderr, sourcePath });
  }
  const executable = findFujiNetExecutable(targetRoot);
  if (!executable) {
    throw makeError('BAD_ARGUMENT', 'FujiNet-PC archive did not contain a fujinet executable', { sourcePath });
  }
  return path.dirname(executable);
}

function prepareFujiNetInstall(sourcePath) {
  if (!session) {
    cleanupStaleRuntimeDirs();
    session = createSession(null);
    session.state = 'not_started';
  }
  const resolvedSource = selectedOrDiscoveredFujiNetPath(sourcePath);
  const installRoot = path.join(session.runtime_dir, 'fujinet-install');
  fs.rmSync(installRoot, { recursive: true, force: true });
  const workDir = copyOrExtractFujiNet(resolvedSource, installRoot);
  const executable = findFujiNetExecutable(workDir) || path.join(workDir, 'fujinet');
  if (!fs.existsSync(executable)) {
    throw makeError('BAD_ARGUMENT', 'FujiNet-PC executable not found after preparation', { workDir });
  }
  fs.chmodSync(executable, 0o755);
  const packagedLauncher = path.join(workDir, 'run-fujinet');
  const launcher = fs.existsSync(packagedLauncher) ? packagedLauncher : executable;
  fs.chmodSync(launcher, 0o755);
  const sdPath = path.join(workDir, 'SD');
  ensureDir(sdPath);
  const configPath = path.join(workDir, 'fnconfig.ini');
  if (!fs.existsSync(configPath)) {
    const dataConfig = path.join(workDir, 'data', 'fnconfig.ini');
    if (fs.existsSync(dataConfig)) fs.copyFileSync(dataConfig, configPath);
    else fs.writeFileSync(configPath, '[BOIP]\nenabled=1\nhost=localhost\nport=\n');
  }
  return { sourcePath: resolvedSource, workDir, executable, launcher, sdPath, configPath };
}

function setIniSectionValue(text, section, key, value) {
  const lines = text.split(/\r?\n/);
  let sectionStart = -1;
  let sectionEnd = lines.length;
  for (let i = 0; i < lines.length; i += 1) {
    if (lines[i].trim().toLowerCase() === `[${section.toLowerCase()}]`) {
      sectionStart = i;
      for (let j = i + 1; j < lines.length; j += 1) {
        if (/^\s*\[.*\]\s*$/.test(lines[j])) {
          sectionEnd = j;
          break;
        }
      }
      break;
    }
  }
  if (sectionStart < 0) {
    lines.push('', `[${section}]`, `${key}=${value}`);
    return lines.join('\n');
  }
  const keyRe = new RegExp(`^\\s*${key.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\s*=`, 'i');
  for (let i = sectionStart + 1; i < sectionEnd; i += 1) {
    if (keyRe.test(lines[i])) {
      lines[i] = `${key}=${value}`;
      return lines.join('\n');
    }
  }
  lines.splice(sectionEnd, 0, `${key}=${value}`);
  return lines.join('\n');
}

function writeFujiNetConfig(configPath, port) {
  let text = fs.existsSync(configPath) ? fs.readFileSync(configPath, 'utf8') : '';
  text = setIniSectionValue(text, 'BOIP', 'enabled', '1');
  text = setIniSectionValue(text, 'BOIP', 'host', 'localhost');
  text = setIniSectionValue(text, 'BOIP', 'port', String(port));
  const tmp = `${configPath}.tmp`;
  fs.writeFileSync(tmp, text);
  fs.renameSync(tmp, configPath);
}

async function udpPortAvailable(port) {
  return new Promise((resolve) => {
    const socket = dgram.createSocket('udp4');
    socket.once('error', () => {
      socket.close();
      resolve(false);
    });
    socket.once('listening', () => {
      socket.close();
      resolve(true);
    });
    socket.bind(port, '127.0.0.1');
  });
}

async function allocateFujiNetPort(preferred = null) {
  if (preferred !== null && preferred !== undefined) {
    const port = Number(preferred);
    if (port === 9997) throw makeError('BAD_ARGUMENT', 'FujiNet sidecar must not use default NetSIO port 9997 by default');
    if (!(await udpPortAvailable(port))) throw makeError('BUSY', 'requested FujiNet NetSIO port is unavailable', { port });
    return port;
  }
  for (let port = FUJINET_PORT_START; port <= FUJINET_PORT_END; port += 1) {
    if (port === 9997) continue;
    if (await udpPortAvailable(port)) return port;
  }
  throw makeError('BUSY', 'no free FujiNet NetSIO UDP port found', { start: FUJINET_PORT_START, end: FUJINET_PORT_END });
}

async function waitForFujiNetReady(port, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (session?.fujinet?.exit_code !== null || session?.fujinet?.signal !== null) {
      throw makeError('EMULATOR_EXITED', 'FujiNet-PC exited during startup', { fujinet: session.fujinet });
    }
    const lines = session?.fujinet?.logs || [];
    if (lines.some((line) => line.text.includes(`Setting up NetSIO (localhost:${port})`))) return true;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw makeError('TIMEOUT', 'FujiNet-PC did not report NetSIO startup before timeout', {
    port,
    logs: session?.fujinet?.logs?.slice(-40) || [],
  });
}

function signalFujiNetProcess(proc, signal) {
  try {
    if (session?.fujinet?.process_group) process.kill(-proc.pid, signal);
    else proc.kill(signal);
  } catch (error) {
    if (error.code !== 'ESRCH') throw error;
  }
}

async function stopFujiNet({ force = false } = {}) {
  if (!session?.fujinet_process || !session.fujinet) {
    return { status: 'ok', state: 'not_started' };
  }
  const proc = session.fujinet_process;
  if (proc.exitCode === null && proc.signalCode === null) {
    signalFujiNetProcess(proc, 'SIGTERM');
    const deadline = Date.now() + 2000;
    while (Date.now() < deadline && proc.exitCode === null && proc.signalCode === null) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    if (force && proc.exitCode === null && proc.signalCode === null) {
      proc.kill('SIGKILL');
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    if (proc.exitCode === null && proc.signalCode === null) {
      throw makeError('BUSY', 'tracked FujiNet-PC process did not exit after SIGTERM; retry with force=true');
    }
  }
  session.fujinet.state = 'exited';
  const snapshot = session.fujinet;
  session.fujinet_process = null;
  return { status: 'ok', fujinet: snapshot };
}

function fujinetStatusSnapshot() {
  const info = session?.fujinet || null;
  return {
    state: info?.state || 'not_started',
    running: fujinetProcessRunning(),
    selection: currentFujiNetSelection(),
    fujinet: info ? {
      ...info,
      logs: undefined,
      last_log_seq: info.log_seq || 0,
      log_count: info.logs?.length || 0,
      dropped_logs: info.dropped_logs || 0,
      atari_netsio_connected: Boolean(
        session?.emulator?.netsio_connected && session.emulator.netsio_port === info.udp_port
      ),
    } : null,
  };
}

async function startFujiNetSidecar(args = {}) {
  if (session?.fujinet_process && session.fujinet_process.exitCode === null && session.fujinet_process.signalCode === null) {
    throw makeError('BUSY', 'FujiNet-PC sidecar is already running', { fujinet: session.fujinet });
  }
  const port = await allocateFujiNetPort(args.port ?? args.netsio_port ?? null);
  const prepared = prepareFujiNetInstall(args.local_path || args.archive_path || null);
  writeFujiNetConfig(prepared.configPath, port);

  const argv = ['-c', prepared.configPath, '-s', prepared.sdPath];
  if (args.web_url) argv.unshift('-u', args.web_url);
  const proc = spawn(prepared.launcher, argv, {
    cwd: prepared.workDir,
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: true,
  });
  session.fujinet = {
    state: 'starting',
    selected_version: fujinetSelection.version || path.basename(prepared.sourcePath),
    source_path: prepared.sourcePath,
    executable_path: prepared.executable,
    launcher_path: prepared.launcher,
    working_dir: prepared.workDir,
    argv: [prepared.launcher, ...argv],
    pid: proc.pid,
    udp_port: port,
    config_path: prepared.configPath,
    sd_path: prepared.sdPath,
    data_path: path.join(prepared.workDir, 'data'),
    started_at: nowIso(),
    exit_code: null,
    signal: null,
    process_group: true,
    logs: [],
    dropped_logs: 0,
    log_seq: 0,
  };
  session.fujinet_process = proc;
  session.owned_paths.push(path.dirname(prepared.workDir));
  proc.stdout.on('data', (chunk) => recordFujiNetLog('stdout', chunk));
  proc.stderr.on('data', (chunk) => recordFujiNetLog('stderr', chunk));
  proc.on('error', (error) => {
    if (!session?.fujinet) return;
    session.fujinet.state = 'crashed';
    recordFujiNetLog('mcp', Buffer.from(`fujinet spawn error: ${error.message}`));
  });
  proc.on('exit', (code, signal) => {
    if (!session?.fujinet) return;
    session.fujinet.exit_code = code;
    session.fujinet.signal = signal;
    session.fujinet.state = code === 0 ? 'exited' : 'crashed';
  });
  try {
    await waitForFujiNetReady(port, args.timeout_ms ?? 5000);
    session.fujinet.state = 'running';
    return session.fujinet;
  } catch (error) {
    await stopFujiNet({ force: true });
    throw error;
  }
}

function readFujiNetLogs({ since_seq = 0, limit = 100, contains, regex, stream = 'both' } = {}) {
  if (!session?.fujinet) return { lines: [], next_seq: since_seq, dropped: 0 };
  const max = Math.max(1, Math.min(Number(limit) || 100, FUJINET_LOG_LIMIT));
  let matcher = null;
  if (regex) matcher = new RegExp(regex);
  let lines = session.fujinet.logs || [];
  lines = lines.filter((line) => line.seq > since_seq);
  if (stream !== 'both') lines = lines.filter((line) => line.stream === stream);
  if (contains) lines = lines.filter((line) => line.text.includes(contains));
  if (matcher) lines = lines.filter((line) => matcher.test(line.text));
  lines = lines.slice(-max);
  return {
    lines,
    next_seq: lines.length ? lines[lines.length - 1].seq + 1 : since_seq,
    dropped: session.fujinet.dropped_logs || 0,
    limit: FUJINET_LOG_LIMIT,
  };
}

function githubJson(url) {
  return new Promise((resolve, reject) => {
    https.get(url, { headers: { 'User-Agent': 'atari800-mcp' } }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        if (res.statusCode < 200 || res.statusCode >= 300) {
          reject(makeError('IO_ERROR', 'GitHub request failed', { status: res.statusCode, body: data.slice(0, 500) }));
          return;
        }
        try { resolve(JSON.parse(data)); }
        catch (error) { reject(error); }
      });
    }).on('error', reject);
  });
}

async function listRemoteFujiNetAssets() {
  const releases = await githubJson('https://api.github.com/repos/FujiNetWIFI/fujinet-firmware/releases?per_page=20');
  const assets = [];
  for (const release of releases) {
    for (const asset of release.assets || []) {
      if (/fujinet-pc-ATARI_.*\.(tar\.gz|tgz)$/i.test(asset.name)) {
        assets.push({
          release: release.tag_name,
          prerelease: release.prerelease,
          name: asset.name,
          url: asset.browser_download_url,
          size: asset.size,
        });
      }
    }
  }
  return assets;
}

async function downloadFile(url, dest) {
  ensureDir(path.dirname(dest));
  return new Promise((resolve, reject) => {
    const file = fs.createWriteStream(dest);
    https.get(url, { headers: { 'User-Agent': 'atari800-mcp' } }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        file.close();
        fs.unlinkSync(dest);
        downloadFile(res.headers.location, dest).then(resolve, reject);
        return;
      }
      if (res.statusCode !== 200) {
        file.close();
        fs.unlinkSync(dest);
        reject(makeError('IO_ERROR', 'download failed', { status: res.statusCode, url }));
        return;
      }
      res.pipe(file);
      file.on('finish', () => file.close(resolve));
    }).on('error', (error) => {
      file.close();
      try { fs.unlinkSync(dest); } catch {}
      reject(error);
    });
  });
}

function selectLocalFujiNetByVersion(version) {
  const archives = findLocalFujiNetArchives();
  const matches = archives.filter((archive) => path.basename(archive).includes(version));
  if (matches.length === 0) {
    throw makeError('CAPABILITY_UNAVAILABLE', 'no local FujiNet-PC archive matches requested version', {
      version,
      local_archives: archives.map(describeFujiNetPath),
      hint: 'Use fujinet_fetch_latest or fujinet_set_local_path.',
    });
  }
  if (matches.length > 1) {
    throw makeError('BAD_ARGUMENT', 'multiple local FujiNet-PC archives match requested version', {
      version,
      matches: matches.map(describeFujiNetPath),
    });
  }
  fujinetSelection = { local_path: matches[0], version, source: 'local_version' };
  return currentFujiNetSelection();
}

async function fetchFujiNetAsset(args = {}) {
  const assets = await listRemoteFujiNetAssets();
  const pattern = args.asset_pattern || detectFujiNetAssetPattern();
  if (!pattern) {
    throw makeError('BAD_ARGUMENT', 'host platform is not auto-detected for FujiNet-PC; pass asset_pattern or use fujinet_set_local_path', {
      assets,
    });
  }
  let candidates = assets.filter((asset) => asset.name.includes(pattern));
  if (args.version) candidates = candidates.filter((asset) => asset.name.includes(args.version) || asset.release === args.version);
  if (args.tag) candidates = candidates.filter((asset) => asset.release === args.tag);
  if (candidates.length === 0) {
    throw makeError('CAPABILITY_UNAVAILABLE', 'no FujiNet-PC Atari asset matched host/version selection', {
      pattern,
      version: args.version || null,
      tag: args.tag || null,
      available_assets: assets,
    });
  }
  const asset = candidates[0];
  const dest = path.join(FUJINET_CACHE_DIR, asset.name);
  if (!fs.existsSync(dest) || args.force === true) {
    await downloadFile(asset.url, dest);
  }
  fujinetSelection = { local_path: dest, version: args.version || asset.release || asset.name, source: 'download' };
  return { asset, path: dest, selection: currentFujiNetSelection() };
}

function nativeDisplayAvailable() {
  return Boolean(process.env.DISPLAY || process.env.WAYLAND_DISPLAY || process.platform === 'darwin');
}

async function buildPreflight() {
  const xvfbPath = executablePath('Xvfb');
  const emulatorExists = fs.existsSync(EMULATOR_PATH);
  const emulatorExecutable = emulatorExists && fileExecutable(EMULATOR_PATH);
  const help = emulatorExecutable ? await runCapture(EMULATOR_PATH, ['-help'], 2500) : null;
  const helpText = help ? `${help.stdout}\n${help.stderr}` : '';
  const missing = [];
  if (!emulatorExists) missing.push({ dependency: 'atari800', hint: `Set ATARI800_PATH or build ${EMULATOR_PATH}` });
  else if (!emulatorExecutable) missing.push({ dependency: 'atari800', hint: `Make ${EMULATOR_PATH} executable` });
  if (!xvfbPath && process.platform === 'linux') {
    missing.push({ dependency: 'Xvfb', hint: 'Install xvfb / xorg-x11-server-Xvfb / xorg-server-xvfb for headless mode' });
  }

  return {
    emulator: {
      path: EMULATOR_PATH,
      exists: emulatorExists,
      executable: emulatorExecutable,
      help_available: Boolean(help),
      help_exit_code: help?.code ?? null,
      help_excerpt: helpText.split(/\r?\n/).filter(Boolean).slice(0, 20),
      ai_interface_hint: helpText.includes('-ai') || helpText.includes('AI'),
      netsio_hint: helpText.includes('-netsio'),
    },
    host: {
      platform: process.platform,
      arch: process.arch,
      os_type: os.type(),
      os_release: os.release(),
    },
    display: {
      display: process.env.DISPLAY || null,
      wayland_display: process.env.WAYLAND_DISPLAY || null,
      native_available: nativeDisplayAvailable(),
      xvfb_path: xvfbPath,
      xvfb_available: Boolean(xvfbPath),
      headless_supported: process.platform === 'linux' && Boolean(xvfbPath),
      caveat: 'Xvfb helps only with X11-compatible Atari800/SDL builds.',
    },
    runtime: {
      root: RUNTIME_ROOT,
      root_exists: fs.existsSync(RUNTIME_ROOT),
      temp_dir: os.tmpdir(),
    },
    fujinet: currentFujiNetSelection(),
    missing_dependencies: missing,
  };
}

function selectXvfbDisplay(preferred) {
  if (preferred !== undefined && preferred !== null) {
    return Number(preferred);
  }
  for (let display = 90; display < 200; display += 1) {
    if (!fs.existsSync(`/tmp/.X11-unix/X${display}`) && !fs.existsSync(`/tmp/.X${display}-lock`)) {
      return display;
    }
  }
  throw makeError('CAPABILITY_UNAVAILABLE', 'No free Xvfb display number found');
}

async function waitForXvfb(display, timeoutMs = 3000) {
  const socketPath = `/tmp/.X11-unix/X${display}`;
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (fs.existsSync(socketPath)) return true;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  return false;
}

async function startXvfbForSession(display, screen) {
  const xvfbPath = executablePath('Xvfb');
  if (!xvfbPath || process.platform !== 'linux') {
    throw makeError('CAPABILITY_UNAVAILABLE', 'Xvfb is not available for headless mode', {
      hint: 'Install Xvfb or use display_mode=visible with a native display.',
    });
  }
  const displayName = `:${display}`;
  const argv = [displayName, '-screen', '0', screen, '-nolisten', 'tcp'];
  const proc = spawn(xvfbPath, argv, { stdio: ['ignore', 'pipe', 'pipe'] });
  session.xvfb = {
    pid: proc.pid,
    path: xvfbPath,
    argv: [xvfbPath, ...argv],
    display: displayName,
    screen,
    started_at: nowIso(),
    exit_code: null,
    signal: null,
  };
  session.xvfb_process = proc;
  proc.stdout.on('data', (chunk) => recordLog('xvfb.stdout', chunk));
  proc.stderr.on('data', (chunk) => recordLog('xvfb.stderr', chunk));
  proc.on('exit', (code, signal) => {
    if (!session?.xvfb) return;
    session.xvfb.exit_code = code;
    session.xvfb.signal = signal;
  });
  if (!(await waitForXvfb(display))) {
    proc.kill('SIGTERM');
    throw makeError('CAPABILITY_UNAVAILABLE', 'Xvfb did not become ready before timeout', {
      display: displayName,
      logs: session.logs.slice(-20),
    });
  }
  return displayName;
}

function resolveMachineArg(machine, ram) {
  const requested = machine || (ram === 128 ? 'xe' : 'xl');
  const machines = {
    atari: '-atari',
    xl: '-xl',
    xe: '-xe',
    xegs: '-xegs',
    '5200': '-5200',
  };
  if (!machines[String(requested)]) {
    throw makeError('BAD_ARGUMENT', 'machine must be atari, xl, xe, xegs, or 5200', { machine: requested });
  }
  if (ram !== undefined && ram !== null) {
    const ramValue = Number(ram);
    if (ramValue === 320) return '-320xe';
    if (ramValue === 576) return '-576xe';
    if (ramValue === 1088) return '-1088xe';
    if (ramValue === 128) return '-xe';
    if (![48, 64].includes(ramValue)) {
      throw makeError('BAD_ARGUMENT', 'ram must be one of 48, 64, 128, 320, 576, or 1088', { ram });
    }
  }
  return machines[String(requested)];
}

function filterExtraArgs(extraArgs = [], unsafeOverride = false) {
  if (!Array.isArray(extraArgs)) {
    throw makeError('BAD_ARGUMENT', 'args must be an array of strings');
  }
  const denied = new Set([
    '-ai',
    '-ai-run',
    '-ai-socket',
    '-ai-video-push-socket',
    '-ai-video-pull-socket',
    '-ai-artifact-dir',
    '-ai-unsafe-paths',
    '-netsio',
    '-nosound',
  ]);
  const out = [];
  for (const item of extraArgs) {
    if (typeof item !== 'string') {
      throw makeError('BAD_ARGUMENT', 'args must contain only strings');
    }
    if (!unsafeOverride && denied.has(item)) {
      throw makeError('BAD_ARGUMENT', 'args contains a managed flag; set unsafe_override=true to allow it', { flag: item });
    }
    out.push(item);
  }
  return out;
}

async function sendCommand(cmd, socketPath = currentSocketPath()) {
  return new Promise((resolve, reject) => {
    const client = net.createConnection(socketPath, () => {
      const json = JSON.stringify(cmd);
      client.write(`${Buffer.byteLength(json)}\n${json}`);
    });

    let data = Buffer.alloc(0);
    let expectedLength = null;

    client.on('data', (chunk) => {
      data = Buffer.concat([data, chunk]);

      if (expectedLength === null) {
        const newlineIdx = data.indexOf(10);
        if (newlineIdx !== -1) {
          expectedLength = Number.parseInt(data.subarray(0, newlineIdx).toString(), 10);
          data = data.subarray(newlineIdx + 1);
        }
      }

      if (expectedLength !== null && data.length >= expectedLength) {
        client.end();
        try {
          resolve(JSON.parse(data.subarray(0, expectedLength).toString()));
        } catch (e) {
          reject(new Error(`Failed to parse response: ${data.toString()}`));
        }
      }
    });

    client.on('error', reject);
    client.on('timeout', () => {
      client.end();
      reject(new Error('Connection timeout'));
    });
    client.setTimeout(10000);
  });
}

async function waitForReady(timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (session?.emulator?.exit_code !== null || session?.emulator?.signal !== null) {
      throw new Error('Emulator exited during startup');
    }
    if (fs.existsSync(session.emulator.socket)) {
      try {
        const resp = await sendCommand({ cmd: 'hello' }, session.emulator.socket);
        if (resp.status === 'ok') {
          session.state = 'running';
          return resp;
        }
      } catch {
        // Socket may exist before the server accepts commands.
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('Emulator socket was not ready before timeout');
}

function cleanupRuntime(cleanupRuntimeDir) {
  if (!session) return;
  if (cleanupRuntimeDir) {
    try {
      fs.rmSync(session.runtime_dir, { recursive: true, force: true });
    } catch (e) {
      session.state = 'cleanup_failed';
      recordLog('mcp', Buffer.from(`cleanup failed: ${e.message}`));
    }
  } else {
    for (const p of [
      session.emulator.socket,
      session.emulator.video_push_socket,
      session.emulator.video_pull_socket,
    ]) {
      try {
        fs.unlinkSync(p);
      } catch {
        // Missing sockets are harmless.
      }
    }
  }
}

async function stopSession({ force = false, cleanup_runtime_dir = true } = {}) {
  if (!session) {
    return { status: 'ok', state: 'not_started' };
  }

  const proc = session.process;
  if (proc && proc.exitCode === null && proc.signalCode === null) {
    proc.kill('SIGTERM');
    const deadline = Date.now() + 2000;
    while (Date.now() < deadline && proc.exitCode === null && proc.signalCode === null) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    if (force && proc.exitCode === null && proc.signalCode === null) {
      proc.kill('SIGKILL');
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    if (proc.exitCode === null && proc.signalCode === null) {
      session.state = 'running';
      return {
        status: 'error',
        code: 'BUSY',
        message: 'tracked emulator did not exit after SIGTERM; retry with force=true',
        session: sessionSnapshot(),
      };
    }
  }

  try {
    await stopFujiNet({ force });
  } catch (error) {
    session.state = 'running';
    return {
      status: 'error',
      code: error.code || 'BUSY',
      message: error.message,
      session: sessionSnapshot(),
    };
  }

  if (session.state !== 'cleanup_failed') {
    session.state = 'exited';
  }
  const xvfbProc = session.xvfb_process;
  if (xvfbProc && xvfbProc.exitCode === null && xvfbProc.signalCode === null) {
    xvfbProc.kill('SIGTERM');
    const deadline = Date.now() + 1000;
    while (Date.now() < deadline && xvfbProc.exitCode === null && xvfbProc.signalCode === null) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    if (force && xvfbProc.exitCode === null && xvfbProc.signalCode === null) {
      xvfbProc.kill('SIGKILL');
    }
  }
  cleanupRuntime(cleanup_runtime_dir);

  const snapshot = sessionSnapshot();
  session = null;
  return { status: 'ok', session: snapshot };
}

function cleanupStaleRuntimeDirs(maxAgeMs = 24 * 60 * 60 * 1000) {
  ensureDir(RUNTIME_ROOT);
  const now = Date.now();
  for (const entry of fs.readdirSync(RUNTIME_ROOT, { withFileTypes: true })) {
    if (!entry.isDirectory() || !entry.name.startsWith('a8-')) continue;
    const full = path.join(RUNTIME_ROOT, entry.name);
    if (session && full === session.runtime_dir) continue;
    try {
      const stat = fs.statSync(full);
      if (now - stat.mtimeMs > maxAgeMs) {
        fs.rmSync(full, { recursive: true, force: true });
      }
    } catch {
      // Ignore stale cleanup races.
    }
  }
}

function formatScreen(data) {
  if (!data || !Array.isArray(data)) return 'No screen data';
  return [
    '+' + '-'.repeat(40) + '+',
    ...data.map((line) => '|' + String(line).padEnd(40).slice(0, 40) + '|'),
    '+' + '-'.repeat(40) + '+',
  ].join('\n');
}

function formatToolResponse(summary, response) {
  return `${summary}\n${formatJson(response)}`;
}

const server = new Server(
  { name: 'atari800', version: '1.0.0' },
  { capabilities: { tools: {} } },
);

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: 'atari_preflight',
      description: 'Report emulator, display, Xvfb, runtime, and host dependency status.',
      inputSchema: { type: 'object', properties: {} },
    },
    {
      name: 'fujinet_list_versions',
      description: 'List local FujiNet-PC Atari archives and optionally GitHub release assets.',
      inputSchema: { type: 'object', properties: { include_remote: { type: 'boolean', default: false } } },
    },
    {
      name: 'fujinet_set_local_path',
      description: 'Select an unpacked FujiNet-PC directory or local .tar.gz archive for offline use.',
      inputSchema: { type: 'object', properties: { path: { type: 'string' } }, required: ['path'] },
    },
    {
      name: 'fujinet_select_version',
      description: 'Select a pinned local FujiNet-PC archive by version/tag/name substring.',
      inputSchema: { type: 'object', properties: { version: { type: 'string' } }, required: ['version'] },
    },
    {
      name: 'fujinet_fetch_latest',
      description: 'Fetch and select the latest matching FujiNet-PC Atari release asset for this host.',
      inputSchema: {
        type: 'object',
        properties: {
          version: { type: 'string' },
          tag: { type: 'string' },
          asset_pattern: { type: 'string' },
          force: { type: 'boolean', default: false },
        },
      },
    },
    {
      name: 'fujinet_start',
      description: 'Start FujiNet-PC as an MCP-owned sidecar with a non-default NetSIO UDP port.',
      inputSchema: {
        type: 'object',
        properties: {
          local_path: { type: 'string' },
          archive_path: { type: 'string' },
          port: { type: 'number' },
          netsio_port: { type: 'number' },
          web_url: { type: 'string' },
          timeout_ms: { type: 'number', default: 5000 },
        },
      },
    },
    {
      name: 'fujinet_stop',
      description: 'Stop only the MCP-owned FujiNet-PC sidecar process.',
      inputSchema: { type: 'object', properties: { force: { type: 'boolean', default: false } } },
    },
    { name: 'fujinet_status', description: 'Report FujiNet-PC sidecar selection, process, port, paths, and log status.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'fujinet_logs',
      description: 'Read bounded FujiNet-PC stdout/stderr logs.',
      inputSchema: {
        type: 'object',
        properties: {
          since_seq: { type: 'number' },
          limit: { type: 'number', default: 100 },
          contains: { type: 'string' },
          regex: { type: 'string' },
          stream: { type: 'string', enum: ['stdout', 'stderr', 'mcp', 'both'], default: 'both' },
        },
      },
    },
    {
      name: 'fujinet_debug_read',
      description: 'Read bounded FujiNet-PC debug output from stdout/stderr.',
      inputSchema: {
        type: 'object',
        properties: {
          since_seq: { type: 'number' },
          limit: { type: 'number', default: 100 },
          contains: { type: 'string' },
          regex: { type: 'string' },
          stream: { type: 'string', enum: ['stdout', 'stderr', 'mcp', 'both'], default: 'both' },
        },
      },
    },
    { name: 'fujinet_debug_clear', description: 'Clear the MCP FujiNet-PC debug output buffer.', inputSchema: { type: 'object', properties: {} } },
    { name: 'fujinet_debug_status', description: 'Report FujiNet-PC debug output buffer counters.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_start',
      description: 'Start an MCP-owned Atari800 emulator session.',
      inputSchema: {
        type: 'object',
        properties: {
          program: { type: 'string', description: 'Optional program path to run with -run' },
          machine: { type: 'string', enum: ['atari', 'xl', 'xe', 'xegs', '5200'], default: 'xl' },
          ram: { type: 'number', description: 'Optional RAM profile: 48, 64, 128, 320, 576, or 1088' },
          basic: { type: 'boolean' },
          netsio: { type: 'boolean', default: false },
          netsio_port: { type: 'number' },
          debug_port: { type: 'number' },
          turbo: { type: 'boolean', default: false },
          sound: { type: 'boolean' },
          display_mode: { type: 'string', enum: ['auto', 'headless', 'visible'], default: 'auto' },
          xvfb_display: { type: 'number' },
          xvfb_screen: { type: 'string', default: '1024x768x24' },
          args: { type: 'array', items: { type: 'string' } },
          unsafe_override: { type: 'boolean', default: false },
          disks: { type: 'array', items: { type: 'string' } },
          artifact_dir: { type: 'string' },
        },
      },
    },
    {
      name: 'atari_stop',
      description: 'Stop the MCP-owned Atari800 emulator session.',
      inputSchema: {
        type: 'object',
        properties: {
          force: { type: 'boolean', default: false },
          cleanup_runtime_dir: { type: 'boolean', default: true },
        },
      },
    },
    {
      name: 'atari_status',
      description: 'Report MCP session state, launch details, sockets, and bounded logs.',
      inputSchema: { type: 'object', properties: {} },
    },
    {
      name: 'atari_logs',
      description: 'Read bounded emulator/Xvfb startup logs.',
      inputSchema: {
        type: 'object',
        properties: {
          since_seq: { type: 'number' },
          limit: { type: 'number', default: 100 },
          contains: { type: 'string' },
          stream: { type: 'string' },
        },
      },
    },
    {
      name: 'atari_run',
      description: 'Run the emulator for N frames.',
      inputSchema: {
        type: 'object',
        properties: { frames: { type: 'number', default: 60 } },
      },
    },
    {
      name: 'atari_frame_step',
      description: 'Run N frame-loop ticks, then pause.',
      inputSchema: {
        type: 'object',
        properties: { frames: { type: 'number', default: 1 } },
      },
    },
    { name: 'atari_pause', description: 'Pause emulation.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_load',
      description: 'Load an executable-style program through the Atari800 BIN loader.',
      inputSchema: {
        type: 'object',
        properties: { path: { type: 'string' } },
        required: ['path'],
      },
    },
    { name: 'atari_screen', description: 'Get the current screen as ASCII art.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_screen_raw', description: 'Get the rendered framebuffer as base64 data.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_screenshot',
      description: 'Save a screenshot. Omit path to use the managed artifact directory.',
      inputSchema: {
        type: 'object',
        properties: { path: { type: 'string' } },
      },
    },
    {
      name: 'atari_joystick',
      description: 'Set joystick state.',
      inputSchema: {
        type: 'object',
        properties: {
          direction: { type: 'string', enum: ['center', 'up', 'down', 'left', 'right', 'ul', 'ur', 'll', 'lr'], default: 'center' },
          fire: { type: 'boolean', default: false },
          port: { type: 'number', default: 0 },
        },
      },
    },
    {
      name: 'atari_key',
      description: 'Press a key on the Atari keyboard.',
      inputSchema: {
        type: 'object',
        properties: { key: { type: 'string' } },
        required: ['key'],
      },
    },
    { name: 'atari_key_release', description: 'Release all AI key state.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_paddle',
      description: 'Set paddle position.',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'number', default: 0 },
          value: { type: 'number', default: 128 },
        },
      },
    },
    {
      name: 'atari_consol',
      description: 'Press console keys.',
      inputSchema: {
        type: 'object',
        properties: {
          start: { type: 'boolean', default: false },
          select: { type: 'boolean', default: false },
          option: { type: 'boolean', default: false },
        },
      },
    },
    {
      name: 'atari_peek',
      description: 'Read memory from the Atari.',
      inputSchema: {
        type: 'object',
        properties: {
          address: { type: 'number' },
          length: { type: 'number', default: 1 },
        },
        required: ['address'],
      },
    },
    {
      name: 'atari_poke',
      description: 'Write to Atari memory.',
      inputSchema: {
        type: 'object',
        properties: {
          address: { type: 'number' },
          values: { type: 'array', items: { type: 'number' } },
        },
        required: ['address', 'values'],
      },
    },
    { name: 'atari_cpu', description: 'Get CPU state.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_cpu_set',
      description: 'Unsafe: set selected CPU registers.',
      inputSchema: {
        type: 'object',
        properties: {
          pc: { type: 'number' },
          a: { type: 'number' },
          x: { type: 'number' },
          y: { type: 'number' },
          sp: { type: 'number' },
        },
      },
    },
    { name: 'atari_gtia', description: 'Get GTIA chip state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_pokey', description: 'Get POKEY chip state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_antic', description: 'Get ANTIC chip state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_pia', description: 'Get PIA chip state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_reset', description: 'Cold reset the Atari.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_dump_memory',
      description: 'Dump a memory range to an artifact-safe path.',
      inputSchema: {
        type: 'object',
        properties: {
          start: { type: 'number' },
          end: { type: 'number' },
          path: { type: 'string' },
        },
        required: ['start', 'end', 'path'],
      },
    },
    {
      name: 'atari_debug_enable',
      description: 'Enable debug output capture at an address.',
      inputSchema: {
        type: 'object',
        properties: { addr: { type: 'number', default: 0xd7ff } },
      },
    },
    { name: 'atari_debug_read', description: 'Read and clear debug output.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_debugger_status', description: 'Report debugger capabilities and stop state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_show_state', description: 'Show debugger CPU/stop state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_history', description: 'Show recent executed instruction history.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_jumps', description: 'Show recent JMP/JSR history.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_stack',
      description: 'Show 6502 stack bytes above SP.',
      inputSchema: { type: 'object', properties: { count: { type: 'number', default: 16 } } },
    },
    {
      name: 'atari_disassemble',
      description: 'Disassemble memory using monitor formatting.',
      inputSchema: {
        type: 'object',
        properties: { addr: { type: 'number' }, count: { type: 'number', default: 24 } },
      },
    },
    {
      name: 'atari_disassemble_loop',
      description: 'Disassemble the loop containing an address when detectable.',
      inputSchema: { type: 'object', properties: { addr: { type: 'number' } } },
    },
    {
      name: 'atari_display_list',
      description: 'Show ANTIC display list entries.',
      inputSchema: {
        type: 'object',
        properties: { addr: { type: 'number' }, count: { type: 'number', default: 64 } },
      },
    },
    {
      name: 'atari_memory_search',
      description: 'Search memory for a byte pattern.',
      inputSchema: {
        type: 'object',
        properties: {
          start: { type: 'number' },
          end: { type: 'number' },
          pattern: { type: 'array', items: { type: 'number' } },
        },
        required: ['start', 'end', 'pattern'],
      },
    },
    {
      name: 'atari_string_search',
      description: 'Search memory for an ATASCII string.',
      inputSchema: {
        type: 'object',
        properties: { start: { type: 'number' }, end: { type: 'number' }, text: { type: 'string' } },
        required: ['start', 'end', 'text'],
      },
    },
    {
      name: 'atari_screencode_string_search',
      description: 'Search memory for an ANTIC screen-code string.',
      inputSchema: {
        type: 'object',
        properties: { start: { type: 'number' }, end: { type: 'number' }, text: { type: 'string' } },
        required: ['start', 'end', 'text'],
      },
    },
    {
      name: 'atari_labels',
      description: 'List monitor labels when MONITOR_HINTS is available.',
      inputSchema: { type: 'object', properties: { limit: { type: 'number', default: 256 } } },
    },
    {
      name: 'atari_step_instruction',
      description: 'True CPU instruction stepping using monitor break-step support.',
      inputSchema: {
        type: 'object',
        properties: { instructions: { type: 'number', default: 1 } },
      },
    },
    { name: 'atari_debugger_continue', description: 'Continue emulation from a debugger stop.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_break_on_pc',
      description: 'Enable or disable simple break-on-PC.',
      inputSchema: {
        type: 'object',
        properties: {
          addr: { type: 'number' },
          enabled: { type: 'boolean', default: true },
        },
      },
    },
    {
      name: 'atari_break_on_brk',
      description: 'Enable or disable break-on-BRK.',
      inputSchema: {
        type: 'object',
        properties: { enabled: { type: 'boolean', default: true } },
      },
    },
    { name: 'atari_breakpoint_status', description: 'Report simple AI breakpoint state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_breakpoint_list', description: 'List rich monitor breakpoint table entries.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_breakpoint_add',
      description: 'Add a rich monitor breakpoint table entry.',
      inputSchema: {
        type: 'object',
        properties: {
          condition: { type: 'string' },
          condition_type: { type: 'string', enum: ['PC', 'A', 'X', 'Y', 'S', 'READ', 'WRITE', 'ACCESS', 'MEM'] },
          operator: { type: 'string', enum: ['=', '==', '!=', '<', '<=', '>', '>='], default: '=' },
          value: { type: 'number' },
          m_addr: { type: 'number' },
        },
      },
    },
    {
      name: 'atari_breakpoint_delete',
      description: 'Delete a rich monitor breakpoint by slot.',
      inputSchema: { type: 'object', properties: { slot: { type: 'number' } }, required: ['slot'] },
    },
    {
      name: 'atari_breakpoint_enable',
      description: 'Enable a rich monitor breakpoint by slot.',
      inputSchema: { type: 'object', properties: { slot: { type: 'number' } }, required: ['slot'] },
    },
    {
      name: 'atari_breakpoint_disable',
      description: 'Disable a rich monitor breakpoint by slot.',
      inputSchema: { type: 'object', properties: { slot: { type: 'number' } }, required: ['slot'] },
    },
    {
      name: 'atari_breakpoint_clear',
      description: 'Clear AI breakpoints.',
      inputSchema: {
        type: 'object',
        properties: { type: { type: 'string', enum: ['all', 'pc', 'brk', 'table', 'rich'], default: 'all' } },
      },
    },
    { name: 'atari_video_status', description: 'Get video socket and stream state.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_video_enable_push', description: 'Enable video push streaming.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_video_disable_push', description: 'Disable video push streaming.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_video_enable_pull', description: 'Enable video pull requests.', inputSchema: { type: 'object', properties: {} } },
    { name: 'atari_video_disable_pull', description: 'Disable video pull requests.', inputSchema: { type: 'object', properties: {} } },
    {
      name: 'atari_video_set_fps_cap',
      description: 'Set push stream max FPS; 0 is uncapped.',
      inputSchema: {
        type: 'object',
        properties: { value: { type: 'number' } },
        required: ['value'],
      },
    },
    {
      name: 'atari_video_set_frameskip',
      description: 'Send one push frame every N emulator frames.',
      inputSchema: {
        type: 'object',
        properties: { n: { type: 'number' } },
        required: ['n'],
      },
    },
    {
      name: 'atari_video_set_change_triggered',
      description: 'Only push frames when the frame CRC changes.',
      inputSchema: {
        type: 'object',
        properties: { enabled: { type: 'boolean' } },
        required: ['enabled'],
      },
    },
    {
      name: 'atari_save_state',
      description: 'Save emulator state to a file.',
      inputSchema: {
        type: 'object',
        properties: { path: { type: 'string' } },
        required: ['path'],
      },
    },
    {
      name: 'atari_load_state',
      description: 'Load emulator state from a file.',
      inputSchema: {
        type: 'object',
        properties: { path: { type: 'string' } },
        required: ['path'],
      },
    },
  ],
}));

const KEY_CODES = {
  a: 63, b: 21, c: 18, d: 58, e: 42, f: 56, g: 61, h: 57,
  i: 13, j: 1, k: 5, l: 0, m: 37, n: 35, o: 8, p: 10,
  q: 47, r: 40, s: 62, t: 45, u: 11, v: 16, w: 46, x: 22,
  y: 43, z: 23,
  0: 50, 1: 31, 2: 30, 3: 26, 4: 24, 5: 29, 6: 27, 7: 51, 8: 53, 9: 48,
  space: 33, ' ': 33, return: 12, enter: 12, escape: 28, esc: 28,
  tab: 44, backspace: 52,
};

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args = {} } = request.params;

  try {
    switch (name) {
      case 'atari_preflight': {
        const preflight = await buildPreflight();
        return { content: [{ type: 'text', text: formatJson(preflight) }] };
      }

      case 'fujinet_list_versions': {
        const result = currentFujiNetSelection();
        if (args.include_remote === true) result.remote_assets = await listRemoteFujiNetAssets();
        return { content: [{ type: 'text', text: formatJson(result) }] };
      }

      case 'fujinet_set_local_path': {
        const selectedPath = path.resolve(args.path);
        if (!fs.existsSync(selectedPath)) {
          throw makeError('BAD_ARGUMENT', 'FujiNet-PC local path does not exist', { path: selectedPath });
        }
        fujinetSelection = { local_path: selectedPath, version: path.basename(selectedPath), source: 'local_path' };
        return { content: [{ type: 'text', text: formatJson(currentFujiNetSelection()) }] };
      }

      case 'fujinet_select_version': {
        const selected = selectLocalFujiNetByVersion(args.version);
        return { content: [{ type: 'text', text: formatJson(selected) }] };
      }

      case 'fujinet_fetch_latest': {
        const fetched = await fetchFujiNetAsset(args);
        return { content: [{ type: 'text', text: formatJson(fetched) }] };
      }

      case 'fujinet_start': {
        await startFujiNetSidecar(args);
        return { content: [{ type: 'text', text: formatToolResponse('FujiNet-PC sidecar started.', fujinetStatusSnapshot()) }] };
      }

      case 'fujinet_stop': {
        const stopped = await stopFujiNet({ force: args.force === true });
        return { content: [{ type: 'text', text: formatJson(stopped) }] };
      }

      case 'fujinet_status': {
        return { content: [{ type: 'text', text: formatJson(fujinetStatusSnapshot()) }] };
      }

      case 'fujinet_logs':
      case 'fujinet_debug_read': {
        return { content: [{ type: 'text', text: formatJson(readFujiNetLogs(args)) }] };
      }

      case 'fujinet_debug_clear': {
        if (session?.fujinet) {
          session.fujinet.logs = [];
          session.fujinet.dropped_logs = 0;
        }
        return { content: [{ type: 'text', text: formatJson({ status: 'ok', debug: fujinetStatusSnapshot().fujinet }) }] };
      }

      case 'fujinet_debug_status': {
        const info = session?.fujinet || null;
        return { content: [{ type: 'text', text: formatJson({
          state: info?.state || 'not_started',
          running: fujinetProcessRunning(),
          last_seq: info?.log_seq || 0,
          next_seq: (info?.log_seq || 0) + 1,
          log_count: info?.logs?.length || 0,
          dropped: info?.dropped_logs || 0,
          limit: FUJINET_LOG_LIMIT,
        }) }] };
      }

      case 'atari_start': {
        if (emulatorProcessRunning()) {
          await stopSession({ force: true, cleanup_runtime_dir: true });
        }
        const preflight = await buildPreflight();
        if (!preflight.emulator.executable) {
          throw makeError('CAPABILITY_UNAVAILABLE', 'Atari800 emulator is not executable', { preflight });
        }
        cleanupStaleRuntimeDirs();
        if (!session) {
          session = createSession(args.program, args.artifact_dir || null);
        } else {
          if (args.artifact_dir && path.resolve(args.artifact_dir) !== session.artifact_dir) {
            throw makeError('BAD_ARGUMENT', 'artifact_dir cannot be changed after starting a FujiNet sidecar; start Atari first or stop the session', {
              requested: path.resolve(args.artifact_dir),
              current: session.artifact_dir,
            });
          }
          session.state = 'starting';
          session.emulator.program = args.program || null;
          session.emulator.started_at = null;
          session.emulator.exit_code = null;
          session.emulator.signal = null;
          session.emulator.netsio_connected = false;
        }

        const requestedMode = args.display_mode || 'auto';
        const displayMode = requestedMode === 'auto' ? 'headless' : requestedMode;
        const xvfbScreen = args.xvfb_screen || '1024x768x24';
        const env = { ...process.env };
        let displayName = env.DISPLAY || null;

        if (displayMode === 'headless') {
          const displayNumber = selectXvfbDisplay(args.xvfb_display);
          displayName = await startXvfbForSession(displayNumber, xvfbScreen);
          env.DISPLAY = displayName;
        } else if (displayMode === 'visible') {
          if (!nativeDisplayAvailable()) {
            throw makeError('MISSING_DISPLAY', 'visible display requested but DISPLAY/WAYLAND_DISPLAY/native desktop is unavailable', {
              hint: 'Use display_mode=headless on Linux with Xvfb installed.',
            });
          }
        } else {
          throw makeError('BAD_ARGUMENT', 'display_mode must be auto, headless, or visible', { display_mode: requestedMode });
        }

        const sound = args.sound !== undefined ? Boolean(args.sound) : displayMode === 'visible';
        const machineArg = resolveMachineArg(args.machine, args.ram);

        const argv = [
          '-ai',
          '-ai-socket', session.emulator.socket,
          '-ai-video-push-socket', session.emulator.video_push_socket,
          '-ai-video-pull-socket', session.emulator.video_pull_socket,
          '-ai-artifact-dir', session.artifact_dir,
          machineArg,
        ];
        if (args.basic === true) argv.push('-basic');
        if (args.basic === false) argv.push('-nobasic');
        const sidecarNetSioPort = fujinetProcessRunning() ? session.fujinet.udp_port : null;
        const netsioEnabled = args.netsio !== undefined ? Boolean(args.netsio) : Boolean(sidecarNetSioPort);
        const netsioPort = args.netsio_port ?? sidecarNetSioPort;
        if (netsioEnabled) {
          argv.push('-netsio');
          if (netsioPort !== null && netsioPort !== undefined) argv.push(String(netsioPort));
        }
        if (args.debug_port !== undefined) argv.push('-ai-debug-port', String(args.debug_port));
        if (args.turbo) argv.push('-turbo');
        if (!sound) argv.push('-nosound');
        if (displayMode === 'headless') argv.push('-no-video-accel');
        argv.push(...filterExtraArgs(args.args || [], args.unsafe_override === true));
        if (args.program) {
          argv.push('-run', args.program);
        }
        if (Array.isArray(args.disks)) {
          for (const disk of args.disks) {
            if (typeof disk !== 'string') {
              throw makeError('BAD_ARGUMENT', 'disks must contain only paths');
            }
            argv.push(disk);
          }
        }

        session.emulator.argv = [EMULATOR_PATH, ...argv];
        session.emulator.env = {
          DISPLAY: env.DISPLAY || null,
          SDL_VIDEODRIVER: env.SDL_VIDEODRIVER || null,
          SDL_AUDIODRIVER: env.SDL_AUDIODRIVER || null,
        };
        session.emulator.display_mode = displayMode;
        session.emulator.display = displayName;
        session.emulator.sound = sound;
        session.emulator.netsio = netsioEnabled;
        session.emulator.netsio_port = netsioEnabled ? (netsioPort ?? 9997) : null;
        session.display = {
          requested_mode: requestedMode,
          effective_mode: displayMode,
          display: displayName,
          xvfb_screen: displayMode === 'headless' ? xvfbScreen : null,
          sound,
        };
        session.emulator.started_at = nowIso();
        session.process = spawn(EMULATOR_PATH, argv, { stdio: ['ignore', 'pipe', 'pipe'], env });
        session.emulator.pid = session.process.pid;
        session.process.stdout.on('data', (chunk) => recordLog('stdout', chunk));
        session.process.stderr.on('data', (chunk) => recordLog('stderr', chunk));
        session.process.on('error', (error) => {
          if (!session) return;
          session.state = 'crashed';
          recordLog('mcp', Buffer.from(`emulator spawn error: ${error.message}`));
        });
        session.process.on('exit', (code, signal) => {
          if (!session) return;
          session.emulator.exit_code = code;
          session.emulator.signal = signal;
          session.emulator.netsio_connected = false;
          if (session.state !== 'cleanup_failed') {
            session.state = code === 0 ? 'exited' : 'crashed';
          }
        });

        const hello = await waitForReady(10000);
        return {
          content: [{
            type: 'text',
            text: `Emulator started\n${formatJson({ session: sessionSnapshot(false), hello })}`,
          }],
        };
      }

      case 'atari_stop': {
        const stopped = await stopSession({
          force: args.force === true,
          cleanup_runtime_dir: args.cleanup_runtime_dir !== false,
        });
        return { content: [{ type: 'text', text: formatJson(stopped) }] };
      }

      case 'atari_status': {
        let hello = null;
        if (session?.state === 'running') {
          try {
            hello = await sendCommand({ cmd: 'hello' });
          } catch (e) {
            hello = { status: 'error', message: e.message };
          }
        }
        return { content: [{ type: 'text', text: formatJson({ session: sessionSnapshot(), hello }) }] };
      }

      case 'atari_logs': {
        const sinceSeq = args.since_seq ?? 0;
        const limit = Math.max(1, Math.min(args.limit ?? 100, LOG_LIMIT));
        let lines = session?.logs || [];
        lines = lines.filter((line) => line.seq > sinceSeq);
        if (args.stream) lines = lines.filter((line) => line.stream === args.stream);
        if (args.contains) lines = lines.filter((line) => line.text.includes(args.contains));
        lines = lines.slice(-limit);
        return {
          content: [{
            type: 'text',
            text: formatJson({
              lines,
              next_seq: lines.length ? lines[lines.length - 1].seq + 1 : sinceSeq,
              dropped: session?.dropped_logs || 0,
              limit: LOG_LIMIT,
            }),
          }],
        };
      }

      case 'atari_run': {
        const frames = args.frames || 60;
        const resp = await sendCommand({ cmd: 'run', frames });
        return { content: [{ type: 'text', text: formatToolResponse(`Ran ${frames} frames.`, resp) }] };
      }

      case 'atari_frame_step': {
        const frames = args.frames || 1;
        const resp = await sendCommand({ cmd: 'frame_step', frames });
        return { content: [{ type: 'text', text: formatToolResponse(`Stepped ${frames} frame-loop ticks.`, resp) }] };
      }

      case 'atari_pause': {
        const resp = await sendCommand({ cmd: 'pause' });
        return { content: [{ type: 'text', text: formatToolResponse('Paused emulator.', resp) }] };
      }

      case 'atari_load': {
        const resp = await sendCommand({ cmd: 'load', path: args.path });
        return { content: [{ type: 'text', text: formatToolResponse(`Load requested: ${args.path}`, resp) }] };
      }

      case 'atari_screen': {
        const resp = await sendCommand({ cmd: 'screen_ascii' });
        return { content: [{ type: 'text', text: `${formatScreen(resp.data)}\n${formatJson(resp)}` }] };
      }

      case 'atari_screen_raw': {
        const resp = await sendCommand({ cmd: 'screen_raw' });
        return { content: [{ type: 'text', text: formatToolResponse('Read rendered framebuffer.', resp) }] };
      }

      case 'atari_screenshot': {
        const cmd = { cmd: 'screenshot' };
        if (args.path) cmd.path = args.path;
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatToolResponse('Screenshot requested.', resp) }] };
      }

      case 'atari_joystick': {
        const resp = await sendCommand({
          cmd: 'joystick',
          port: args.port || 0,
          direction: args.direction || 'center',
          fire: args.fire || false,
        });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_key': {
        const key = args.key.toLowerCase();
        const code = KEY_CODES[key];
        if (code === undefined) {
          return { content: [{ type: 'text', text: `Unknown key: ${args.key}` }], isError: true };
        }
        const resp = await sendCommand({ cmd: 'key', code, shift: false });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_key_release': {
        const resp = await sendCommand({ cmd: 'key_release' });
        return { content: [{ type: 'text', text: formatToolResponse('Released AI key state.', resp) }] };
      }

      case 'atari_paddle': {
        const resp = await sendCommand({
          cmd: 'paddle',
          port: args.port || 0,
          value: args.value ?? 128,
        });
        return { content: [{ type: 'text', text: formatToolResponse(`Paddle ${args.port || 0} set.`, resp) }] };
      }

      case 'atari_consol': {
        const resp = await sendCommand({
          cmd: 'consol',
          start: args.start || false,
          select: args.select || false,
          option: args.option || false,
        });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_peek': {
        const resp = await sendCommand({ cmd: 'peek', addr: args.address, len: args.length || 1 });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_poke': {
        const resp = await sendCommand({ cmd: 'poke', addr: args.address, data: args.values });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_cpu':
      case 'atari_gtia':
      case 'atari_pokey':
      case 'atari_antic':
      case 'atari_pia': {
        const resp = await sendCommand({ cmd: name.replace('atari_', '') });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_cpu_set': {
        const cmd = { cmd: 'cpu_set' };
        for (const field of ['pc', 'a', 'x', 'y', 'sp']) {
          if (args[field] !== undefined) cmd[field] = args[field];
        }
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatToolResponse('CPU register update requested.', resp) }] };
      }

      case 'atari_reset': {
        const resp = await sendCommand({ cmd: 'reset' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_dump_memory': {
        const resp = await sendCommand({
          cmd: 'dump',
          start: args.start,
          end: args.end,
          path: args.path,
        });
        return { content: [{ type: 'text', text: formatToolResponse(`Memory dump requested: ${args.path}`, resp) }] };
      }

      case 'atari_debug_enable': {
        const resp = await sendCommand({ cmd: 'debug_enable', addr: args.addr ?? 0xd7ff });
        return { content: [{ type: 'text', text: formatToolResponse('Debug capture enabled.', resp) }] };
      }

      case 'atari_debug_read': {
        const resp = await sendCommand({ cmd: 'debug_read' });
        return { content: [{ type: 'text', text: formatToolResponse('Debug output read and cleared.', resp) }] };
      }

      case 'atari_debugger_status': {
        const resp = await sendCommand({ cmd: 'debugger.status' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_show_state': {
        const resp = await sendCommand({ cmd: 'debugger.show_state' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_history': {
        const resp = await sendCommand({ cmd: 'debugger.history' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_jumps': {
        const resp = await sendCommand({ cmd: 'debugger.jumps' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_stack': {
        const resp = await sendCommand({ cmd: 'debugger.stack', count: args.count ?? 16 });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_disassemble': {
        const cmd = { cmd: 'debugger.disassemble', count: args.count ?? 24 };
        if (args.addr !== undefined) cmd.addr = args.addr;
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_disassemble_loop': {
        const cmd = { cmd: 'debugger.disassemble_loop' };
        if (args.addr !== undefined) cmd.addr = args.addr;
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_display_list': {
        const cmd = { cmd: 'debugger.dlist', count: args.count ?? 64 };
        if (args.addr !== undefined) cmd.addr = args.addr;
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_memory_search': {
        const resp = await sendCommand({
          cmd: 'debugger.search_memory',
          start: args.start,
          end: args.end,
          pattern: args.pattern,
        });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_string_search': {
        const resp = await sendCommand({
          cmd: 'debugger.search_string',
          start: args.start,
          end: args.end,
          text: args.text,
        });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_screencode_string_search': {
        const resp = await sendCommand({
          cmd: 'debugger.search_screencode_string',
          start: args.start,
          end: args.end,
          text: args.text,
        });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_labels': {
        const resp = await sendCommand({ cmd: 'debugger.labels', limit: args.limit ?? 256 });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_step_instruction': {
        const instructions = args.instructions || 1;
        const resp = await sendCommand({ cmd: 'debugger.step_instruction', instructions });
        return { content: [{ type: 'text', text: formatToolResponse(`Stepped ${instructions} CPU instructions.`, resp) }] };
      }

      case 'atari_debugger_continue': {
        const resp = await sendCommand({ cmd: 'debugger.continue' });
        return { content: [{ type: 'text', text: formatToolResponse('Debugger continue requested.', resp) }] };
      }

      case 'atari_break_on_pc': {
        const cmd = { cmd: 'breakpoint.pc', enabled: args.enabled !== false };
        if (args.addr !== undefined) cmd.addr = args.addr;
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatToolResponse('PC breakpoint updated.', resp) }] };
      }

      case 'atari_break_on_brk': {
        const resp = await sendCommand({ cmd: 'breakpoint.brk', enabled: args.enabled !== false });
        return { content: [{ type: 'text', text: formatToolResponse('BRK breakpoint updated.', resp) }] };
      }

      case 'atari_breakpoint_status': {
        const resp = await sendCommand({ cmd: 'breakpoint.status' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_breakpoint_list': {
        const resp = await sendCommand({ cmd: 'breakpoint.list' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_breakpoint_add': {
        const cmd = { cmd: 'breakpoint.add' };
        for (const key of ['condition', 'condition_type', 'operator', 'value', 'm_addr']) {
          if (args[key] !== undefined) cmd[key] = args[key];
        }
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_breakpoint_delete': {
        const resp = await sendCommand({ cmd: 'breakpoint.delete', slot: args.slot });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_breakpoint_enable': {
        const resp = await sendCommand({ cmd: 'breakpoint.enable', slot: args.slot });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_breakpoint_disable': {
        const resp = await sendCommand({ cmd: 'breakpoint.disable', slot: args.slot });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_breakpoint_clear': {
        const cmd = { cmd: 'breakpoint.clear' };
        if (args.type) cmd.type = args.type;
        const resp = await sendCommand(cmd);
        return { content: [{ type: 'text', text: formatToolResponse('Breakpoint clear requested.', resp) }] };
      }

      case 'atari_video_status': {
        const resp = await sendCommand({ cmd: 'video.status' });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_video_enable_push':
      case 'atari_video_disable_push':
      case 'atari_video_enable_pull':
      case 'atari_video_disable_pull': {
        const map = {
          atari_video_enable_push: 'video.enable_push',
          atari_video_disable_push: 'video.disable_push',
          atari_video_enable_pull: 'video.enable_pull',
          atari_video_disable_pull: 'video.disable_pull',
        };
        const resp = await sendCommand({ cmd: map[name] });
        return { content: [{ type: 'text', text: formatToolResponse(`${map[name]} requested.`, resp) }] };
      }

      case 'atari_video_set_fps_cap': {
        const resp = await sendCommand({ cmd: 'video.push.set_fps_cap', value: args.value });
        return { content: [{ type: 'text', text: formatToolResponse('Video FPS cap updated.', resp) }] };
      }

      case 'atari_video_set_frameskip': {
        const resp = await sendCommand({ cmd: 'video.push.set_frameskip', n: args.n });
        return { content: [{ type: 'text', text: formatToolResponse('Video frameskip updated.', resp) }] };
      }

      case 'atari_video_set_change_triggered': {
        const resp = await sendCommand({ cmd: 'video.push.enable_change_triggered', enabled: args.enabled });
        return { content: [{ type: 'text', text: formatToolResponse('Video change-trigger mode updated.', resp) }] };
      }

      case 'atari_save_state': {
        const resp = await sendCommand({ cmd: 'save_state', path: args.path });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      case 'atari_load_state': {
        const resp = await sendCommand({ cmd: 'load_state', path: args.path });
        return { content: [{ type: 'text', text: formatJson(resp) }] };
      }

      default:
        return { content: [{ type: 'text', text: `Unknown tool: ${name}` }], isError: true };
    }
  } catch (error) {
    const code = error.code || 'INTERNAL_ERROR';
    const details = error.details || {};
    return {
      content: [{
        type: 'text',
        text: formatJson({
          status: 'error',
          code,
          message: error.message,
          details,
          session: sessionSnapshot(),
        }),
      }],
      isError: true,
    };
  }
});

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error('Atari 800 MCP Server running');
}

main().catch(console.error);
