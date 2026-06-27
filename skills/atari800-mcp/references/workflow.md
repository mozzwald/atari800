# Atari App/Game Development Workflow

## Target Artifacts

Work with the artifact the Atari would actually run:

- `.atr` disk images
- `.xex`, `.com`, or other executable loads
- BASIC or assembly outputs
- generated test disks or boot images
- saved states or memory/debug fixtures when appropriate

If source changed, build the runnable artifact before emulator validation.

## Session Lifecycle

- Use MCP preflight/capability/status tools when the environment or tool support is uncertain.
- Start Atari800 through the MCP server so sockets, display mode, artifact paths, and cleanup are session-owned.
- Prefer headless sessions for automated tests unless visible display behavior is specifically being checked.
- Stop through MCP cleanup tools; never use broad process cleanup for emulator tests.

## Loading And Booting

- For executable-style programs, use the MCP load/start path appropriate to the tool inventory.
- For native Atari disk testing, use native disk insert/eject/status tools. Source disk images should be copied into the managed workspace and mounted read-only by default.
- Request write-enabled disk tests only when the app needs to write, and report the managed output disk path instead of mutating the source image.
- For FujiNet disk/network workflows, use the FujiNet reference instead of trying to drive CONFIG UI manually.

## Driving The Program

- Use MCP keyboard, typed text, console key, joystick, and input-status helpers.
- Advance the emulator with frame/run tools and bounded waits.
- Prefer deterministic input sequences and include them in the test summary.

## Observing Results

Use the least invasive observable that proves the behavior:

- screen text for OS/simple text modes
- screenshots/framebuffer captures for graphics or visual checks
- debug port output for test programs that emit markers
- `run_until` predicates for screen text, memory, CPU/PC, debug text, FujiNet logs, NetSIO events, breakpoints, frame counts, or emulator exit
- artifact tools for screenshots, logs, dumps, generated configs, copied disks, and saved outputs
- debugger/memory/disassembly tools for failure diagnosis or low-level assertions

Always bound waits with frame and/or wall-clock limits. On timeout, collect diagnostics rather than waiting indefinitely.

## Reporting

Report:

- artifact path/type tested
- boot/load method
- key inputs and timing/frame assumptions
- observed screen/debug/artifact/memory evidence
- pass/fail/inconclusive result
- diagnostics for failures or timeouts

Do not claim success from compile/build success alone. Run the Atari target and observe behavior.
