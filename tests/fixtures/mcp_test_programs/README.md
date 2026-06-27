# MCP Test Programs

`generate.py` writes deterministic Atari test fixtures into a caller-provided output directory:

- `hello_debug.xex`: writes `PHASE14` to the AI debug port and displays `PHASE14 READY`.
- `screen_text.xex`: displays stable OS text for `atari_screen_text`.
- `joystick_test.xex`: mirrors STICK0/STRIG0 into `$0600/$0601` and the debug port.
- `disk_boot.atr`: native boot ATR that writes `DISK14`.
- `fujinet_boot.atr`: FujiNet boot ATR that writes `FUJI14`.
- `netstream_speed.xex`: deterministic NETStream probe placeholder that writes `NETSPEED14` and marker bytes at `$0608-$060a`.

The fixtures are generated at smoke-test runtime so binary outputs do not need to be stored in git.
