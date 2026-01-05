/*
 * ai_interface.c - AI/Automation Interface for Atari800
 *
 * Copyright (c) 2026 - AI Interface Extension
 * Licensed under GPL-2.0-or-later
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <time.h>

#include "ai_interface.h"
#include "atari.h"
#include "cpu.h"
#include "memory.h"
#include "antic.h"
#include "gtia.h"
#include "pokey.h"
#include "pia.h"
#include "input.h"
#include "screen.h"
#include "akey.h"
#include "binload.h"
#include "sio.h"
#include "statesav.h"
#include "log.h"

/* Configuration */
int AI_enabled = 0;
int AI_debug_port = 0;
char AI_socket_path[256] = AI_SOCKET_PATH;

/* AI input overrides (-1 = no override) */
int AI_joy_override[4] = {-1, -1, -1, -1};
int AI_trig_override[4] = {-1, -1, -1, -1};

/* State */
static int ai_server_fd = -1;
static int ai_client_fd = -1;
static int ai_paused = 1;  /* Start paused, waiting for AI */
static int ai_frames_to_run = 0;
static int ai_steps_to_run = 0;

/* Debug output buffer */
#define AI_DEBUG_BUFFER_SIZE 4096
static UBYTE ai_debug_buffer[AI_DEBUG_BUFFER_SIZE];
static int ai_debug_buffer_pos = 0;

/* Response buffer */
static char ai_response[AI_MAX_RESPONSE];

/* Simple JSON helpers - minimal implementation */

static const char* json_get_string(const char *json, const char *key, char *buf, int bufsize) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < bufsize - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return buf;
}

static int json_get_int(const char *json, const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') return def;  /* It's a string, not int */
    return atoi(p);
}

static int json_get_bool(const char *json, const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return def;
}

/* Base64 encoding for binary data */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const UBYTE *data, int len, char *out, int outsize) {
    int i, j = 0;
    for (i = 0; i < len && j < outsize - 4; i += 3) {
        int n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        out[j++] = b64_table[(n >> 18) & 63];
        out[j++] = b64_table[(n >> 12) & 63];
        out[j++] = (i + 1 < len) ? b64_table[(n >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? b64_table[n & 63] : '=';
    }
    out[j] = '\0';
    return j;
}

/* Socket setup */
static int setup_server_socket(void) {
    struct sockaddr_un addr;

    unlink(AI_socket_path);  /* Remove existing socket */

    ai_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ai_server_fd < 0) {
        Log_print("AI: Failed to create socket: %s", strerror(errno));
        return 0;
    }

    /* Set non-blocking */
    int flags = fcntl(ai_server_fd, F_GETFL, 0);
    fcntl(ai_server_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AI_socket_path, sizeof(addr.sun_path) - 1);

    if (bind(ai_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        Log_print("AI: Failed to bind socket: %s", strerror(errno));
        close(ai_server_fd);
        ai_server_fd = -1;
        return 0;
    }

    if (listen(ai_server_fd, 1) < 0) {
        Log_print("AI: Failed to listen: %s", strerror(errno));
        close(ai_server_fd);
        ai_server_fd = -1;
        return 0;
    }

    Log_print("AI: Listening on %s", AI_socket_path);
    return 1;
}

/* Send response to client */
void AI_SendResponse(const char *json) {
    if (ai_client_fd < 0) return;

    int len = strlen(json);
    char header[32];
    snprintf(header, sizeof(header), "%d\n", len);
    write(ai_client_fd, header, strlen(header));
    write(ai_client_fd, json, len);
}

/* Debug write hook - called when program writes to debug port */
void AI_DebugWrite(UBYTE byte) {
    if (ai_debug_buffer_pos < AI_DEBUG_BUFFER_SIZE) {
        ai_debug_buffer[ai_debug_buffer_pos++] = byte;
    }
}

/* Screen to ASCII conversion */
static void screen_to_ascii(char *out, int outsize) {
    /* Map Atari screen (384x240) to 40x24 ASCII */
    const char *chars = " .:-=+*#%@";
    int char_count = strlen(chars);

    int lines = 0;
    int pos = 0;
    out[pos++] = '[';

    for (int row = 0; row < 24 && pos < outsize - 100; row++) {
        out[pos++] = '"';
        for (int col = 0; col < 40 && pos < outsize - 50; col++) {
            /* Sample screen at this character position */
            int sx = (col * 336 / 40) + 24;  /* 24 pixel left margin */
            int sy = (row * 192 / 24) + 24;  /* 24 pixel top margin */

            if (sx < 0) sx = 0;
            if (sx >= Screen_WIDTH) sx = Screen_WIDTH - 1;
            if (sy < 0) sy = 0;
            if (sy >= Screen_HEIGHT) sy = Screen_HEIGHT - 1;

            /* Get pixel value and map to ASCII */
            UBYTE pixel = ((UBYTE *)Screen_atari)[sy * Screen_WIDTH + sx];
            int brightness = (pixel & 0x0F);  /* Luminance is low nibble */
            int char_idx = brightness * (char_count - 1) / 15;
            out[pos++] = chars[char_idx];
        }
        out[pos++] = '"';
        if (row < 23) out[pos++] = ',';
        lines++;
    }
    out[pos++] = ']';
    out[pos] = '\0';
}

/* Process a command */
static void process_command(const char *cmd) {
    char cmd_type[32] = "";
    char path[512] = "";

    json_get_string(cmd, "cmd", cmd_type, sizeof(cmd_type));

    /* === CONTROL === */
    if (strcmp(cmd_type, "ping") == 0) {
        AI_SendResponse("{\"status\":\"ok\",\"msg\":\"pong\"}");
    }
    else if (strcmp(cmd_type, "load") == 0) {
        json_get_string(cmd, "path", path, sizeof(path));
        if (path[0] && BINLOAD_Loader(path)) {
            AI_SendResponse("{\"status\":\"ok\"}");
        } else {
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"error\",\"msg\":\"Failed to load %s\"}", path);
            AI_SendResponse(ai_response);
        }
    }
    else if (strcmp(cmd_type, "run") == 0) {
        ai_frames_to_run = json_get_int(cmd, "frames", 1);
        ai_paused = 0;
        /* Response sent after frames complete */
    }
    else if (strcmp(cmd_type, "step") == 0) {
        ai_steps_to_run = json_get_int(cmd, "instructions", 1);
        ai_paused = 0;
        /* Response sent after steps complete */
    }
    else if (strcmp(cmd_type, "pause") == 0) {
        ai_paused = 1;
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "reset") == 0) {
        Atari800_Coldstart();
        AI_SendResponse("{\"status\":\"ok\"}");
    }

    /* === INPUT === */
    else if (strcmp(cmd_type, "key") == 0) {
        INPUT_key_code = json_get_int(cmd, "code", AKEY_NONE);
        INPUT_key_shift = json_get_bool(cmd, "shift", 0);
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "key_release") == 0) {
        INPUT_key_code = AKEY_NONE;
        INPUT_key_shift = 0;
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "joystick") == 0) {
        int port = json_get_int(cmd, "port", 0);
        char dir[16] = "";
        json_get_string(cmd, "direction", dir, sizeof(dir));
        int fire = json_get_bool(cmd, "fire", 0);

        int stick = INPUT_STICK_CENTRE;
        if (strcmp(dir, "up") == 0) stick = INPUT_STICK_FORWARD;
        else if (strcmp(dir, "down") == 0) stick = INPUT_STICK_BACK;
        else if (strcmp(dir, "left") == 0) stick = INPUT_STICK_LEFT;
        else if (strcmp(dir, "right") == 0) stick = INPUT_STICK_RIGHT;
        else if (strcmp(dir, "ul") == 0) stick = INPUT_STICK_UL;
        else if (strcmp(dir, "ur") == 0) stick = INPUT_STICK_UR;
        else if (strcmp(dir, "ll") == 0) stick = INPUT_STICK_LL;
        else if (strcmp(dir, "lr") == 0) stick = INPUT_STICK_LR;

        if (port >= 0 && port < 4) {
            /* Set joystick override - will be applied after INPUT_Frame */
            /* Use -1 (no override) for center to allow keyboard input */
            AI_joy_override[port] = (stick == INPUT_STICK_CENTRE) ? -1 : stick;
            /* Set fire button override: 0 = pressed, -1 = no override (allows keyboard) */
            AI_trig_override[port] = fire ? 0 : -1;
        }
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "paddle") == 0) {
        int port = json_get_int(cmd, "port", 0);
        int value = json_get_int(cmd, "value", 128);
        if (port >= 0 && port < 8) {
            POKEY_POT_input[port] = value;
        }
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "consol") == 0) {
        INPUT_key_consol = INPUT_CONSOL_NONE;
        if (!json_get_bool(cmd, "start", 1)) INPUT_key_consol &= ~INPUT_CONSOL_START;
        if (!json_get_bool(cmd, "select", 1)) INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
        if (!json_get_bool(cmd, "option", 1)) INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
        AI_SendResponse("{\"status\":\"ok\"}");
    }

    /* === SCREEN === */
    else if (strcmp(cmd_type, "screenshot") == 0) {
        json_get_string(cmd, "path", path, sizeof(path));
        if (path[0]) {
            if (Screen_SaveScreenshot(path, 0)) {
                snprintf(ai_response, sizeof(ai_response),
                    "{\"status\":\"ok\",\"path\":\"%s\"}", path);
            } else {
                snprintf(ai_response, sizeof(ai_response),
                    "{\"status\":\"error\",\"msg\":\"Failed to save screenshot\"}");
            }
        } else {
            /* Default path */
            snprintf(path, sizeof(path), "/tmp/atari800_ai_%ld.png", (long)time(NULL));
            Screen_SaveScreenshot(path, 0);
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"ok\",\"path\":\"%s\"}", path);
        }
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "screen_ascii") == 0) {
        char ascii_data[2048];
        screen_to_ascii(ascii_data, sizeof(ascii_data));
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"width\":40,\"height\":24,\"data\":%s}", ascii_data);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "screen_raw") == 0) {
        /* Base64 encode screen buffer */
        static char b64_buf[Screen_WIDTH * Screen_HEIGHT * 2];
        base64_encode((UBYTE*)Screen_atari, Screen_WIDTH * Screen_HEIGHT,
                      b64_buf, sizeof(b64_buf));
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"width\":%d,\"height\":%d,\"data\":\"%s\"}",
            Screen_WIDTH, Screen_HEIGHT, b64_buf);
        AI_SendResponse(ai_response);
    }

    /* === MEMORY === */
    else if (strcmp(cmd_type, "peek") == 0) {
        int addr = json_get_int(cmd, "addr", 0);
        int len = json_get_int(cmd, "len", 1);
        if (len > 256) len = 256;  /* Limit */

        int pos = snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"addr\":%d,\"data\":[", addr);
        for (int i = 0; i < len && pos < sizeof(ai_response) - 20; i++) {
            pos += snprintf(ai_response + pos, sizeof(ai_response) - pos,
                "%s%d", i ? "," : "", MEMORY_SafeGetByte((UWORD)(addr + i)));
        }
        snprintf(ai_response + pos, sizeof(ai_response) - pos, "]}");
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "poke") == 0) {
        int addr = json_get_int(cmd, "addr", 0);
        /* Parse data array - simple extraction */
        const char *data = strstr(cmd, "\"data\":");
        if (data) {
            data = strchr(data, '[');
            if (data) {
                data++;
                while (*data && *data != ']') {
                    while (*data == ' ' || *data == ',') data++;
                    if (*data >= '0' && *data <= '9') {
                        int val = atoi(data);
                        MEMORY_PutByte((UWORD)addr++, (UBYTE)val);
                        while (*data >= '0' && *data <= '9') data++;
                    } else {
                        break;
                    }
                }
            }
        }
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "dump") == 0) {
        int start = json_get_int(cmd, "start", 0);
        int end = json_get_int(cmd, "end", 0xFFFF);
        json_get_string(cmd, "path", path, sizeof(path));
        if (path[0]) {
            FILE *f = fopen(path, "wb");
            if (f) {
                for (int i = start; i <= end; i++) {
                    UBYTE b = MEMORY_SafeGetByte((UWORD)i);
                    fwrite(&b, 1, 1, f);
                }
                fclose(f);
                snprintf(ai_response, sizeof(ai_response),
                    "{\"status\":\"ok\",\"bytes\":%d}", end - start + 1);
            } else {
                snprintf(ai_response, sizeof(ai_response),
                    "{\"status\":\"error\",\"msg\":\"Failed to open file\"}");
            }
        } else {
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"error\",\"msg\":\"No path specified\"}");
        }
        AI_SendResponse(ai_response);
    }

    /* === CPU === */
    else if (strcmp(cmd_type, "cpu") == 0) {
        CPU_GetStatus();
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"pc\":%d,\"a\":%d,\"x\":%d,\"y\":%d,"
            "\"sp\":%d,\"p\":%d,"
            "\"n\":%d,\"v\":%d,\"b\":%d,\"d\":%d,\"i\":%d,\"z\":%d,\"c\":%d}",
            CPU_regPC, CPU_regA, CPU_regX, CPU_regY, CPU_regS, CPU_regP,
            (CPU_regP & CPU_N_FLAG) ? 1 : 0,
            (CPU_regP & CPU_V_FLAG) ? 1 : 0,
            (CPU_regP & CPU_B_FLAG) ? 1 : 0,
            (CPU_regP & CPU_D_FLAG) ? 1 : 0,
            (CPU_regP & CPU_I_FLAG) ? 1 : 0,
            (CPU_regP & CPU_Z_FLAG) ? 1 : 0,
            (CPU_regP & CPU_C_FLAG) ? 1 : 0);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "cpu_set") == 0) {
        /* Only set values that are specified */
        const char *p;
        if ((p = strstr(cmd, "\"pc\":"))) CPU_regPC = atoi(p + 5);
        if ((p = strstr(cmd, "\"a\":"))) CPU_regA = atoi(p + 4);
        if ((p = strstr(cmd, "\"x\":"))) CPU_regX = atoi(p + 4);
        if ((p = strstr(cmd, "\"y\":"))) CPU_regY = atoi(p + 4);
        if ((p = strstr(cmd, "\"sp\":"))) CPU_regS = atoi(p + 5);
        CPU_PutStatus();
        AI_SendResponse("{\"status\":\"ok\"}");
    }

    /* === CHIPS === */
    else if (strcmp(cmd_type, "antic") == 0) {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"dmactl\":%d,\"chactl\":%d,\"dlist\":%d,"
            "\"hscrol\":%d,\"vscrol\":%d,\"pmbase\":%d,\"chbase\":%d,"
            "\"nmien\":%d,\"nmist\":%d,\"ypos\":%d,\"xpos\":%d}",
            ANTIC_DMACTL, ANTIC_CHACTL, ANTIC_dlist,
            ANTIC_HSCROL, ANTIC_VSCROL, ANTIC_PMBASE, ANTIC_CHBASE,
            ANTIC_NMIEN, ANTIC_NMIST, ANTIC_ypos, ANTIC_xpos);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "gtia") == 0) {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\","
            "\"hposp0\":%d,\"hposp1\":%d,\"hposp2\":%d,\"hposp3\":%d,"
            "\"hposm0\":%d,\"hposm1\":%d,\"hposm2\":%d,\"hposm3\":%d,"
            "\"sizep0\":%d,\"sizep1\":%d,\"sizep2\":%d,\"sizep3\":%d,\"sizem\":%d,"
            "\"grafp0\":%d,\"grafp1\":%d,\"grafp2\":%d,\"grafp3\":%d,\"grafm\":%d,"
            "\"colpm0\":%d,\"colpm1\":%d,\"colpm2\":%d,\"colpm3\":%d,"
            "\"colpf0\":%d,\"colpf1\":%d,\"colpf2\":%d,\"colpf3\":%d,\"colbk\":%d,"
            "\"prior\":%d,\"gractl\":%d,"
            "\"trig0\":%d,\"trig1\":%d,\"trig2\":%d,\"trig3\":%d}",
            GTIA_HPOSP0, GTIA_HPOSP1, GTIA_HPOSP2, GTIA_HPOSP3,
            GTIA_HPOSM0, GTIA_HPOSM1, GTIA_HPOSM2, GTIA_HPOSM3,
            GTIA_SIZEP0, GTIA_SIZEP1, GTIA_SIZEP2, GTIA_SIZEP3, GTIA_SIZEM,
            GTIA_GRAFP0, GTIA_GRAFP1, GTIA_GRAFP2, GTIA_GRAFP3, GTIA_GRAFM,
            GTIA_COLPM0, GTIA_COLPM1, GTIA_COLPM2, GTIA_COLPM3,
            GTIA_COLPF0, GTIA_COLPF1, GTIA_COLPF2, GTIA_COLPF3, GTIA_COLBK,
            GTIA_PRIOR, GTIA_GRACTL,
            GTIA_TRIG[0], GTIA_TRIG[1], GTIA_TRIG[2], GTIA_TRIG[3]);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "pokey") == 0) {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\","
            "\"audf1\":%d,\"audc1\":%d,\"audf2\":%d,\"audc2\":%d,"
            "\"audf3\":%d,\"audc3\":%d,\"audf4\":%d,\"audc4\":%d,"
            "\"audctl\":%d,\"kbcode\":%d,\"irqen\":%d,\"irqst\":%d,"
            "\"skstat\":%d,\"skctl\":%d,"
            "\"pot0\":%d,\"pot1\":%d,\"pot2\":%d,\"pot3\":%d,"
            "\"pot4\":%d,\"pot5\":%d,\"pot6\":%d,\"pot7\":%d}",
            POKEY_AUDF[0], POKEY_AUDC[0], POKEY_AUDF[1], POKEY_AUDC[1],
            POKEY_AUDF[2], POKEY_AUDC[2], POKEY_AUDF[3], POKEY_AUDC[3],
            POKEY_AUDCTL[0], POKEY_KBCODE, POKEY_IRQEN, POKEY_IRQST,
            POKEY_SKSTAT, POKEY_SKCTL,
            POKEY_POT_input[0], POKEY_POT_input[1], POKEY_POT_input[2], POKEY_POT_input[3],
            POKEY_POT_input[4], POKEY_POT_input[5], POKEY_POT_input[6], POKEY_POT_input[7]);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "pia") == 0) {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"porta\":%d,\"portb\":%d,"
            "\"pactl\":%d,\"pbctl\":%d,"
            "\"port_input0\":%d,\"port_input1\":%d}",
            PIA_PORTA, PIA_PORTB, PIA_PACTL, PIA_PBCTL,
            PIA_PORT_input[0], PIA_PORT_input[1]);
        AI_SendResponse(ai_response);
    }

    /* === DEBUG === */
    else if (strcmp(cmd_type, "debug_enable") == 0) {
        AI_debug_port = json_get_int(cmd, "addr", 0xD7FF);
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "debug_read") == 0) {
        int pos = snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"data\":[");
        for (int i = 0; i < ai_debug_buffer_pos && pos < sizeof(ai_response) - 100; i++) {
            pos += snprintf(ai_response + pos, sizeof(ai_response) - pos,
                "%s%d", i ? "," : "", ai_debug_buffer[i]);
        }
        pos += snprintf(ai_response + pos, sizeof(ai_response) - pos, "],\"ascii\":\"");
        for (int i = 0; i < ai_debug_buffer_pos && pos < sizeof(ai_response) - 10; i++) {
            UBYTE c = ai_debug_buffer[i];
            if (c >= 32 && c < 127 && c != '"' && c != '\\') {
                ai_response[pos++] = c;
            } else {
                ai_response[pos++] = '.';
            }
        }
        snprintf(ai_response + pos, sizeof(ai_response) - pos, "\"}");
        ai_debug_buffer_pos = 0;  /* Clear buffer */
        AI_SendResponse(ai_response);
    }

    /* === STATE === */
    else if (strcmp(cmd_type, "save_state") == 0) {
        json_get_string(cmd, "path", path, sizeof(path));
        if (path[0] && StateSav_SaveAtariState(path, "wb", TRUE)) {
            AI_SendResponse("{\"status\":\"ok\"}");
        } else {
            AI_SendResponse("{\"status\":\"error\",\"msg\":\"Failed to save state\"}");
        }
    }
    else if (strcmp(cmd_type, "load_state") == 0) {
        json_get_string(cmd, "path", path, sizeof(path));
        if (path[0] && StateSav_ReadAtariState(path, "rb")) {
            AI_SendResponse("{\"status\":\"ok\"}");
        } else {
            AI_SendResponse("{\"status\":\"error\",\"msg\":\"Failed to load state\"}");
        }
    }

    /* Unknown command */
    else {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"error\",\"msg\":\"Unknown command: %s\"}", cmd_type);
        AI_SendResponse(ai_response);
    }
}

/* Read command from client */
static int read_command(char *buf, int bufsize) {
    if (ai_client_fd < 0) return 0;

    /* Read length prefix */
    char header[32];
    int hpos = 0;
    while (hpos < sizeof(header) - 1) {
        int r = read(ai_client_fd, header + hpos, 1);
        if (r <= 0) {
            if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                /* Client disconnected */
                close(ai_client_fd);
                ai_client_fd = -1;
                Log_print("AI: Client disconnected");
            }
            return 0;
        }
        if (header[hpos] == '\n') {
            header[hpos] = '\0';
            break;
        }
        hpos++;
    }

    int len = atoi(header);
    if (len <= 0 || len >= bufsize) return 0;

    /* Read JSON body */
    int total = 0;
    while (total < len) {
        int r = read(ai_client_fd, buf + total, len - total);
        if (r <= 0) return 0;
        total += r;
    }
    buf[len] = '\0';
    return len;
}

/* Check for and accept new connections */
static void check_connections(void) {
    if (ai_server_fd < 0) return;

    fd_set fds;
    struct timeval tv = {0, 0};

    FD_ZERO(&fds);
    FD_SET(ai_server_fd, &fds);

    if (select(ai_server_fd + 1, &fds, NULL, NULL, &tv) > 0) {
        int client = accept(ai_server_fd, NULL, NULL);
        if (client >= 0) {
            if (ai_client_fd >= 0) {
                close(ai_client_fd);  /* Only one client at a time */
            }
            ai_client_fd = client;
            /* Set non-blocking */
            int flags = fcntl(ai_client_fd, F_GETFL, 0);
            fcntl(ai_client_fd, F_SETFL, flags | O_NONBLOCK);
            Log_print("AI: Client connected");
            ai_paused = 1;  /* Pause and wait for commands */
        }
    }
}

/* Initialize AI interface */
int AI_Initialise(int *argc, char *argv[]) {
    int i, j;

    for (i = j = 1; i < *argc; i++) {
        int match = FALSE;

        if (strcmp(argv[i], "-ai") == 0) {
            AI_enabled = TRUE;
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-socket") == 0 && i + 1 < *argc) {
            strncpy(AI_socket_path, argv[++i], sizeof(AI_socket_path) - 1);
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-debug-port") == 0 && i + 1 < *argc) {
            AI_debug_port = strtol(argv[++i], NULL, 0);
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-run") == 0) {
            AI_enabled = TRUE;
            ai_paused = 0;  /* Don't start paused */
            match = TRUE;
        }

        if (!match) {
            argv[j++] = argv[i];
        }
    }
    *argc = j;

    if (AI_enabled) {
        if (!setup_server_socket()) {
            AI_enabled = FALSE;
            return FALSE;
        }
        Log_print("AI: Interface enabled");
    }

    return TRUE;
}

/* Cleanup */
void AI_Exit(void) {
    if (ai_client_fd >= 0) {
        close(ai_client_fd);
        ai_client_fd = -1;
    }
    if (ai_server_fd >= 0) {
        close(ai_server_fd);
        ai_server_fd = -1;
    }
    unlink(AI_socket_path);
}

/* Process AI commands each frame */
void AI_Frame(void) {
    static char cmd_buf[AI_BUFFER_SIZE];

    if (!AI_enabled) return;

    check_connections();

    /* If we were running frames, decrement and check */
    if (ai_frames_to_run > 0) {
        ai_frames_to_run--;
        if (ai_frames_to_run == 0) {
            ai_paused = 1;
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"ok\",\"frames_run\":1}");
            AI_SendResponse(ai_response);
        }
    }

    /* Process commands while paused */
    while (ai_paused && ai_client_fd >= 0) {
        if (read_command(cmd_buf, sizeof(cmd_buf)) > 0) {
            process_command(cmd_buf);
        } else {
            /* No command available, wait a bit */
            usleep(1000);  /* 1ms */
        }
        check_connections();
    }
}

/* Check if AI is paused */
int AI_IsPaused(void) {
    return AI_enabled && ai_paused;
}

/* Apply AI input overrides - call AFTER INPUT_Frame */
void AI_ApplyInput(void) {
    int i;
    if (!AI_enabled) return;

    for (i = 0; i < 4; i++) {
        /* Apply joystick override */
        if (AI_joy_override[i] >= 0) {
            int pia_idx = (i < 2) ? 0 : 1;
            int shift = (i & 1) * 4;
            PIA_PORT_input[pia_idx] &= ~(0x0F << shift);
            PIA_PORT_input[pia_idx] |= (AI_joy_override[i] << shift);
        }

        /* Apply trigger override */
        if (AI_trig_override[i] >= 0) {
            GTIA_TRIG[i] = AI_trig_override[i];
        }
    }
}
