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
 * - Control disk drives and other devices
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
 * Responses are JSON objects with "status" ("ok" or "error") and data.
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
 * {"cmd": "step", "instructions": 1}
 *   Single-step N CPU instructions (default 1)
 *   -> {"status": "ok", "pc": 0x1234}
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
 *   Press a key (AKEY_* code)
 *   -> {"status": "ok"}
 *
 * {"cmd": "key_release"}
 *   Release all keys
 *   -> {"status": "ok"}
 *
 * {"cmd": "joystick", "port": 0, "direction": "up", "fire": true}
 *   Set joystick state. direction: "up","down","left","right","center",
 *   "ul","ur","ll","lr"
 *   -> {"status": "ok"}
 *
 * {"cmd": "paddle", "port": 0, "value": 128}
 *   Set paddle position (0-228)
 *   -> {"status": "ok"}
 *
 * {"cmd": "consol", "start": false, "select": false, "option": false}
 *   Set console keys
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
 * {"cmd": "screen_raw"}
 *   Get raw screen buffer (base64 encoded, 384x240 bytes)
 *   -> {"status": "ok", "width": 384, "height": 240, "data": "base64..."}
 *
 * === MEMORY ===
 * {"cmd": "peek", "addr": 0x1234, "len": 16}
 *   Read memory (len defaults to 1)
 *   -> {"status": "ok", "addr": 0x1234, "data": [0x00, 0x01, ...]}
 *
 * {"cmd": "poke", "addr": 0x1234, "data": [0x00, 0x01]}
 *   Write memory
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
 * {"cmd": "breakpoint", "addr": 0x1234, "enabled": true}
 *   Set/clear breakpoint at address
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
 * === DEVICES ===
 * {"cmd": "disk_insert", "drive": 1, "path": "/path/to/disk.atr"}
 *   Insert disk image
 *   -> {"status": "ok"}
 *
 * {"cmd": "disk_eject", "drive": 1}
 *   Eject disk
 *   -> {"status": "ok"}
 *
 * {"cmd": "disk_status"}
 *   Get all drive status
 *   -> {"status": "ok", "drives": [{"drive": 1, "path": "...", "sectors": 720}, ...]}
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
 */

/* Exposed for internal use */
void AI_SendResponse(const char *json);
void AI_DebugWrite(UBYTE byte);
void AI_ApplyInput(void);  /* Apply AI input overrides after INPUT_Frame */

/* AI input overrides - set by joystick command, applied after INPUT_Frame */
extern int AI_joy_override[4];     /* -1 = no override, 0-15 = stick value */
extern int AI_trig_override[4];    /* -1 = no override, 0/1 = trigger state */

/* Configuration options */
extern int AI_enabled;
extern int AI_debug_port;  /* Memory address for debug output (0 = disabled) */
extern char AI_socket_path[256];

#endif /* AI_INTERFACE_H_ */
