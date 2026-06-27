import fs from 'fs';
import path from 'path';

function findSection(config, name) {
  const wanted = String(name).toLowerCase();
  return config.sections.find((section) => section.name.toLowerCase() === wanted) || null;
}

function findEntry(section, key) {
  const wanted = String(key).toLowerCase();
  return section?.entries.find((entry) => entry.type === 'value' && entry.key.toLowerCase() === wanted) || null;
}

export function parseIni(text) {
  const config = { preamble: [], sections: [] };
  let current = null;

  for (const line of String(text).replace(/\r\n/g, '\n').split('\n')) {
    const sectionMatch = /^\s*\[([^\]\r\n]+)\]\s*$/.exec(line);
    if (sectionMatch) {
      current = { name: sectionMatch[1].trim(), entries: [] };
      config.sections.push(current);
      continue;
    }

    const valueMatch = /^\s*([^=;#][^=]*)=(.*)$/.exec(line);
    if (current && valueMatch) {
      current.entries.push({
        type: 'value',
        key: valueMatch[1].trim(),
        value: valueMatch[2],
      });
      continue;
    }

    const raw = { type: 'raw', text: line };
    if (current) current.entries.push(raw);
    else config.preamble.push(raw);
  }

  return config;
}

export function serializeIni(config) {
  const lines = config.preamble.map((entry) => entry.text);
  for (const section of config.sections) {
    if (lines.length && lines[lines.length - 1] !== '') lines.push('');
    lines.push(`[${section.name}]`);
    for (const entry of section.entries) {
      lines.push(entry.type === 'value' ? `${entry.key}=${entry.value}` : entry.text);
    }
  }
  while (lines.length > 1 && lines[lines.length - 1] === '') lines.pop();
  return `${lines.join('\n')}\n`;
}

export function getIniValue(config, sectionName, key) {
  return findEntry(findSection(config, sectionName), key)?.value ?? null;
}

export function setIniValue(config, sectionName, key, value) {
  if (!/^[^\[\]\r\n=]+$/.test(String(sectionName)) || !/^[^\r\n=]+$/.test(String(key))) {
    throw new Error('INI section and key names may not contain brackets, equals signs, or newlines');
  }
  if (/[\r\n]/.test(String(value))) {
    throw new Error('INI values may not contain newlines');
  }

  let section = findSection(config, sectionName);
  if (!section) {
    section = { name: String(sectionName), entries: [] };
    config.sections.push(section);
  }
  const entry = findEntry(section, key);
  if (entry) entry.value = String(value);
  else section.entries.push({ type: 'value', key: String(key), value: String(value) });
  return config;
}

export function removeIniSection(config, sectionName) {
  const wanted = String(sectionName).toLowerCase();
  const index = config.sections.findIndex((section) => section.name.toLowerCase() === wanted);
  if (index >= 0) config.sections.splice(index, 1);
  return index >= 0;
}

export function iniToObject(config) {
  const result = {};
  for (const section of config.sections) {
    const values = {};
    for (const entry of section.entries) {
      if (entry.type === 'value') values[entry.key] = entry.value;
    }
    result[section.name] = values;
  }
  return result;
}

export function writeIniAtomic(configPath, config) {
  const dir = path.dirname(configPath);
  const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '');
  let backupPath = fs.existsSync(configPath) ? `${configPath}.backup-${stamp}` : null;
  for (let suffix = 1; backupPath && fs.existsSync(backupPath); suffix += 1) {
    backupPath = `${configPath}.backup-${stamp}-${suffix}`;
  }
  const tempPath = path.join(dir, `.${path.basename(configPath)}.tmp-${process.pid}-${Math.random().toString(16).slice(2)}`);

  if (backupPath) fs.copyFileSync(configPath, backupPath, fs.constants.COPYFILE_EXCL);
  try {
    fs.writeFileSync(tempPath, serializeIni(config), { mode: 0o600 });
    fs.renameSync(tempPath, configPath);
  } catch (error) {
    try { fs.unlinkSync(tempPath); } catch {}
    throw error;
  }

  return { config_path: configPath, backup_path: backupPath };
}
