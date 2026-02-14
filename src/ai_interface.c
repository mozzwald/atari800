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
#include "crc32.h"

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

/* Frame streaming (RGB565 over UNIX sockets) */
#define AI_FB_PUSH_SOCKET_PATH "/tmp/atari800-fb-push.sock"
#define AI_FB_PULL_SOCKET_PATH "/tmp/atari800-fb-pull.sock"

#define AI_FB_HEADER_SIZE 36
#define AI_FB_PULL_REQ_SIZE 16

enum {
    AI_FB_FLAG_RGB565 = 1 << 0,
    AI_FB_FLAG_TIMESTAMP = 1 << 1
};

enum {
    AI_FB_PULL_GET_LATEST = 1,
    AI_FB_PULL_RUN_FRAMES_AND_GET = 2
};

static int ai_fb_push_server_fd = -1;
static int ai_fb_push_client_fd = -1;
static int ai_fb_pull_server_fd = -1;
static int ai_fb_pull_client_fd = -1;

static int ai_fb_push_enabled = 1;
static int ai_fb_pull_enabled = 1;
static int ai_fb_fps_cap = 0;             /* 0 = uncapped */
static int ai_fb_send_every_n_frames = 1; /* 1 = send all */
static int ai_fb_change_triggered = 0;

static UBYTE *ai_fb_buf_a = NULL;
static UBYTE *ai_fb_buf_b = NULL;
static UBYTE *ai_fb_latest_buf = NULL;
static UBYTE *ai_fb_write_buf = NULL;
static ULONG ai_fb_buf_size = 0;

static int ai_fb_width = 0;
static int ai_fb_height = 0;
static ULONG ai_fb_stride = 0;
static ULONG ai_fb_frame_no = 0;
static ULONG ai_fb_latest_crc32 = 0;
static unsigned long long ai_fb_latest_timestamp_us = 0;

static int ai_fb_have_last_push_crc = 0;
static ULONG ai_fb_last_push_crc = 0;
static unsigned long long ai_fb_last_push_us = 0;
static UBYTE *ai_fb_push_tx_buf = NULL;
static ULONG ai_fb_push_tx_buf_size = 0;
static ULONG ai_fb_push_tx_len = 0;
static ULONG ai_fb_push_tx_sent = 0;

static UBYTE ai_fb_pull_rx_buf[AI_FB_PULL_REQ_SIZE];
static int ai_fb_pull_rx_len = 0;

static int ai_fb_pull_waiting = 0;
static ULONG ai_fb_pull_target_frame = 0;
static int ai_fb_pull_restore_pause = 0;

static unsigned long long monotonic_us(void);
static void put_u16le(UBYTE *dst, UWORD value);
static void put_u32le(UBYTE *dst, ULONG value);
static void put_u64le(UBYTE *dst, unsigned long long value);
static UWORD get_u16le(const UBYTE *src);
static ULONG get_u32le(const UBYTE *src);
static int set_nonblocking(int fd);
static int create_unix_server_socket(const char *path);
static int send_all_blocking(int fd, const UBYTE *data, ULONG len);
static int send_all_nonblocking(int fd, const UBYTE *data, ULONG len);
static void ai_fb_close_push_client(void);
static void ai_fb_close_pull_client(void);
static void ai_fb_accept_push_client(void);
static void ai_fb_accept_pull_client(void);
static int ai_fb_send_error(int fd, UWORD code, const char *msg, int nonblocking);
static void ai_fb_build_frame_header(UBYTE *header);
static int ai_fb_send_frame(int fd, int nonblocking);
static int ai_fb_push_try_flush(void);
static int ai_fb_push_queue_latest(void);
static void ai_fb_process_pull_request(const UBYTE *req);
static void ai_fb_poll_pull_requests(void);
static void ai_fb_poll_sockets(void);
static int ai_fb_ensure_buffers(int width, int height);
static void ai_fb_convert_surface_to_rgb565(const void *pixels, int width, int height, int pitch,
                                            int bits_per_pixel, unsigned int rmask,
                                            unsigned int gmask, unsigned int bmask);
static int ai_fb_init(void);
static void ai_fb_shutdown(void);

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

static unsigned long long monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (unsigned long long) time(NULL) * 1000000ULL;
    }
    return (unsigned long long) ts.tv_sec * 1000000ULL + (unsigned long long) ts.tv_nsec / 1000ULL;
}

static void put_u16le(UBYTE *dst, UWORD value)
{
    dst[0] = (UBYTE) (value & 0xff);
    dst[1] = (UBYTE) ((value >> 8) & 0xff);
}

static void put_u32le(UBYTE *dst, ULONG value)
{
    dst[0] = (UBYTE) (value & 0xff);
    dst[1] = (UBYTE) ((value >> 8) & 0xff);
    dst[2] = (UBYTE) ((value >> 16) & 0xff);
    dst[3] = (UBYTE) ((value >> 24) & 0xff);
}

static void put_u64le(UBYTE *dst, unsigned long long value)
{
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (UBYTE) ((value >> (8 * i)) & 0xff);
    }
}

static UWORD get_u16le(const UBYTE *src)
{
    return (UWORD) (src[0] | (src[1] << 8));
}

static ULONG get_u32le(const UBYTE *src)
{
    return (ULONG) src[0]
         | ((ULONG) src[1] << 8)
         | ((ULONG) src[2] << 16)
         | ((ULONG) src[3] << 24);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return FALSE;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int create_unix_server_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (!set_nonblocking(fd)) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all_impl(int fd, const UBYTE *data, ULONG len, int nonblocking)
{
    ULONG sent = 0;
    int send_flags = 0;
#ifdef MSG_DONTWAIT
    if (nonblocking) {
        send_flags |= MSG_DONTWAIT;
    }
#endif
#ifdef MSG_NOSIGNAL
    send_flags |= MSG_NOSIGNAL;
#endif

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, send_flags);
        if (n > 0) {
            sent += (ULONG) n;
            continue;
        }
        if (n == 0) {
            return FALSE;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!nonblocking) {
                usleep(1000);
                continue;
            }
            return FALSE;
        }
        return FALSE;
    }
    return TRUE;
}

static int send_all_nonblocking(int fd, const UBYTE *data, ULONG len)
{
    return send_all_impl(fd, data, len, TRUE);
}

static int send_all_blocking(int fd, const UBYTE *data, ULONG len)
{
    return send_all_impl(fd, data, len, FALSE);
}

static void ai_fb_close_push_client(void)
{
    if (ai_fb_push_client_fd >= 0) {
        close(ai_fb_push_client_fd);
        ai_fb_push_client_fd = -1;
    }
    ai_fb_push_tx_len = 0;
    ai_fb_push_tx_sent = 0;
}

static void ai_fb_close_pull_client(void)
{
    if (ai_fb_pull_client_fd >= 0) {
        close(ai_fb_pull_client_fd);
        ai_fb_pull_client_fd = -1;
    }
    ai_fb_pull_rx_len = 0;
    if (ai_fb_pull_waiting) {
        ai_paused = ai_fb_pull_restore_pause;
    }
    ai_fb_pull_waiting = 0;
}

static void ai_fb_accept_push_client(void)
{
    int client;

    if (ai_fb_push_server_fd < 0) {
        return;
    }
    client = accept(ai_fb_push_server_fd, NULL, NULL);
    if (client < 0) {
        return;
    }
    set_nonblocking(client);
    if (ai_fb_push_client_fd >= 0) {
        close(ai_fb_push_client_fd);
    }
    ai_fb_push_client_fd = client;
    ai_fb_have_last_push_crc = 0;
    ai_fb_push_tx_len = 0;
    ai_fb_push_tx_sent = 0;
    Log_print("AI: Frame push client connected");
}

static void ai_fb_accept_pull_client(void)
{
    int client;

    if (ai_fb_pull_server_fd < 0) {
        return;
    }
    client = accept(ai_fb_pull_server_fd, NULL, NULL);
    if (client < 0) {
        return;
    }
    set_nonblocking(client);
    if (ai_fb_pull_client_fd >= 0) {
        close(ai_fb_pull_client_fd);
    }
    ai_fb_pull_client_fd = client;
    ai_fb_pull_rx_len = 0;
    if (ai_fb_pull_waiting) {
        ai_paused = ai_fb_pull_restore_pause;
        ai_fb_pull_waiting = 0;
    }
    Log_print("AI: Frame pull client connected");
}

static int ai_fb_send_error(int fd, UWORD code, const char *msg, int nonblocking)
{
    UBYTE header[12];
    ULONG msg_len = (ULONG) strlen(msg);
    memcpy(header, "A8ER", 4);
    put_u16le(header + 4, 1);
    put_u16le(header + 6, code);
    put_u32le(header + 8, msg_len);
    if ((nonblocking ? !send_all_nonblocking(fd, header, sizeof(header))
                     : !send_all_blocking(fd, header, sizeof(header)))) {
        return FALSE;
    }
    if (msg_len > 0 && (nonblocking ? !send_all_nonblocking(fd, (const UBYTE *) msg, msg_len)
                                    : !send_all_blocking(fd, (const UBYTE *) msg, msg_len))) {
        return FALSE;
    }
    return TRUE;
}

static int ai_fb_send_frame(int fd, int nonblocking)
{
    UBYTE header[AI_FB_HEADER_SIZE];
    ULONG payload_len;

    if (ai_fb_latest_buf == NULL || ai_fb_width <= 0 || ai_fb_height <= 0 || ai_fb_stride == 0) {
        return ai_fb_send_error(fd, 2, "NO_FRAME", nonblocking);
    }

    payload_len = ai_fb_stride * (ULONG) ai_fb_height;
    ai_fb_build_frame_header(header);

    if ((nonblocking ? !send_all_nonblocking(fd, header, sizeof(header))
                     : !send_all_blocking(fd, header, sizeof(header)))) {
        return FALSE;
    }
    if ((nonblocking ? !send_all_nonblocking(fd, ai_fb_latest_buf, payload_len)
                     : !send_all_blocking(fd, ai_fb_latest_buf, payload_len))) {
        return FALSE;
    }
    return TRUE;
}

static void ai_fb_build_frame_header(UBYTE *header)
{
    ULONG payload_len = ai_fb_stride * (ULONG) ai_fb_height;

    memcpy(header, "A8FB", 4);
    put_u16le(header + 4, 1);
    put_u16le(header + 6, AI_FB_FLAG_RGB565 | AI_FB_FLAG_TIMESTAMP);
    put_u16le(header + 8, (UWORD) ai_fb_width);
    put_u16le(header + 10, (UWORD) ai_fb_height);
    put_u32le(header + 12, ai_fb_stride);
    put_u32le(header + 16, ai_fb_frame_no);
    put_u32le(header + 20, payload_len);
    put_u64le(header + 24, ai_fb_latest_timestamp_us);
    put_u32le(header + 32, ai_fb_latest_crc32);
}

static int ai_fb_push_try_flush(void)
{
    if (ai_fb_push_client_fd < 0) {
        return FALSE;
    }

    while (ai_fb_push_tx_sent < ai_fb_push_tx_len) {
        ssize_t n = send(ai_fb_push_client_fd,
                         ai_fb_push_tx_buf + ai_fb_push_tx_sent,
                         ai_fb_push_tx_len - ai_fb_push_tx_sent,
#ifdef MSG_NOSIGNAL
                         MSG_DONTWAIT | MSG_NOSIGNAL
#else
                         MSG_DONTWAIT
#endif
                         );
        if (n > 0) {
            ai_fb_push_tx_sent += (ULONG) n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return TRUE;
        }
        ai_fb_close_push_client();
        return FALSE;
    }

    ai_fb_push_tx_len = 0;
    ai_fb_push_tx_sent = 0;
    return TRUE;
}

static int ai_fb_push_queue_latest(void)
{
    ULONG payload_len;
    ULONG needed;

    if (ai_fb_latest_buf == NULL || ai_fb_width <= 0 || ai_fb_height <= 0 || ai_fb_stride == 0) {
        return FALSE;
    }

    payload_len = ai_fb_stride * (ULONG) ai_fb_height;
    needed = AI_FB_HEADER_SIZE + payload_len;
    if (needed > ai_fb_push_tx_buf_size) {
        UBYTE *new_buf = (UBYTE *) realloc(ai_fb_push_tx_buf, needed);
        if (new_buf == NULL) {
            return FALSE;
        }
        ai_fb_push_tx_buf = new_buf;
        ai_fb_push_tx_buf_size = needed;
    }

    ai_fb_build_frame_header(ai_fb_push_tx_buf);
    memcpy(ai_fb_push_tx_buf + AI_FB_HEADER_SIZE, ai_fb_latest_buf, payload_len);
    ai_fb_push_tx_len = needed;
    ai_fb_push_tx_sent = 0;
    return TRUE;
}

static void ai_fb_process_pull_request(const UBYTE *req)
{
    UWORD version;
    UWORD command;
    ULONG arg0;

    if (memcmp(req, "A8RQ", 4) != 0) {
        if (!ai_fb_send_error(ai_fb_pull_client_fd, 1, "BAD_MAGIC", FALSE)) {
            ai_fb_close_pull_client();
        }
        return;
    }

    version = get_u16le(req + 4);
    command = get_u16le(req + 6);
    arg0 = get_u32le(req + 8);
    if (version != 1) {
        if (!ai_fb_send_error(ai_fb_pull_client_fd, 1, "BAD_VERSION", FALSE)) {
            ai_fb_close_pull_client();
        }
        return;
    }

    if (!ai_fb_pull_enabled) {
        if (!ai_fb_send_error(ai_fb_pull_client_fd, 4, "PULL_DISABLED", FALSE)) {
            ai_fb_close_pull_client();
        }
        return;
    }

    if (command == AI_FB_PULL_GET_LATEST) {
        if (!ai_fb_send_frame(ai_fb_pull_client_fd, FALSE)) {
            ai_fb_close_pull_client();
        }
        return;
    }

    if (command == AI_FB_PULL_RUN_FRAMES_AND_GET) {
        if (arg0 == 0) {
            if (!ai_fb_send_frame(ai_fb_pull_client_fd, FALSE)) {
                ai_fb_close_pull_client();
            }
            return;
        }

        if (ai_fb_pull_waiting) {
            if (!ai_fb_send_error(ai_fb_pull_client_fd, 5, "BUSY", FALSE)) {
                ai_fb_close_pull_client();
            }
            return;
        }

        ai_fb_pull_waiting = TRUE;
        ai_fb_pull_target_frame = ai_fb_frame_no + arg0;
        ai_fb_pull_restore_pause = ai_paused;
        ai_paused = 0;
        return;
    }

    if (!ai_fb_send_error(ai_fb_pull_client_fd, 3, "BAD_COMMAND", FALSE)) {
        ai_fb_close_pull_client();
    }
}

static void ai_fb_poll_pull_requests(void)
{
    while (ai_fb_pull_client_fd >= 0) {
        ssize_t r;
        int handled = FALSE;

        if (ai_fb_pull_rx_len < AI_FB_PULL_REQ_SIZE) {
            r = recv(ai_fb_pull_client_fd,
                     ai_fb_pull_rx_buf + ai_fb_pull_rx_len,
                     AI_FB_PULL_REQ_SIZE - ai_fb_pull_rx_len,
                     MSG_DONTWAIT);
            if (r > 0) {
                ai_fb_pull_rx_len += (int) r;
            }
            else if (r == 0) {
                ai_fb_close_pull_client();
                break;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ai_fb_close_pull_client();
                break;
            }
        }

        while (ai_fb_pull_client_fd >= 0 && ai_fb_pull_rx_len >= AI_FB_PULL_REQ_SIZE) {
            UBYTE req[AI_FB_PULL_REQ_SIZE];
            memcpy(req, ai_fb_pull_rx_buf, AI_FB_PULL_REQ_SIZE);
            if (ai_fb_pull_rx_len > AI_FB_PULL_REQ_SIZE) {
                memmove(ai_fb_pull_rx_buf,
                        ai_fb_pull_rx_buf + AI_FB_PULL_REQ_SIZE,
                        ai_fb_pull_rx_len - AI_FB_PULL_REQ_SIZE);
            }
            ai_fb_pull_rx_len -= AI_FB_PULL_REQ_SIZE;
            ai_fb_process_pull_request(req);
            handled = TRUE;
        }

        if (!handled) {
            break;
        }
    }
}

static void ai_fb_poll_sockets(void)
{
    if (!AI_enabled) {
        return;
    }

    if (ai_fb_push_enabled) {
        ai_fb_accept_push_client();
    }
    else {
        ai_fb_close_push_client();
    }

    if (ai_fb_pull_enabled) {
        ai_fb_accept_pull_client();
        ai_fb_poll_pull_requests();
    }
    else {
        ai_fb_close_pull_client();
    }
}

static int ai_fb_ensure_buffers(int width, int height)
{
    ULONG needed;

    if (width <= 0 || height <= 0) {
        return FALSE;
    }
    if (width > 8192 || height > 8192) {
        return FALSE;
    }

    needed = (ULONG) width * (ULONG) height * 2U;
    if (needed == 0) {
        return FALSE;
    }

    if (needed != ai_fb_buf_size) {
        UBYTE *new_a = (UBYTE *) malloc(needed);
        UBYTE *new_b = (UBYTE *) malloc(needed);
        if (new_a == NULL || new_b == NULL) {
            free(new_a);
            free(new_b);
            return FALSE;
        }
        free(ai_fb_buf_a);
        free(ai_fb_buf_b);
        ai_fb_buf_a = new_a;
        ai_fb_buf_b = new_b;
        ai_fb_latest_buf = ai_fb_buf_a;
        ai_fb_write_buf = ai_fb_buf_b;
        ai_fb_buf_size = needed;
        ai_fb_have_last_push_crc = 0;
    }

    ai_fb_width = width;
    ai_fb_height = height;
    ai_fb_stride = (ULONG) width * 2U;
    return TRUE;
}

static int color_shift(unsigned int mask)
{
    int shift = 0;
    if (mask == 0) {
        return 0;
    }
    while ((mask & 1U) == 0U) {
        shift++;
        mask >>= 1;
    }
    return shift;
}

static int color_bits(unsigned int mask)
{
    int bits = 0;
    if (mask == 0) {
        return 0;
    }
    while ((mask & 1U) == 0U) {
        mask >>= 1;
    }
    while (mask & 1U) {
        bits++;
        mask >>= 1;
    }
    return bits;
}

static UBYTE color_to_8bit(unsigned int raw, unsigned int mask, int shift, int bits)
{
    unsigned int v;
    unsigned int maxv;
    if (mask == 0 || bits <= 0) {
        return 0;
    }
    v = (raw & mask) >> shift;
    if (bits >= 31) {
        maxv = 0xffffffffU;
    }
    else {
        maxv = (1U << bits) - 1U;
    }
    if (maxv == 0) {
        return 0;
    }
    return (UBYTE) ((v * 255U + maxv / 2U) / maxv);
}

static void ai_fb_convert_surface_to_rgb565(const void *pixels, int width, int height, int pitch,
                                            int bits_per_pixel, unsigned int rmask,
                                            unsigned int gmask, unsigned int bmask)
{
    int x, y;
    int rshift = color_shift(rmask);
    int gshift = color_shift(gmask);
    int bshift = color_shift(bmask);
    int rbits = color_bits(rmask);
    int gbits = color_bits(gmask);
    int bbits = color_bits(bmask);
    ULONG crc = 0xffffffffU;
    int bytes_per_pixel = (bits_per_pixel + 7) / 8;

    for (y = 0; y < height; y++) {
        const UBYTE *src = (const UBYTE *) pixels + y * pitch;
        UWORD *dst = (UWORD *) (ai_fb_write_buf + y * ai_fb_stride);

        if (bits_per_pixel == 16 && rmask == 0xF800U && gmask == 0x07E0U && bmask == 0x001FU) {
            memcpy(dst, src, width * 2U);
        }
        else if (bits_per_pixel == 16) {
            const UWORD *src16 = (const UWORD *) src;
            for (x = 0; x < width; x++) {
                unsigned int raw = src16[x];
                UBYTE r = color_to_8bit(raw, rmask, rshift, rbits);
                UBYTE g = color_to_8bit(raw, gmask, gshift, gbits);
                UBYTE b = color_to_8bit(raw, bmask, bshift, bbits);
                dst[x] = (UWORD) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
        else if (bits_per_pixel == 32) {
            const ULONG *src32 = (const ULONG *) src;
            for (x = 0; x < width; x++) {
                unsigned int raw = src32[x];
                UBYTE r = color_to_8bit(raw, rmask, rshift, rbits);
                UBYTE g = color_to_8bit(raw, gmask, gshift, gbits);
                UBYTE b = color_to_8bit(raw, bmask, bshift, bbits);
                dst[x] = (UWORD) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
        else {
            for (x = 0; x < width; x++) {
                unsigned int raw = 0;
                int b;
                const UBYTE *p = src + x * bytes_per_pixel;
                for (b = 0; b < bytes_per_pixel && b < 4; b++) {
                    raw |= ((unsigned int) p[b]) << (8 * b);
                }
                dst[x] = (UWORD) (((color_to_8bit(raw, rmask, rshift, rbits) >> 3) << 11)
                               | ((color_to_8bit(raw, gmask, gshift, gbits) >> 2) << 5)
                               | (color_to_8bit(raw, bmask, bshift, bbits) >> 3));
            }
        }

        crc = CRC32_Update(crc, (UBYTE const *) dst, width * 2U);
    }

    ai_fb_latest_crc32 = ~crc;
}

void AI_FrameStreamSubmitSurface(const void *pixels, int width, int height, int pitch,
                                 int bits_per_pixel, unsigned int rmask,
                                 unsigned int gmask, unsigned int bmask)
{
    UBYTE *tmp;
    unsigned long long now_us;

    if (!AI_enabled || pixels == NULL) {
        return;
    }
    if (bits_per_pixel <= 0) {
        return;
    }

    ai_fb_poll_sockets();

    if (!ai_fb_ensure_buffers(width, height)) {
        return;
    }

    ai_fb_convert_surface_to_rgb565(pixels, width, height, pitch, bits_per_pixel, rmask, gmask, bmask);
    now_us = monotonic_us();

    tmp = ai_fb_latest_buf;
    ai_fb_latest_buf = ai_fb_write_buf;
    ai_fb_write_buf = tmp;

    ai_fb_latest_timestamp_us = now_us;
    ai_fb_frame_no++;

    if (ai_fb_pull_waiting && ai_fb_pull_client_fd >= 0 && ai_fb_frame_no >= ai_fb_pull_target_frame) {
        if (!ai_fb_send_frame(ai_fb_pull_client_fd, FALSE)) {
            ai_fb_close_pull_client();
        }
        else {
            ai_fb_pull_waiting = FALSE;
            ai_paused = ai_fb_pull_restore_pause;
        }
    }

    if (ai_fb_push_enabled && ai_fb_push_client_fd >= 0) {
        if (!ai_fb_push_try_flush()) {
            return;
        }
        if (ai_fb_push_tx_len > 0) {
            /* Keep stream framing intact: don't start a new frame until current frame is fully sent. */
            return;
        }
        if (ai_fb_send_every_n_frames > 1 && (ai_fb_frame_no % (ULONG) ai_fb_send_every_n_frames) != 0) {
            return;
        }
        if (ai_fb_fps_cap > 0) {
            unsigned long long min_interval = 1000000ULL / (unsigned long long) ai_fb_fps_cap;
            if (now_us < ai_fb_last_push_us + min_interval) {
                return;
            }
        }
        if (ai_fb_change_triggered && ai_fb_have_last_push_crc && ai_fb_last_push_crc == ai_fb_latest_crc32) {
            return;
        }

        if (!ai_fb_push_queue_latest()) {
            ai_fb_close_push_client();
            return;
        }
        ai_fb_last_push_us = now_us;
        ai_fb_last_push_crc = ai_fb_latest_crc32;
        ai_fb_have_last_push_crc = TRUE;
        ai_fb_push_try_flush();
    }
}

static int ai_fb_init(void)
{
    ai_fb_push_server_fd = create_unix_server_socket(AI_FB_PUSH_SOCKET_PATH);
    if (ai_fb_push_server_fd < 0) {
        Log_print("AI: Failed to create frame push socket %s: %s", AI_FB_PUSH_SOCKET_PATH, strerror(errno));
    }
    else {
        Log_print("AI: Frame push socket listening on %s", AI_FB_PUSH_SOCKET_PATH);
    }

    ai_fb_pull_server_fd = create_unix_server_socket(AI_FB_PULL_SOCKET_PATH);
    if (ai_fb_pull_server_fd < 0) {
        Log_print("AI: Failed to create frame pull socket %s: %s", AI_FB_PULL_SOCKET_PATH, strerror(errno));
    }
    else {
        Log_print("AI: Frame pull socket listening on %s", AI_FB_PULL_SOCKET_PATH);
    }

    return ai_fb_push_server_fd >= 0 || ai_fb_pull_server_fd >= 0;
}

static void ai_fb_shutdown(void)
{
    ai_fb_close_push_client();
    ai_fb_close_pull_client();

    if (ai_fb_push_server_fd >= 0) {
        close(ai_fb_push_server_fd);
        ai_fb_push_server_fd = -1;
    }
    if (ai_fb_pull_server_fd >= 0) {
        close(ai_fb_pull_server_fd);
        ai_fb_pull_server_fd = -1;
    }
    unlink(AI_FB_PUSH_SOCKET_PATH);
    unlink(AI_FB_PULL_SOCKET_PATH);

    free(ai_fb_buf_a);
    free(ai_fb_buf_b);
    free(ai_fb_push_tx_buf);
    ai_fb_buf_a = NULL;
    ai_fb_buf_b = NULL;
    ai_fb_push_tx_buf = NULL;
    ai_fb_latest_buf = NULL;
    ai_fb_write_buf = NULL;
    ai_fb_buf_size = 0;
    ai_fb_width = 0;
    ai_fb_height = 0;
    ai_fb_stride = 0;
    ai_fb_frame_no = 0;
    ai_fb_latest_crc32 = 0;
    ai_fb_latest_timestamp_us = 0;
    ai_fb_have_last_push_crc = 0;
    ai_fb_last_push_crc = 0;
    ai_fb_last_push_us = 0;
    ai_fb_pull_rx_len = 0;
    ai_fb_pull_waiting = 0;
    ai_fb_push_tx_buf_size = 0;
    ai_fb_push_tx_len = 0;
    ai_fb_push_tx_sent = 0;
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
        if (!path[0]) {
            snprintf(path, sizeof(path), "/tmp/atari800_ai_%ld.png", (long)time(NULL));
        }
        /* Debug: log screen dimensions */
        Log_print("AI screenshot: path=%s vis=%d,%d-%d,%d", path,
            Screen_visible_x1, Screen_visible_y1, Screen_visible_x2, Screen_visible_y2);
        if (Screen_SaveScreenshot(path, 0)) {
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"ok\",\"path\":\"%s\"}", path);
        } else {
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"error\",\"msg\":\"Failed to save screenshot vis=%d,%d-%d,%d\"}",
                Screen_visible_x1, Screen_visible_y1, Screen_visible_x2, Screen_visible_y2);
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
    else if (strcmp(cmd_type, "video.enable_push") == 0) {
        ai_fb_push_enabled = TRUE;
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "video.disable_push") == 0) {
        ai_fb_push_enabled = FALSE;
        ai_fb_close_push_client();
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "video.enable_pull") == 0) {
        ai_fb_pull_enabled = TRUE;
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "video.disable_pull") == 0) {
        ai_fb_pull_enabled = FALSE;
        ai_fb_close_pull_client();
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "video.push.set_fps_cap") == 0) {
        int fps_cap = json_get_int(cmd, "value", 0);
        if (fps_cap < 0) {
            fps_cap = 0;
        }
        ai_fb_fps_cap = fps_cap;
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"fps_cap\":%d}", ai_fb_fps_cap);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "video.push.set_frameskip") == 0) {
        int frame_skip = json_get_int(cmd, "n", 1);
        if (frame_skip < 1) {
            frame_skip = 1;
        }
        ai_fb_send_every_n_frames = frame_skip;
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"send_every_n_frames\":%d}", ai_fb_send_every_n_frames);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "video.push.enable_change_triggered") == 0) {
        ai_fb_change_triggered = json_get_bool(cmd, "enabled", TRUE);
        AI_SendResponse("{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd_type, "video.status") == 0) {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\","
            "\"push_socket\":\"%s\",\"pull_socket\":\"%s\","
            "\"push_enabled\":%s,\"pull_enabled\":%s,"
            "\"fps_cap\":%d,\"send_every_n_frames\":%d,"
            "\"change_triggered\":%s,"
            "\"width\":%d,\"height\":%d,\"stride\":%u,\"frame_no\":%u}",
            AI_FB_PUSH_SOCKET_PATH, AI_FB_PULL_SOCKET_PATH,
            ai_fb_push_enabled ? "true" : "false",
            ai_fb_pull_enabled ? "true" : "false",
            ai_fb_fps_cap, ai_fb_send_every_n_frames,
            ai_fb_change_triggered ? "true" : "false",
            ai_fb_width, ai_fb_height, ai_fb_stride, ai_fb_frame_no);
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
                        MEMORY_mem[(UWORD)addr++] = (UBYTE)val;  /* Direct write - bypasses attribute check */
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
        ai_fb_init();
        Log_print("AI: Interface enabled");
    }

    return TRUE;
}

/* Cleanup */
void AI_Exit(void) {
    ai_fb_shutdown();

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

    ai_fb_poll_sockets();
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
        ai_fb_poll_sockets();
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
