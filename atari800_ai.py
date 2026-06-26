#!/usr/bin/env python3
"""
Atari800 AI Interface Client Library

Provides a Python interface to control the Atari800 emulator via the AI socket.

Usage:
    from atari800_ai import Atari800AI

    with Atari800AI() as atari:
        atari.load("/path/to/program.xex")
        atari.run(frames=60)
        screen = atari.screen_ascii()
        print(screen)
        cpu = atari.cpu()
        print(f"PC: ${cpu['pc']:04X}")
"""

import socket
import json
import time
import base64
from typing import Optional, List, Dict, Any, Union
from contextlib import contextmanager


class Atari800AI:
    """Client for Atari800 AI interface"""

    DEFAULT_SOCKET = "/tmp/atari800_ai.sock"

    # Atari key codes (common ones)
    AKEY_NONE = -1
    AKEY_A = 63
    AKEY_B = 21
    AKEY_C = 18
    AKEY_D = 58
    AKEY_E = 42
    AKEY_F = 56
    AKEY_G = 61
    AKEY_H = 57
    AKEY_I = 13
    AKEY_J = 1
    AKEY_K = 5
    AKEY_L = 0
    AKEY_M = 37
    AKEY_N = 35
    AKEY_O = 8
    AKEY_P = 10
    AKEY_Q = 47
    AKEY_R = 40
    AKEY_S = 62
    AKEY_T = 45
    AKEY_U = 11
    AKEY_V = 16
    AKEY_W = 46
    AKEY_X = 22
    AKEY_Y = 43
    AKEY_Z = 23
    AKEY_0 = 50
    AKEY_1 = 31
    AKEY_2 = 30
    AKEY_3 = 26
    AKEY_4 = 24
    AKEY_5 = 29
    AKEY_6 = 27
    AKEY_7 = 51
    AKEY_8 = 53
    AKEY_9 = 48
    AKEY_SPACE = 33
    AKEY_RETURN = 12
    AKEY_ESCAPE = 28
    AKEY_TAB = 44
    AKEY_BACKSPACE = 52

    def __init__(self, socket_path: str = None):
        self.socket_path = socket_path or self.DEFAULT_SOCKET
        self.sock = None

    def connect(self):
        """Connect to the emulator"""
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.socket_path)
        # Verify connection
        response = self._send({"cmd": "ping"})
        if response.get("status") != "ok":
            raise ConnectionError("Failed to connect to emulator")
        return self

    def disconnect(self):
        """Disconnect from the emulator"""
        if self.sock:
            self.sock.close()
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()

    def _send(self, cmd: dict) -> dict:
        """Send a command and receive response"""
        if not self.sock:
            raise ConnectionError("Not connected to emulator")

        # Send command with length prefix
        data = json.dumps(cmd).encode('utf-8')
        header = f"{len(data)}\n".encode('utf-8')
        self.sock.sendall(header + data)

        # Receive response
        # Read length prefix
        header = b""
        while not header.endswith(b"\n"):
            c = self.sock.recv(1)
            if not c:
                raise ConnectionError("Connection closed")
            header += c

        length = int(header.decode('utf-8').strip())

        # Read response body
        response = b""
        while len(response) < length:
            chunk = self.sock.recv(length - len(response))
            if not chunk:
                raise ConnectionError("Connection closed")
            response += chunk

        return json.loads(response.decode('utf-8'))

    # === Control ===

    def load(self, path: str) -> bool:
        """Load a program (XEX, COM, BAS, etc.)"""
        response = self._send({"cmd": "load", "path": path})
        return response.get("status") == "ok"

    def run(self, frames: int = 1) -> dict:
        """Run emulator for N frames"""
        return self._send({"cmd": "run", "frames": frames})

    def step(self, instructions: int = 1) -> dict:
        """Single-step N CPU instructions"""
        return self._send({"cmd": "step", "instructions": instructions})

    def pause(self) -> bool:
        """Pause emulator"""
        response = self._send({"cmd": "pause"})
        return response.get("status") == "ok"

    def reset(self) -> bool:
        """Cold reset the machine"""
        response = self._send({"cmd": "reset"})
        return response.get("status") == "ok"

    # === Input ===

    def key(self, code: int, shift: bool = False) -> bool:
        """Press a key"""
        response = self._send({"cmd": "key", "code": code, "shift": shift})
        return response.get("status") == "ok"

    def key_release(self) -> bool:
        """Release all keys"""
        response = self._send({"cmd": "key_release"})
        return response.get("status") == "ok"

    def type_char(self, char: str) -> bool:
        """Type a single character"""
        char = char.upper()
        key_map = {
            'A': self.AKEY_A, 'B': self.AKEY_B, 'C': self.AKEY_C, 'D': self.AKEY_D,
            'E': self.AKEY_E, 'F': self.AKEY_F, 'G': self.AKEY_G, 'H': self.AKEY_H,
            'I': self.AKEY_I, 'J': self.AKEY_J, 'K': self.AKEY_K, 'L': self.AKEY_L,
            'M': self.AKEY_M, 'N': self.AKEY_N, 'O': self.AKEY_O, 'P': self.AKEY_P,
            'Q': self.AKEY_Q, 'R': self.AKEY_R, 'S': self.AKEY_S, 'T': self.AKEY_T,
            'U': self.AKEY_U, 'V': self.AKEY_V, 'W': self.AKEY_W, 'X': self.AKEY_X,
            'Y': self.AKEY_Y, 'Z': self.AKEY_Z,
            '0': self.AKEY_0, '1': self.AKEY_1, '2': self.AKEY_2, '3': self.AKEY_3,
            '4': self.AKEY_4, '5': self.AKEY_5, '6': self.AKEY_6, '7': self.AKEY_7,
            '8': self.AKEY_8, '9': self.AKEY_9,
            ' ': self.AKEY_SPACE, '\n': self.AKEY_RETURN,
        }
        code = key_map.get(char)
        if code is not None:
            return self.key(code)
        return False

    def type_string(self, text: str, frame_delay: int = 5) -> bool:
        """Type a string of characters"""
        for char in text:
            if not self.type_char(char):
                continue
            self.run(frames=frame_delay)
            self.key_release()
            self.run(frames=2)
        return True

    def joystick(self, port: int = 0, direction: str = "center", fire: bool = False) -> bool:
        """Set joystick state"""
        response = self._send({
            "cmd": "joystick",
            "port": port,
            "direction": direction,
            "fire": fire
        })
        return response.get("status") == "ok"

    def paddle(self, port: int = 0, value: int = 128) -> bool:
        """Set paddle position (0-228)"""
        response = self._send({"cmd": "paddle", "port": port, "value": value})
        return response.get("status") == "ok"

    def consol(self, start: bool = False, select: bool = False, option: bool = False) -> bool:
        """Press console keys"""
        response = self._send({
            "cmd": "consol",
            "start": start,
            "select": select,
            "option": option
        })
        return response.get("status") == "ok"

    # === Screen ===

    def screenshot(self, path: str = None) -> str:
        """Save screenshot to file, returns path"""
        cmd = {"cmd": "screenshot"}
        if path:
            cmd["path"] = path
        response = self._send(cmd)
        return response.get("path", "")

    def screen_ascii(self) -> List[str]:
        """Get screen as ASCII art (40x24 lines)"""
        response = self._send({"cmd": "screen_ascii"})
        return response.get("data", [])

    def screen_raw(self) -> bytes:
        """Get raw screen buffer (384x240 bytes, Atari color codes)"""
        response = self._send({"cmd": "screen_raw"})
        data = response.get("data", "")
        return base64.b64decode(data) if data else b""

    def print_screen(self):
        """Print screen to console"""
        lines = self.screen_ascii()
        print("+" + "-" * 40 + "+")
        for line in lines:
            print("|" + line + "|")
        print("+" + "-" * 40 + "+")

    # === Memory ===

    def peek(self, addr: int, length: int = 1) -> List[int]:
        """Read memory"""
        response = self._send({"cmd": "peek", "addr": addr, "len": length})
        return response.get("data", [])

    def poke(self, addr: int, data: Union[int, List[int]]) -> bool:
        """Write memory"""
        if isinstance(data, int):
            data = [data]
        response = self._send({"cmd": "poke", "addr": addr, "data": data})
        return response.get("status") == "ok"

    def dump(self, start: int, end: int, path: str) -> int:
        """Dump memory range to file"""
        response = self._send({"cmd": "dump", "start": start, "end": end, "path": path})
        return response.get("bytes", 0)

    # === CPU ===

    def cpu(self) -> dict:
        """Get CPU state"""
        return self._send({"cmd": "cpu"})

    def cpu_set(self, **kwargs) -> bool:
        """Set CPU registers (pc, a, x, y, sp)"""
        cmd = {"cmd": "cpu_set"}
        cmd.update(kwargs)
        response = self._send(cmd)
        return response.get("status") == "ok"

    # === Chips ===

    def antic(self) -> dict:
        """Get ANTIC chip state"""
        return self._send({"cmd": "antic"})

    def gtia(self) -> dict:
        """Get GTIA chip state"""
        return self._send({"cmd": "gtia"})

    def pokey(self) -> dict:
        """Get POKEY chip state"""
        return self._send({"cmd": "pokey"})

    def pia(self) -> dict:
        """Get PIA chip state"""
        return self._send({"cmd": "pia"})

    # === Debug ===

    def debug_enable(self, addr: int = 0xD7FF) -> bool:
        """Enable debug output port at address"""
        response = self._send({"cmd": "debug_enable", "addr": addr})
        return response.get("status") == "ok"

    def debug_read(self) -> tuple:
        """Read and clear debug output buffer, returns (bytes, ascii_string)"""
        response = self._send({"cmd": "debug_read"})
        return (response.get("data", []), response.get("ascii", ""))

    # === State ===

    def save_state(self, path: str) -> bool:
        """Save emulator state"""
        response = self._send({"cmd": "save_state", "path": path})
        return response.get("status") == "ok"

    def load_state(self, path: str) -> bool:
        """Load emulator state"""
        response = self._send({"cmd": "load_state", "path": path})
        return response.get("status") == "ok"


# === Helper functions for common tasks ===

def wait_for_emulator(socket_path: str = None, timeout: float = 10.0) -> Atari800AI:
    """Wait for emulator to be available and return connected client"""
    socket_path = socket_path or Atari800AI.DEFAULT_SOCKET
    start = time.time()
    while time.time() - start < timeout:
        try:
            client = Atari800AI(socket_path)
            client.connect()
            return client
        except (ConnectionRefusedError, FileNotFoundError):
            time.sleep(0.1)
    raise TimeoutError(f"Emulator not available after {timeout}s")


# === Example usage ===

if __name__ == "__main__":
    import sys

    print("Atari800 AI Client")
    print("==================")

    try:
        with Atari800AI() as atari:
            print("Connected to emulator")

            # Get CPU state
            cpu = atari.cpu()
            print(f"CPU: PC=${cpu['pc']:04X} A=${cpu['a']:02X} X=${cpu['x']:02X} Y=${cpu['y']:02X}")

            # Show screen
            print("\nScreen:")
            atari.print_screen()

            # Run for a bit
            print("\nRunning 60 frames...")
            atari.run(frames=60)

            # Show screen again
            print("\nScreen after 60 frames:")
            atari.print_screen()

    except FileNotFoundError:
        print(f"Error: Emulator not running. Start with:")
        print(f"  ./src/atari800 -ai -xl -run your_program.xex")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)
