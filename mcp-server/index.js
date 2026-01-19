#!/usr/bin/env node
/**
 * Atari 800 MCP Server
 *
 * Provides MCP tools for controlling the Atari 800 emulator via AI interface.
 */

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import net from 'net';
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

// Get directory of this script
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const SOCKET_PATH = '/tmp/atari800_ai.sock';
// Default to emulator built in ../src/atari800 relative to mcp-server directory
// Can be overridden with ATARI800_PATH environment variable
const EMULATOR_PATH = process.env.ATARI800_PATH || path.join(__dirname, '..', 'src', 'atari800');

let emulatorProcess = null;

// Send command to emulator and get response
async function sendCommand(cmd) {
  return new Promise((resolve, reject) => {
    const client = net.createConnection(SOCKET_PATH, () => {
      const json = JSON.stringify(cmd);
      const header = `${json.length}\n`;
      client.write(header + json);
    });

    let data = '';
    let expectedLength = null;

    client.on('data', (chunk) => {
      data += chunk.toString();

      if (expectedLength === null) {
        const newlineIdx = data.indexOf('\n');
        if (newlineIdx !== -1) {
          expectedLength = parseInt(data.substring(0, newlineIdx));
          data = data.substring(newlineIdx + 1);
        }
      }

      if (expectedLength !== null && data.length >= expectedLength) {
        client.end();
        try {
          resolve(JSON.parse(data.substring(0, expectedLength)));
        } catch (e) {
          reject(new Error(`Failed to parse response: ${data}`));
        }
      }
    });

    client.on('error', (err) => {
      reject(err);
    });

    client.on('timeout', () => {
      client.end();
      reject(new Error('Connection timeout'));
    });

    client.setTimeout(10000);
  });
}

// Check if emulator is running
function isEmulatorRunning() {
  return fs.existsSync(SOCKET_PATH);
}

// Format screen ASCII for display
function formatScreen(data) {
  if (!data || !Array.isArray(data)) return 'No screen data';
  return '┌' + '─'.repeat(40) + '┐\n' +
         data.map(line => '│' + line + '│').join('\n') + '\n' +
         '└' + '─'.repeat(40) + '┘';
}

// Create the MCP server
const server = new Server(
  {
    name: 'atari800',
    version: '1.0.0',
  },
  {
    capabilities: {
      tools: {},
    },
  }
);

// List available tools
server.setRequestHandler(ListToolsRequestSchema, async () => {
  return {
    tools: [
      {
        name: 'atari_start',
        description: 'Start the Atari 800 emulator with a program. Must be called before other commands.',
        inputSchema: {
          type: 'object',
          properties: {
            program: {
              type: 'string',
              description: 'Path to the program (.xex, .com, .bas) to run',
            },
          },
          required: ['program'],
        },
      },
      {
        name: 'atari_stop',
        description: 'Stop the Atari 800 emulator',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_run',
        description: 'Run the emulator for N frames (1 frame = 1/60 second)',
        inputSchema: {
          type: 'object',
          properties: {
            frames: {
              type: 'number',
              description: 'Number of frames to run (default: 60)',
              default: 60,
            },
          },
        },
      },
      {
        name: 'atari_screen',
        description: 'Get the current screen as ASCII art (40x24 characters)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_joystick',
        description: 'Set joystick state (direction and fire button)',
        inputSchema: {
          type: 'object',
          properties: {
            direction: {
              type: 'string',
              enum: ['center', 'up', 'down', 'left', 'right', 'ul', 'ur', 'll', 'lr'],
              description: 'Joystick direction',
              default: 'center',
            },
            fire: {
              type: 'boolean',
              description: 'Fire button pressed',
              default: false,
            },
            port: {
              type: 'number',
              description: 'Joystick port (0-3)',
              default: 0,
            },
          },
        },
      },
      {
        name: 'atari_key',
        description: 'Press a key on the Atari keyboard',
        inputSchema: {
          type: 'object',
          properties: {
            key: {
              type: 'string',
              description: 'Key to press (a-z, 0-9, space, return, escape)',
            },
          },
          required: ['key'],
        },
      },
      {
        name: 'atari_consol',
        description: 'Press console keys (Start, Select, Option)',
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
        description: 'Read memory from the Atari',
        inputSchema: {
          type: 'object',
          properties: {
            address: {
              type: 'number',
              description: 'Memory address (0-65535)',
            },
            length: {
              type: 'number',
              description: 'Number of bytes to read (default: 1)',
              default: 1,
            },
          },
          required: ['address'],
        },
      },
      {
        name: 'atari_poke',
        description: 'Write to Atari memory',
        inputSchema: {
          type: 'object',
          properties: {
            address: {
              type: 'number',
              description: 'Memory address (0-65535)',
            },
            values: {
              type: 'array',
              items: { type: 'number' },
              description: 'Byte values to write (0-255)',
            },
          },
          required: ['address', 'values'],
        },
      },
      {
        name: 'atari_cpu',
        description: 'Get CPU state (registers, flags)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_gtia',
        description: 'Get GTIA chip state (graphics, sprites, triggers)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_pokey',
        description: 'Get POKEY chip state (sound, keyboard)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_antic',
        description: 'Get ANTIC chip state (display list, scrolling)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_pia',
        description: 'Get PIA chip state (I/O ports, joystick raw values)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_reset',
        description: 'Cold reset the Atari',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'atari_save_state',
        description: 'Save emulator state to a file',
        inputSchema: {
          type: 'object',
          properties: {
            path: {
              type: 'string',
              description: 'Path to save state file',
            },
          },
          required: ['path'],
        },
      },
      {
        name: 'atari_load_state',
        description: 'Load emulator state from a file',
        inputSchema: {
          type: 'object',
          properties: {
            path: {
              type: 'string',
              description: 'Path to state file',
            },
          },
          required: ['path'],
        },
      },
    ],
  };
});

// Key code mapping
const KEY_CODES = {
  'a': 63, 'b': 21, 'c': 18, 'd': 58, 'e': 42, 'f': 56, 'g': 61, 'h': 57,
  'i': 13, 'j': 1, 'k': 5, 'l': 0, 'm': 37, 'n': 35, 'o': 8, 'p': 10,
  'q': 47, 'r': 40, 's': 62, 't': 45, 'u': 11, 'v': 16, 'w': 46, 'x': 22,
  'y': 43, 'z': 23,
  '0': 50, '1': 31, '2': 30, '3': 26, '4': 24, '5': 29, '6': 27, '7': 51,
  '8': 53, '9': 48,
  'space': 33, ' ': 33, 'return': 12, 'enter': 12, 'escape': 28, 'esc': 28,
  'tab': 44, 'backspace': 52,
};

// Handle tool calls
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {
      case 'atari_start': {
        if (emulatorProcess) {
          emulatorProcess.kill();
          emulatorProcess = null;
        }

        // Remove old socket
        if (fs.existsSync(SOCKET_PATH)) {
          fs.unlinkSync(SOCKET_PATH);
        }

        // Start emulator
        emulatorProcess = spawn(EMULATOR_PATH, [
          '-ai', '-xl', '-run', args.program
        ], {
          detached: true,
          stdio: 'ignore',
        });
        emulatorProcess.unref();

        // Wait for socket to appear
        for (let i = 0; i < 50; i++) {
          await new Promise(r => setTimeout(r, 100));
          if (fs.existsSync(SOCKET_PATH)) {
            // Try to ping
            try {
              const resp = await sendCommand({ cmd: 'ping' });
              if (resp.status === 'ok') {
                return {
                  content: [{ type: 'text', text: `Emulator started with ${args.program}\nSocket: ${SOCKET_PATH}\nReady for commands.` }],
                };
              }
            } catch (e) {
              // Keep waiting
            }
          }
        }
        return {
          content: [{ type: 'text', text: 'Emulator started but socket not ready. Try again in a moment.' }],
        };
      }

      case 'atari_stop': {
        if (emulatorProcess) {
          emulatorProcess.kill();
          emulatorProcess = null;
        }
        // Also try to kill by name
        try {
          spawn('pkill', ['-9', 'atari800']);
        } catch (e) {}
        return {
          content: [{ type: 'text', text: 'Emulator stopped.' }],
        };
      }

      case 'atari_run': {
        const frames = args.frames || 60;
        const resp = await sendCommand({ cmd: 'run', frames });
        return {
          content: [{ type: 'text', text: `Ran ${frames} frames.` }],
        };
      }

      case 'atari_screen': {
        const resp = await sendCommand({ cmd: 'screen_ascii' });
        const screen = formatScreen(resp.data);
        return {
          content: [{ type: 'text', text: screen }],
        };
      }

      case 'atari_joystick': {
        const resp = await sendCommand({
          cmd: 'joystick',
          port: args.port || 0,
          direction: args.direction || 'center',
          fire: args.fire || false,
        });
        return {
          content: [{ type: 'text', text: `Joystick: ${args.direction || 'center'}, fire: ${args.fire || false}` }],
        };
      }

      case 'atari_key': {
        const key = args.key.toLowerCase();
        const code = KEY_CODES[key];
        if (code === undefined) {
          return {
            content: [{ type: 'text', text: `Unknown key: ${args.key}` }],
          };
        }
        await sendCommand({ cmd: 'key', code, shift: false });
        return {
          content: [{ type: 'text', text: `Pressed key: ${args.key}` }],
        };
      }

      case 'atari_consol': {
        const resp = await sendCommand({
          cmd: 'consol',
          start: args.start || false,
          select: args.select || false,
          option: args.option || false,
        });
        return {
          content: [{ type: 'text', text: `Console keys: start=${args.start}, select=${args.select}, option=${args.option}` }],
        };
      }

      case 'atari_peek': {
        const resp = await sendCommand({
          cmd: 'peek',
          addr: args.address,
          len: args.length || 1,
        });
        const hex = resp.data.map(b => b.toString(16).padStart(2, '0')).join(' ');
        return {
          content: [{ type: 'text', text: `$${args.address.toString(16).padStart(4, '0')}: ${hex} (${resp.data.join(', ')})` }],
        };
      }

      case 'atari_poke': {
        const resp = await sendCommand({
          cmd: 'poke',
          addr: args.address,
          data: args.values,
        });
        return {
          content: [{ type: 'text', text: `Wrote ${args.values.length} bytes to $${args.address.toString(16).padStart(4, '0')}` }],
        };
      }

      case 'atari_cpu': {
        const resp = await sendCommand({ cmd: 'cpu' });
        const text = `CPU State:
  PC: $${resp.pc.toString(16).padStart(4, '0')}
  A:  $${resp.a.toString(16).padStart(2, '0')} (${resp.a})
  X:  $${resp.x.toString(16).padStart(2, '0')} (${resp.x})
  Y:  $${resp.y.toString(16).padStart(2, '0')} (${resp.y})
  SP: $${resp.sp.toString(16).padStart(2, '0')}
  Flags: N=${resp.n} V=${resp.v} B=${resp.b} D=${resp.d} I=${resp.i} Z=${resp.z} C=${resp.c}`;
        return {
          content: [{ type: 'text', text }],
        };
      }

      case 'atari_gtia': {
        const resp = await sendCommand({ cmd: 'gtia' });
        const text = `GTIA State:
  Players: HPOS=${resp.hposp0},${resp.hposp1},${resp.hposp2},${resp.hposp3}
  Missiles: HPOS=${resp.hposm0},${resp.hposm1},${resp.hposm2},${resp.hposm3}
  Colors: PM=${resp.colpm0},${resp.colpm1},${resp.colpm2},${resp.colpm3}
          PF=${resp.colpf0},${resp.colpf1},${resp.colpf2},${resp.colpf3} BK=${resp.colbk}
  Triggers: ${resp.trig0},${resp.trig1},${resp.trig2},${resp.trig3}
  PRIOR: ${resp.prior} GRACTL: ${resp.gractl}`;
        return {
          content: [{ type: 'text', text }],
        };
      }

      case 'atari_pokey': {
        const resp = await sendCommand({ cmd: 'pokey' });
        const text = `POKEY State:
  Audio: F=${resp.audf1},${resp.audf2},${resp.audf3},${resp.audf4}
         C=${resp.audc1},${resp.audc2},${resp.audc3},${resp.audc4}
  AUDCTL: ${resp.audctl}
  KBCODE: ${resp.kbcode}
  IRQ: EN=${resp.irqen} ST=${resp.irqst}
  Pots: ${resp.pot0},${resp.pot1},${resp.pot2},${resp.pot3}`;
        return {
          content: [{ type: 'text', text }],
        };
      }

      case 'atari_antic': {
        const resp = await sendCommand({ cmd: 'antic' });
        const text = `ANTIC State:
  DMACTL: ${resp.dmactl}
  DLIST: $${resp.dlist.toString(16).padStart(4, '0')}
  CHBASE: $${(resp.chbase * 256).toString(16).padStart(4, '0')}
  PMBASE: $${(resp.pmbase * 256).toString(16).padStart(4, '0')}
  Scroll: H=${resp.hscrol} V=${resp.vscrol}
  NMI: EN=${resp.nmien} ST=${resp.nmist}
  Position: Y=${resp.ypos} X=${resp.xpos}`;
        return {
          content: [{ type: 'text', text }],
        };
      }

      case 'atari_pia': {
        const resp = await sendCommand({ cmd: 'pia' });
        const text = `PIA State:
  PORTA: $${resp.porta.toString(16).padStart(2, '0')} (joysticks 0-1)
  PORTB: $${resp.portb.toString(16).padStart(2, '0')} (memory control)
  PACTL: $${resp.pactl.toString(16).padStart(2, '0')}
  PBCTL: $${resp.pbctl.toString(16).padStart(2, '0')}
  Input: ${resp.port_input0.toString(16)}, ${resp.port_input1.toString(16)}`;
        return {
          content: [{ type: 'text', text }],
        };
      }

      case 'atari_reset': {
        const resp = await sendCommand({ cmd: 'reset' });
        return {
          content: [{ type: 'text', text: 'Atari reset.' }],
        };
      }

      case 'atari_save_state': {
        const resp = await sendCommand({ cmd: 'save_state', path: args.path });
        return {
          content: [{ type: 'text', text: resp.status === 'ok' ? `State saved to ${args.path}` : `Failed: ${resp.msg}` }],
        };
      }

      case 'atari_load_state': {
        const resp = await sendCommand({ cmd: 'load_state', path: args.path });
        return {
          content: [{ type: 'text', text: resp.status === 'ok' ? `State loaded from ${args.path}` : `Failed: ${resp.msg}` }],
        };
      }

      default:
        return {
          content: [{ type: 'text', text: `Unknown tool: ${name}` }],
          isError: true,
        };
    }
  } catch (error) {
    return {
      content: [{ type: 'text', text: `Error: ${error.message}. Is the emulator running? Use atari_start first.` }],
      isError: true,
    };
  }
});

// Start the server
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error('Atari 800 MCP Server running');
}

main().catch(console.error);
