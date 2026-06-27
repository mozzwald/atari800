#!/usr/bin/env node
import assert from 'assert/strict';
import fs from 'fs';
import os from 'os';
import path from 'path';
import {
  getIniValue,
  iniToObject,
  parseIni,
  removeIniSection,
  serializeIni,
  setIniValue,
  writeIniAtomic,
} from '../mcp-server/fujinet-config.js';

const source = `; retained comment
[General]
boot_mode=0

[Mount1]
hostslot=1
path=/old.atr
mode=r
`;
const config = parseIni(source);
assert.equal(getIniValue(config, 'general', 'BOOT_MODE'), '0');
setIniValue(config, 'General', 'boot_mode', 1);
setIniValue(config, 'Mount2', 'path', '/new.atr');
assert.equal(getIniValue(config, 'GENERAL', 'boot_MODE'), '1');
assert.equal(iniToObject(config).Mount2.path, '/new.atr');
assert.equal(removeIniSection(config, 'mount1'), true);
const serialized = serializeIni(config);
assert.match(serialized, /; retained comment/);
assert.doesNotMatch(serialized, /\[Mount1\]/);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fujinet-config-test-'));
const configPath = path.join(dir, 'fnconfig.ini');
try {
  fs.writeFileSync(configPath, source);
  const result = writeIniAtomic(configPath, config);
  assert.ok(result.backup_path);
  assert.equal(fs.readFileSync(result.backup_path, 'utf8'), source);
  const second = writeIniAtomic(configPath, config);
  assert.notEqual(second.backup_path, result.backup_path);
  assert.equal(getIniValue(parseIni(fs.readFileSync(configPath, 'utf8')), 'General', 'boot_mode'), '1');
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
