#!/usr/bin/env python3
"""Live AI debugger smoke test.

Starts Atari800 with a private AI socket, verifies true instruction stepping,
and verifies a PC breakpoint returns JSON instead of entering the interactive
monitor. Requires a built ./src/atari800 with AI interface enabled.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "/tmp/a8-ai-debugger-smoke"


def send(sock_path, cmd, timeout=5):
    data = json.dumps(cmd).encode("utf-8")
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(timeout)
        client.connect(sock_path)
        client.sendall(str(len(data)).encode("ascii") + b"\n" + data)
        header = b""
        while not header.endswith(b"\n"):
            header += client.recv(1)
        expected = int(header)
        body = b""
        while len(body) < expected:
            body += client.recv(expected - len(body))
    return json.loads(body.decode("utf-8"))


def main():
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(os.path.join(BASE, "artifacts"), exist_ok=True)
    sock_path = os.path.join(BASE, "ai.sock")
    emulator = os.path.join(ROOT, "src", "atari800")
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")

    proc = subprocess.Popen([
        emulator,
        "-ai",
        "-ai-socket", sock_path,
        "-ai-video-push-socket", os.path.join(BASE, "push.sock"),
        "-ai-video-pull-socket", os.path.join(BASE, "pull.sock"),
        "-ai-artifact-dir", os.path.join(BASE, "artifacts"),
        "-xl",
        "-nosound",
    ], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)

    try:
        deadline = time.time() + 5
        while time.time() < deadline and not os.path.exists(sock_path):
            time.sleep(0.05)
        if not os.path.exists(sock_path):
            raise RuntimeError("AI socket did not appear")

        status = send(sock_path, {"cmd": "debugger.status"})
        assert status["status"] == "ok"
        assert status["debugger"]["capabilities"]["instruction_step"] is True

        step = send(sock_path, {"cmd": "debugger.step_instruction", "instructions": 1})
        assert step["status"] == "ok"
        assert step["debugger"]["stopped_reason"] == "instruction_limit"

        pc = step["cpu"]["pc"]
        set_break = send(sock_path, {"cmd": "breakpoint.pc", "addr": pc, "enabled": True})
        assert set_break["status"] == "ok"

        hit = send(sock_path, {"cmd": "run", "frames": 5})
        assert hit["status"] == "ok"
        assert hit["debugger"]["stopped_reason"] == "breakpoint_pc"
        assert hit["debugger"]["paused"] is True

        history = send(sock_path, {"cmd": "debugger.history"})
        assert history["status"] == "ok"
        assert history["count"] > 0

        jumps = send(sock_path, {"cmd": "debugger.jumps"})
        assert jumps["status"] == "ok"

        stack = send(sock_path, {"cmd": "debugger.stack", "count": 4})
        assert stack["status"] == "ok"
        assert stack["count"] <= 4

        disasm = send(sock_path, {"cmd": "debugger.disassemble", "addr": pc, "count": 3})
        assert disasm["status"] == "ok"
        assert disasm["count"] == 3

        dlist = send(sock_path, {"cmd": "debugger.dlist", "count": 4})
        assert dlist["status"] == "ok"

        mem_search = send(sock_path, {"cmd": "debugger.search_memory", "start": pc, "end": pc + 2, "pattern": [disasm["lines"][0].split(": ")[1][:2]]})
        assert mem_search["status"] == "error"

        opcode = int(disasm["lines"][0].split(": ")[1][:2], 16)
        mem_search = send(sock_path, {"cmd": "debugger.search_memory", "start": pc, "end": pc + 2, "pattern": [opcode]})
        assert mem_search["status"] == "ok"
        assert pc in mem_search["matches"]

        bp_list = send(sock_path, {"cmd": "breakpoint.list"})
        if bp_list["status"] == "ok":
            added = send(sock_path, {"cmd": "breakpoint.add", "condition": f"PC={pc:04X}"})
            assert added["status"] == "ok"
            assert added["size"] >= 1
            disabled = send(sock_path, {"cmd": "breakpoint.disable", "slot": added["size"] - 1})
            assert disabled["status"] == "ok"
            deleted = send(sock_path, {"cmd": "breakpoint.delete", "slot": added["size"] - 1})
            assert deleted["status"] == "ok"
        else:
            assert bp_list["code"] == "CAPABILITY_UNAVAILABLE"

        print("ai_debugger_smoke: ok")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        shutil.rmtree(BASE, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
