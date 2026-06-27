#!/usr/bin/env python3
import argparse
from pathlib import Path


def word(value):
    return bytes((value & 0xff, (value >> 8) & 0xff))


def screen_code(ch):
    value = ord(ch)
    if 32 <= value < 96:
        return value - 32
    return 0


def xex_segment(addr, data):
    end = addr + len(data) - 1
    return word(addr) + word(end) + bytes(data)


def write_xex(path, start, code):
    data = bytearray((0xff, 0xff))
    data += xex_segment(start, code)
    data += xex_segment(0x02e0, word(start))
    path.write_bytes(data)


def display_debug_code(start, screen_text, debug_text, extra=()):
    msg_addr = start + 14 + len(debug_text) * 5 + len(extra) + 3
    code = bytearray((
        0xa0, 0x00,              # ldy #0
        0xb9, msg_addr & 0xff, msg_addr >> 8,  # lda msg,y
        0xc9, 0xff,              # cmp #$ff
        0xf0, 0x05,              # beq debug
        0x91, 0x58,              # sta (SAVMSC),y
        0xc8,                    # iny
        0xd0, 0xf4,              # bne copy
    ))
    for ch in debug_text.encode("ascii"):
        code += bytes((0xa9, ch, 0x8d, 0xff, 0xd7))
    code += bytes(extra)
    wait_addr = start + len(code)
    code += bytes((0x4c, wait_addr & 0xff, wait_addr >> 8))
    code += bytes(screen_code(ch) for ch in screen_text)
    code += b"\xff"
    return code


def joystick_code():
    return bytes((
        0xad, 0x78, 0x02,        # lda STICK0 shadow
        0x8d, 0x00, 0x06,        # sta $0600
        0x8d, 0xff, 0xd7,        # sta debug
        0xad, 0x84, 0x02,        # lda STRIG0 shadow
        0x8d, 0x01, 0x06,        # sta $0601
        0x8d, 0xff, 0xd7,        # sta debug
        0x4c, 0x00, 0x21,        # jmp $2100
    ))


def blank_atr(payload_sectors=None):
    sectors = 720
    sector_size = 128
    paragraphs = sectors * sector_size // 16
    header = bytearray(16)
    header[0] = 0x96
    header[1] = 0x02
    header[2] = paragraphs & 0xff
    header[3] = (paragraphs >> 8) & 0xff
    header[4] = sector_size & 0xff
    header[5] = (sector_size >> 8) & 0xff
    body = bytearray(sectors * sector_size)
    if payload_sectors:
        for sector, data in payload_sectors.items():
            offset = (sector - 1) * sector_size
            body[offset:offset + len(data)] = data[:sector_size]
    return bytes(header + body)


def boot_sector(marker):
    start = 0x0600
    code = display_debug_code(start + 6, f"{marker} READY", f"{marker}\n")
    sector = bytearray(128)
    sector[0] = 0x00              # DFLAGS
    sector[1] = 0x01              # one boot sector
    sector[2:4] = word(start)     # BOOTAD
    sector[4:6] = word(0x0000)    # INITAD
    sector[6:6 + len(code)] = code[:122]
    return bytes(sector)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir")
    args = parser.parse_args()
    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    write_xex(out / "hello_debug.xex", 0x2000, display_debug_code(0x2000, "PHASE14 READY", "PHASE14\n"))
    write_xex(out / "screen_text.xex", 0x2000, display_debug_code(0x2000, "SCREEN14 READY", "SCREEN14\n"))
    write_xex(out / "joystick_test.xex", 0x2100, joystick_code())
    net_extra = bytes((
        0xa9, 0x14, 0x8d, 0x08, 0x06,
        0xa9, 0x28, 0x8d, 0x09, 0x06,
        0xa9, 0x40, 0x8d, 0x0a, 0x06,
    ))
    write_xex(out / "netstream_speed.xex", 0x2200, display_debug_code(0x2200, "NETSPEED14 READY", "NETSPEED14\n", net_extra))
    (out / "disk_boot.atr").write_bytes(blank_atr({1: boot_sector("DISK14")}))
    (out / "fujinet_boot.atr").write_bytes(blank_atr({1: boot_sector("FUJI14")}))


if __name__ == "__main__":
    main()
