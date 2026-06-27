/*
 * ai_interface.h - AI/Automation Interface for Atari800
 *
 * Provides a socket-based JSON API for autonomous control of the emulator.
 * Enables AI agents to:
 * - Load and run programs
 * - Inject keyboard, joystick, and paddle input
 * - Read screen output (PNG and ASCII)
 * - Access CPU state and memory
 * - Read all chip registers (ANTIC, GTIA, POKEY, PIA)
 *
 * Copyright (c) 2026 - AI Interface Extension
 * Licensed under GPL-2.0-or-later
 */

#ifndef AI_INTERFACE_H_
#define AI_INTERFACE_H_

#include "atari.h"

/* Configuration */
#define AI_SOCKET_PATH "/tmp/atari800_ai.sock"
#define AI_BUFFER_SIZE 65536
#define AI_MAX_RESPONSE 1048576  /* 1MB max response */

/* Initialize AI interface - call from main() */
int AI_Initialise(int *argc, char *argv[]);

/* Cleanup AI interface */
void AI_Exit(void);

/* Process pending AI commands - call each frame */
void AI_Frame(void);

/* Check if AI is controlling (paused waiting for command) */
int AI_IsPaused(void);

/* JSON API Commands:
 *
 * All commands are JSON objects with a "cmd" field.
 * Successful responses are JSON objects with "status":"ok" and data.
 * Error responses use {"status":"error","code":"...","message":"...","details":{...}}.
 *
 * === PROTOCOL ===
 * {"cmd": "hello"}
 * {"cmd": "capabilities"}
 *   -> protocol version, build flags, limits, sockets, command list, and command classes
 *
 * === CONTROL ===
 * {"cmd": "ping"}
 *   -> {"status": "ok", "msg": "pong"}
 *
 * {"cmd": "load", "path": "/path/to/program.xex"}
 *   -> {"status": "ok"} or {"status": "error", "msg": "..."}
 *
 * {"cmd": "run", "frames": 60}
 *   Run for N frames (default 1), then pause and respond
 *   -> {"status": "ok", "frames_run": 60}
 *
 * {"cmd": "frame_step", "frames": 1}
 *   Run N frame-loop ticks (default 1), then pause. This is not CPU instruction stepping.
 *   -> {"status": "ok", "steps_run": 1, "pc": 0x1234}
 *
 * {"cmd": "step", "instructions": 1}
 *   Deprecated compatibility alias for frame-loop stepping, not CPU instruction stepping.
 *   -> {"status": "ok", "steps_run": 1, "pc": 0x1234}
 *
 * {"cmd": "pause"}
 *   -> {"status": "ok"}
 *
 * {"cmd": "reset"}
 *   Cold reset the machine
 *   -> {"status": "ok"}
 *
 * === INPUT ===
 * {"cmd": "key", "code": 33, "shift": false}
 * {"cmd": "key.down", "code": 33, "shift": false}
 *   Press/hold a key (AKEY_* code)
 *   -> {"status": "ok"}
 *
 * {"cmd": "key_release"}
 * {"cmd": "key.up"}
 *   Release all keys
 *   -> {"status": "ok"}
 *
 * {"cmd": "joystick", "port": 0, "direction": "up", "fire": true}
 *   Set joystick state. direction: "up","down","left","right","center",
 *   "ul","ur","ll","lr"
 *   -> {"status": "ok"}
 *
 * {"cmd": "paddle", "port": 0, "value": 128}
 *   Set paddle position (0-255)
 *   -> {"status": "ok"}
 *
 * {"cmd": "consol", "start": false, "select": false, "option": false}
 *   Set console keys using active-low booleans: false means pressed
 *   -> {"status": "ok"}
 *
 * {"cmd": "input.status"}
 *   Read keyboard, console, joystick override, trigger, and paddle input state
 *   -> {"status": "ok"}
 *
 * === SCREEN ===
 * {"cmd": "screenshot", "format": "png", "path": "/tmp/screen.png"}
 *   Save screenshot to file
 *   -> {"status": "ok", "path": "/tmp/screen.png"}
 *
 * {"cmd": "screen_ascii"}
 *   Get screen as ASCII art (40x24 chars approximation)
 *   -> {"status": "ok", "width": 40, "height": 24, "data": ["line1", ...]}
 *
 * {"cmd": "screen.text"}
 *   Read simple ANTIC/OS text screen memory with confidence and unsupported-mode reporting
 *   -> {"status": "ok", "supported": true, "lines": ["READY", ...]}
 *
 * {"cmd": "screen_raw"}
 * {"cmd": "framebuffer.raw"}
 *   Get rendered screen buffer (base64 encoded, 384x240 bytes)
 *   -> {"status": "ok", "width": 384, "height": 240, "data": "base64..."}
 *
 * === DISK ===
 * {"cmd": "disk.insert", "drive": 1, "path": "/tmp/disk.atr", "read_only": true}
 *   Mount a native disk image in D1..D8. Use read_only=true by default.
 *   -> {"status": "ok", "drives": [{"drive": 1, "state": "read_only", ...}]}
 *
 * {"cmd": "disk.eject", "drive": 1}
 *   Dismount a native disk drive
 *   -> {"status": "ok", "drives": [{"drive": 1, "state": "empty", ...}]}
 *
 * {"cmd": "disk.status", "drive": 0}
 *   Report native disk drive status. drive=0 reports all drives.
 *   -> {"status": "ok", "drives": [...]}
 *
 * === MEMORY ===
 * {"cmd": "peek", "addr": 0x1234, "len": 16}
 *   Read memory (len defaults to 1, maximum 256)
 *   -> {"status": "ok", "addr": 0x1234, "data": [0x00, 0x01, ...]}
 *
 * {"cmd": "poke", "addr": 0x1234, "data": [0x00, 0x01]}
 *   Write memory directly to MEMORY_mem
 *   -> {"status": "ok"}
 *
 * {"cmd": "dump", "start": 0x0000, "end": 0xFFFF, "path": "/tmp/mem.bin"}
 *   Dump memory range to binary file
 *   -> {"status": "ok", "bytes": 65536}
 *
 * === CPU ===
 * {"cmd": "cpu"}
 *   Get CPU state
 *   -> {"status": "ok", "pc": 0x1234, "a": 0x00, "x": 0x00, "y": 0x00,
 *       "sp": 0xFF, "p": 0x00, "n": 0, "v": 0, "b": 0, "d": 0, "i": 0, "z": 0, "c": 0}
 *
 * {"cmd": "cpu_set", "pc": 0x1234, "a": 0x00, ...}
 *   Set CPU registers (only specified ones)
 *   -> {"status": "ok"}
 *
 * === CHIPS ===
 * {"cmd": "antic"}
 *   Get ANTIC state
 *   -> {"status": "ok", "dmactl": 0x00, "chactl": 0x00, "dlist": 0x1234,
 *       "hscrol": 0x00, "vscrol": 0x00, "pmbase": 0x00, "chbase": 0x00,
 *       "nmien": 0x00, "nmist": 0x00, "vcount": 0x00, "ypos": 0, "xpos": 0}
 *
 * {"cmd": "gtia"}
 *   Get GTIA state
 *   -> {"status": "ok", "hposp0": 0x00, ..., "colbk": 0x00, "prior": 0x00, ...}
 *
 * {"cmd": "pokey"}
 *   Get POKEY state
 *   -> {"status": "ok", "audf1": 0x00, ..., "audctl": 0x00, "kbcode": 0x00, ...}
 *
 * {"cmd": "pia"}
 *   Get PIA state
 *   -> {"status": "ok", "porta": 0x00, "portb": 0x00, "pactl": 0x00, "pbctl": 0x00}
 *
 * === STATE ===
 * {"cmd": "save_state", "path": "/tmp/state.sav"}
 *   Save emulator state
 *   -> {"status": "ok"}
 *
 * {"cmd": "load_state", "path": "/tmp/state.sav"}
 *   Load emulator state
 *   -> {"status": "ok"}
 *
 * === DEBUG OUTPUT ===
 * {"cmd": "debug_enable", "addr": 0xD7FF}
 *   Enable debug port - writes to this address will be captured
 *   -> {"status": "ok"}
 *
 * {"cmd": "debug_read"}
 *   Read and clear debug output buffer
 *   -> {"status": "ok", "data": [0x41, 0x42, ...], "ascii": "AB..."}
 *
 * === DEBUGGER ===
 * {"cmd": "debugger.status"}
 * {"cmd": "debugger.show_state"}
 *   Report debugger capabilities, stop state, simple breakpoints, and CPU registers.
 *
 * {"cmd": "debugger.history"}
 * {"cmd": "debugger.jumps"}
 * {"cmd": "debugger.stack", "count": 16}
 * {"cmd": "debugger.disassemble", "addr": 0x2000, "count": 24}
 * {"cmd": "debugger.disassemble_loop", "addr": 0x2000}
 * {"cmd": "debugger.dlist", "addr": 0x9c20, "count": 64}
 * {"cmd": "debugger.search_memory", "start": 0x0000, "end": 0xffff, "pattern": [0, 1]}
 * {"cmd": "debugger.search_string", "start": 0x0000, "end": 0xffff, "text": "READY"}
 * {"cmd": "debugger.search_screencode_string", "start": 0x0000, "end": 0xffff, "text": "READY"}
 * {"cmd": "debugger.labels", "limit": 256}
 *   Read-only monitor-aligned debugger views. Labels require MONITOR_HINTS.
 *
 * {"cmd": "debugger.step_instruction", "instructions": 1}
 *   True CPU instruction stepping through MONITOR_BREAK break-step support.
 *
 * {"cmd": "debugger.continue"}
 *   Continue emulation from a debugger stop.
 *
 * {"cmd": "breakpoint.pc", "addr": 0x1234, "enabled": true}
 * {"cmd": "breakpoint.brk", "enabled": true}
 * {"cmd": "breakpoint.status"}
 * {"cmd": "breakpoint.clear", "type": "all"}
 *   Simple AI-owned PC and BRK breakpoints. AI breakpoint hits pause and report JSON
 *   instead of entering the interactive monitor.
 *
 * {"cmd": "breakpoint.list"}
 * {"cmd": "breakpoint.add", "condition": "PC=2000"}
 * {"cmd": "breakpoint.add", "condition_type": "MEM", "m_addr": 0x0600, "operator": "=", "value": 0x42}
 * {"cmd": "breakpoint.delete", "slot": 0}
 * {"cmd": "breakpoint.enable", "slot": 0}
 * {"cmd": "breakpoint.disable", "slot": 0}
 *   Rich monitor breakpoint table commands require MONITOR_BREAKPOINTS.
 *
 */

/* Exposed for internal use */
void AI_SendResponse(const char *json);
void AI_DebugWrite(UBYTE byte);
void AI_ApplyInput(void);  /* Apply AI input overrides after INPUT_Frame */
int AI_DebuggerShouldBreakPC(UWORD pc);
int AI_DebuggerShouldBreakBRK(void);
int AI_DebuggerBreak(const char *reason, int breakpoint_id);
void AI_FrameStreamSubmitSurface(const void *pixels, int width, int height, int pitch,
                                 int bits_per_pixel, unsigned int rmask,
                                 unsigned int gmask, unsigned int bmask);

/* AI input overrides - set by joystick command, applied after INPUT_Frame */
extern int AI_joy_override[4];     /* -1 = no override, 0-15 = stick value */
extern int AI_trig_override[4];    /* -1 = no override, 0/1 = trigger state */

/* Configuration options */
extern int AI_enabled;
extern int AI_debug_port;  /* Memory address for debug output (0 = disabled) */
extern char AI_socket_path[256];

#endif /* AI_INTERFACE_H_ */
