#!/usr/bin/env python3
"""Check the AI/MCP inventory in AGENT_CONTRACT.md against source."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "AGENT_CONTRACT.md"
AI_C = ROOT / "src" / "ai_interface.c"
MCP_JS = ROOT / "mcp-server" / "index.js"


def extract_section(text, begin, end):
    start = text.find(begin)
    stop = text.find(end)
    if start < 0 or stop < 0 or stop <= start:
        raise RuntimeError(f"missing inventory markers {begin} / {end}")
    return text[start:stop]


def extract_table_names(section):
    names = set()
    for line in section.splitlines():
        match = re.match(r"\|\s*`([^`]+)`\s*\|", line)
        if match and match.group(1) not in {"Command", "Tool"}:
            names.add(match.group(1))
    return names


def c_commands():
    text = AI_C.read_text(encoding="utf-8")
    return set(re.findall(r'strcmp\(cmd_type,\s*"([^"]+)"\)', text))


def hello_metadata_commands():
    text = AI_C.read_text(encoding="utf-8")
    commands_start = text.find(r'\"commands\":[')
    classes_start = text.find(r'\"command_classes\"', commands_start)
    send_end = text.find("AI_SendResponse(ai_response)", classes_start)
    if commands_start < 0 or classes_start < 0 or send_end < 0:
        raise RuntimeError("could not locate hello command metadata")

    commands_block = text[commands_start:classes_start]
    classes_block = text[classes_start:send_end]
    commands = set(re.findall(r'\\"([^"]+)\\"', commands_block))
    classes = set(re.findall(r'\\"([^"]+)\\"', classes_block))
    commands.discard("commands")
    for label in ("command_classes", "read_only", "mutating", "unsafe"):
        classes.discard(label)
    return commands, classes


def mcp_tools():
    text = MCP_JS.read_text(encoding="utf-8")
    return set(name for name in re.findall(r"name:\s*'([^']+)'", text)
               if name.startswith("atari_"))


def main():
    contract = CONTRACT.read_text(encoding="utf-8")
    documented_c = extract_table_names(extract_section(
        contract,
        "<!-- BEGIN AI_COMMAND_INVENTORY -->",
        "<!-- END AI_COMMAND_INVENTORY -->",
    ))
    documented_mcp = extract_table_names(extract_section(
        contract,
        "<!-- BEGIN MCP_TOOL_INVENTORY -->",
        "<!-- END MCP_TOOL_INVENTORY -->",
    ))

    actual_c = c_commands()
    actual_mcp = mcp_tools()
    hello_commands, hello_classes = hello_metadata_commands()
    errors = []

    if documented_c != actual_c:
        errors.append("C command inventory mismatch")
        errors.append(f"  missing from doc: {sorted(actual_c - documented_c)}")
        errors.append(f"  missing from source: {sorted(documented_c - actual_c)}")

    if documented_mcp != actual_mcp:
        errors.append("MCP tool inventory mismatch")
        errors.append(f"  missing from doc: {sorted(actual_mcp - documented_mcp)}")
        errors.append(f"  missing from source: {sorted(documented_mcp - actual_mcp)}")

    if hello_commands != actual_c:
        errors.append("hello commands list mismatch")
        errors.append(f"  missing from hello: {sorted(actual_c - hello_commands)}")
        errors.append(f"  missing from dispatch: {sorted(hello_commands - actual_c)}")

    if hello_classes != actual_c:
        errors.append("hello command_classes mismatch")
        errors.append(f"  unclassified dispatch commands: {sorted(actual_c - hello_classes)}")
        errors.append(f"  classified but not dispatched: {sorted(hello_classes - actual_c)}")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(f"AI inventory ok: {len(actual_c)} C commands, {len(actual_mcp)} MCP tools")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
