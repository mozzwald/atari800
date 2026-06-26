/*
 * ai_interface.c - AI/Automation Interface for Atari800
 *
 * Copyright (c) 2026 - AI Interface Extension
 * Licensed under GPL-2.0-or-later
 */

#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

#include "ai_interface.h"
#include "ai_protocol.h"
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
#include "monitor.h"
#include "log.h"
#include "crc32.h"

/* Configuration */
int AI_enabled = 0;
int AI_debug_port = 0;
char AI_socket_path[256] = AI_SOCKET_PATH;
static char AI_video_push_socket_path[256] = "/tmp/atari800-fb-push.sock";
static char AI_video_pull_socket_path[256] = "/tmp/atari800-fb-pull.sock";
static char AI_artifact_dir[PATH_MAX] = "/tmp/atari800_ai_artifacts";
static char AI_artifact_dir_resolved[PATH_MAX] = "/tmp/atari800_ai_artifacts";
static int AI_unsafe_paths = 0;

/* AI input overrides (-1 = no override) */
int AI_joy_override[4] = {-1, -1, -1, -1};
int AI_trig_override[4] = {-1, -1, -1, -1};

/* State */
static int ai_server_fd = -1;
static int ai_client_fd = -1;
static int ai_paused = 1;  /* Start paused, waiting for AI */
static int ai_frames_to_run = 0;
static int ai_frames_requested = 0;
static int ai_steps_to_run = 0;
static int ai_steps_requested = 0;
static int AI_command_timeout_ms = 30000;
static unsigned long long ai_command_deadline_us = 0;
static int ai_async_response_pending = 0;
static char ai_stopped_reason[32] = "manual_pause";
static int ai_instruction_steps_to_run = 0;
static int ai_instruction_steps_requested = 0;
static int ai_break_pc_enabled = 0;
static UWORD ai_break_pc_addr = 0;
static int ai_break_brk_enabled = 0;
static int ai_last_breakpoint_id = -1;
static UWORD ai_last_stop_pc = 0;

/* Debug output buffer */
#define AI_DEBUG_BUFFER_SIZE 4096
static UBYTE ai_debug_buffer[AI_DEBUG_BUFFER_SIZE];
static int ai_debug_buffer_pos = 0;

/* Response buffer */
static char ai_response[AI_MAX_RESPONSE];

/* Frame streaming (RGB565 over UNIX sockets) */
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
static void ai_set_stopped_reason(const char *reason);
static void ai_send_debugger_break_response(const char *reason);
static int ai_send_monitor_capture(void (*writer)(FILE *fp, void *ctx), void *ctx);

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
    ai_fb_push_server_fd = create_unix_server_socket(AI_video_push_socket_path);
    if (ai_fb_push_server_fd < 0) {
        Log_print("AI: Failed to create frame push socket %s: %s", AI_video_push_socket_path, strerror(errno));
    }
    else {
        Log_print("AI: Frame push socket listening on %s", AI_video_push_socket_path);
    }

    ai_fb_pull_server_fd = create_unix_server_socket(AI_video_pull_socket_path);
    if (ai_fb_pull_server_fd < 0) {
        Log_print("AI: Failed to create frame pull socket %s: %s", AI_video_pull_socket_path, strerror(errno));
    }
    else {
        Log_print("AI: Frame pull socket listening on %s", AI_video_pull_socket_path);
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
        unlink(AI_video_push_socket_path);
        unlink(AI_video_pull_socket_path);

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
    int len;
    char header[32];

    if (ai_client_fd < 0) return;

    len = strlen(json);
    snprintf(header, sizeof(header), "%d\n", len);
    if (!send_all_blocking(ai_client_fd, (const UBYTE *)header, strlen(header)) ||
        !send_all_blocking(ai_client_fd, (const UBYTE *)json, len)) {
        close(ai_client_fd);
        ai_client_fd = -1;
        Log_print("AI: Client disconnected during response");
    }
}

/* Debug write hook - called when program writes to debug port */
void AI_DebugWrite(UBYTE byte) {
    if (ai_debug_buffer_pos < AI_DEBUG_BUFFER_SIZE) {
        ai_debug_buffer[ai_debug_buffer_pos++] = byte;
    }
}

int AI_DebuggerShouldBreakPC(UWORD pc)
{
    return AI_enabled && ai_break_pc_enabled && pc == ai_break_pc_addr;
}

int AI_DebuggerShouldBreakBRK(void)
{
    return AI_enabled && ai_break_brk_enabled;
}

int AI_DebuggerBreak(const char *reason, int breakpoint_id)
{
    if (!AI_enabled)
        return FALSE;

    if (strcmp(reason, "instruction_limit") == 0) {
        if (ai_instruction_steps_to_run > 0)
            ai_instruction_steps_to_run--;
        if (ai_instruction_steps_to_run > 0) {
            return TRUE;
        }
#ifdef MONITOR_BREAK
        MONITOR_break_step = FALSE;
#endif
        ai_paused = 1;
        ai_last_breakpoint_id = breakpoint_id;
        ai_set_stopped_reason("instruction_limit");
        ai_command_deadline_us = 0;
        if (ai_async_response_pending) {
            ai_async_response_pending = 0;
            ai_send_debugger_break_response("instruction_limit");
        }
        return TRUE;
    }

    if (strcmp(reason, "breakpoint_pc") == 0 || strcmp(reason, "breakpoint_brk") == 0) {
#ifdef MONITOR_BREAK
        MONITOR_break_step = FALSE;
#endif
        ai_frames_to_run = 0;
        ai_steps_to_run = 0;
        ai_instruction_steps_to_run = 0;
        ai_command_deadline_us = 0;
        ai_paused = 1;
        ai_last_breakpoint_id = breakpoint_id;
        ai_set_stopped_reason(reason);
        if (ai_async_response_pending) {
            ai_async_response_pending = 0;
            ai_send_debugger_break_response(reason);
        }
        return TRUE;
    }

    return FALSE;
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

static int ai_append(char *buf, size_t bufsize, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    int written;

    if (*pos >= bufsize)
        return FALSE;
    va_start(ap, fmt);
    written = vsnprintf(buf + *pos, bufsize - *pos, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= bufsize - *pos)
        return FALSE;
    *pos += (size_t)written;
    return TRUE;
}

static void ai_send_error(const char *code, const char *message, const char *field)
{
    size_t pos = 0;

    ai_response[0] = '\0';
    if (!ai_append(ai_response, sizeof(ai_response), &pos, "{\"status\":\"error\",\"code\":"))
        return;
    if (!AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, code != NULL ? code : "INTERNAL_ERROR"))
        return;
    if (!ai_append(ai_response, sizeof(ai_response), &pos, ",\"message\":"))
        return;
    if (!AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, message != NULL ? message : "internal error"))
        return;
    if (field != NULL && field[0] != '\0') {
        if (!ai_append(ai_response, sizeof(ai_response), &pos, ",\"details\":{\"field\":"))
            return;
        if (!AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, field))
            return;
        if (!ai_append(ai_response, sizeof(ai_response), &pos, "}"))
            return;
    }
    if (!ai_append(ai_response, sizeof(ai_response), &pos, "}"))
        return;
    AI_SendResponse(ai_response);
}

static void ai_send_validation_error(int rc, const char *field, int required)
{
    char msg[128];
    const char *reason = "is invalid";

    if (rc == AI_JSON_MISSING)
        reason = "is required";
    else if (rc == AI_JSON_BAD_TYPE)
        reason = "has the wrong type";
    else if (rc == AI_JSON_BAD_VALUE)
        reason = "is out of range";
    else if (rc == AI_JSON_NO_SPACE)
        reason = "is too large";
    snprintf(msg, sizeof(msg), "%s %s", field != NULL ? field : "field", reason);
    ai_send_error(AI_JSON_ErrorCode(rc, required), msg, field);
}

static void ai_send_ok(void)
{
    AI_SendResponse("{\"status\":\"ok\"}");
}

static void ai_send_ok_path(const char *path)
{
    size_t pos = 0;

    ai_response[0] = '\0';
    if (!ai_append(ai_response, sizeof(ai_response), &pos, "{\"status\":\"ok\",\"path\":"))
        return;
    if (!AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, path))
        return;
    if (!ai_append(ai_response, sizeof(ai_response), &pos, "}"))
        return;
    AI_SendResponse(ai_response);
}

static void ai_set_stopped_reason(const char *reason)
{
    if (reason == NULL || reason[0] == '\0')
        reason = "manual_pause";
    strncpy(ai_stopped_reason, reason, sizeof(ai_stopped_reason) - 1);
    ai_stopped_reason[sizeof(ai_stopped_reason) - 1] = '\0';
    CPU_GetStatus();
    ai_last_stop_pc = CPU_regPC;
}

static void ai_append_cpu_state(char *buf, size_t bufsize, size_t *pos)
{
    CPU_GetStatus();
    ai_append(buf, bufsize, pos,
        "\"cpu\":{\"pc\":%u,\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,\"p\":%u,"
        "\"n\":%d,\"v\":%d,\"b\":%d,\"d\":%d,\"i\":%d,\"z\":%d,\"c\":%d}",
        CPU_regPC, CPU_regA, CPU_regX, CPU_regY, CPU_regS, CPU_regP,
        (CPU_regP & CPU_N_FLAG) ? 1 : 0,
        (CPU_regP & CPU_V_FLAG) ? 1 : 0,
        (CPU_regP & CPU_B_FLAG) ? 1 : 0,
        (CPU_regP & CPU_D_FLAG) ? 1 : 0,
        (CPU_regP & CPU_I_FLAG) ? 1 : 0,
        (CPU_regP & CPU_Z_FLAG) ? 1 : 0,
        (CPU_regP & CPU_C_FLAG) ? 1 : 0);
}

static void ai_send_debugger_state(const char *status)
{
    size_t pos = 0;

    ai_response[0] = '\0';
    ai_append(ai_response, sizeof(ai_response), &pos,
        "{\"status\":\"%s\",\"debugger\":{\"paused\":%s,\"stopped_reason\":",
        status != NULL ? status : "ok", ai_paused ? "true" : "false");
    AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, ai_stopped_reason);
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"last_breakpoint_id\":%d,\"last_stop_pc\":%u,"
        "\"breakpoints\":{\"pc\":{\"enabled\":%s,\"addr\":%u},\"brk\":{\"enabled\":%s}},"
        "\"instruction_step\":{\"remaining\":%d,\"requested\":%d},"
        "\"capabilities\":{\"instruction_step\":",
        ai_last_breakpoint_id, ai_last_stop_pc,
        ai_break_pc_enabled ? "true" : "false", ai_break_pc_addr,
        ai_break_brk_enabled ? "true" : "false",
        ai_instruction_steps_to_run, ai_instruction_steps_requested);
#ifdef MONITOR_BREAK
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"monitor_break\":");
#ifdef MONITOR_BREAK
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"monitor_breakpoints\":");
#ifdef MONITOR_BREAKPOINTS
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"monitor_assembler\":");
#ifdef MONITOR_ASSEMBLER
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"monitor_hints\":");
#ifdef MONITOR_HINTS
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"monitor_trace\":");
#ifdef MONITOR_TRACE
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"monitor_profile\":");
#ifdef MONITOR_PROFILE
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        ",\"history\":");
#ifdef MONITOR_BREAK
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos, "}},");
    ai_append_cpu_state(ai_response, sizeof(ai_response), &pos);
    ai_append(ai_response, sizeof(ai_response), &pos, "}");
    AI_SendResponse(ai_response);
}

static void ai_send_debugger_break_response(const char *reason)
{
    ai_set_stopped_reason(reason);
    ai_send_debugger_state("ok");
}

typedef struct {
    UWORD addr;
    int count;
} ai_monitor_range_ctx;

static char ai_screen_to_asc(char c)
{
    char bit7 = c & 0x80;
    c &= 0x7f;
    if (c < 32)
        c += 64;
    else if (c < 96)
        c -= 32;
    return c | bit7;
}

static void ai_write_history(FILE *fp, void *ctx)
{
    (void)ctx;
    MONITOR_ShowHistory(fp);
}

static void ai_write_jumps(FILE *fp, void *ctx)
{
    (void)ctx;
    MONITOR_ShowLastJumps(fp);
}

static void ai_write_stack(FILE *fp, void *ctx)
{
    ai_monitor_range_ctx *range = (ai_monitor_range_ctx *)ctx;
    CPU_GetStatus();
    MONITOR_ShowStack(fp, CPU_regS, range != NULL ? range->count : 16);
}

static void ai_write_disassemble(FILE *fp, void *ctx)
{
    ai_monitor_range_ctx *range = (ai_monitor_range_ctx *)ctx;
    MONITOR_Disassemble(fp, range->addr, range->count);
}

static void ai_write_disassemble_loop(FILE *fp, void *ctx)
{
    ai_monitor_range_ctx *range = (ai_monitor_range_ctx *)ctx;
    MONITOR_DisassembleLoop(fp, range->addr);
}

static void ai_write_dlist(FILE *fp, void *ctx)
{
    ai_monitor_range_ctx *range = (ai_monitor_range_ctx *)ctx;
    MONITOR_ShowDisplayList(fp, range->addr, range->count);
}

static void ai_write_labels(FILE *fp, void *ctx)
{
    ai_monitor_range_ctx *range = (ai_monitor_range_ctx *)ctx;
#ifdef MONITOR_HINTS
    MONITOR_ShowLabels(fp, TRUE, range != NULL ? range->count : 256);
#else
    (void)fp;
    (void)range;
#endif
}

static int ai_send_monitor_capture(void (*writer)(FILE *fp, void *ctx), void *ctx)
{
    FILE *fp;
    char line[512];
    size_t pos = 0;
    int count = 0;

    fp = tmpfile();
    if (fp == NULL) {
        ai_send_error("IO_ERROR", "failed to allocate monitor capture buffer", NULL);
        return FALSE;
    }
    writer(fp, ctx);
    fflush(fp);
    rewind(fp);

    ai_response[0] = '\0';
    if (!ai_append(ai_response, sizeof(ai_response), &pos, "{\"status\":\"ok\",\"lines\":[")) {
        fclose(fp);
        return FALSE;
    }
    while (fgets(line, sizeof(line), fp) != NULL && pos < sizeof(ai_response) - 256) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (count > 0 && !ai_append(ai_response, sizeof(ai_response), &pos, ",")) {
            fclose(fp);
            return FALSE;
        }
        if (!AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, line)) {
            fclose(fp);
            return FALSE;
        }
        count++;
    }
    fclose(fp);
    if (!ai_append(ai_response, sizeof(ai_response), &pos, "],\"count\":%d}", count))
        return FALSE;
    AI_SendResponse(ai_response);
    return TRUE;
}

static void ai_send_memory_search(int start_addr, int end_addr, const UBYTE *pattern, int pattern_len)
{
    size_t pos = 0;
    int found = 0;
    int a;

    ai_response[0] = '\0';
    if (pattern_len <= 0) {
        ai_send_error("BAD_ARGUMENT", "pattern must not be empty", "pattern");
        return;
    }
    ai_append(ai_response, sizeof(ai_response), &pos, "{\"status\":\"ok\",\"matches\":[");
    for (a = start_addr; a <= end_addr && found < 1024; a++) {
        int i;
        if (a + pattern_len - 1 > end_addr)
            break;
        for (i = 0; i < pattern_len; i++) {
            if (MEMORY_SafeGetByte((UWORD)(a + i)) != pattern[i])
                break;
        }
        if (i == pattern_len) {
            ai_append(ai_response, sizeof(ai_response), &pos, "%s%d", found ? "," : "", a);
            found++;
        }
    }
    ai_append(ai_response, sizeof(ai_response), &pos, "],\"count\":%d,\"truncated\":%s}", found, found >= 1024 ? "true" : "false");
    AI_SendResponse(ai_response);
}

#ifdef MONITOR_BREAKPOINTS
static const char *ai_breakpoint_condition_name(UWORD condition)
{
    switch (condition & ~(MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL | MONITOR_BREAKPOINT_GREATER)) {
    case MONITOR_BREAKPOINT_PC: return "PC";
    case MONITOR_BREAKPOINT_A: return "A";
    case MONITOR_BREAKPOINT_X: return "X";
    case MONITOR_BREAKPOINT_Y: return "Y";
    case MONITOR_BREAKPOINT_S: return "S";
    case MONITOR_BREAKPOINT_READ: return "READ";
    case MONITOR_BREAKPOINT_WRITE: return "WRITE";
    case MONITOR_BREAKPOINT_ACCESS: return "ACCESS";
    case MONITOR_BREAKPOINT_MEMORY: return "MEM";
    default: return "UNKNOWN";
    }
}

static int ai_breakpoint_operator_bits(const char *op)
{
    if (op == NULL || op[0] == '\0' || strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return MONITOR_BREAKPOINT_EQUAL;
    if (strcmp(op, "<") == 0)
        return MONITOR_BREAKPOINT_LESS;
    if (strcmp(op, ">") == 0)
        return MONITOR_BREAKPOINT_GREATER;
    if (strcmp(op, "<=") == 0)
        return MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL;
    if (strcmp(op, ">=") == 0)
        return MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL;
    if (strcmp(op, "!=") == 0)
        return MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER;
    return 0;
}

static int ai_breakpoint_type_bits(const char *type)
{
    if (strcmp(type, "PC") == 0) return MONITOR_BREAKPOINT_PC;
    if (strcmp(type, "A") == 0) return MONITOR_BREAKPOINT_A;
    if (strcmp(type, "X") == 0) return MONITOR_BREAKPOINT_X;
    if (strcmp(type, "Y") == 0) return MONITOR_BREAKPOINT_Y;
    if (strcmp(type, "S") == 0) return MONITOR_BREAKPOINT_S;
    if (strcmp(type, "READ") == 0) return MONITOR_BREAKPOINT_READ;
    if (strcmp(type, "WRITE") == 0) return MONITOR_BREAKPOINT_WRITE;
    if (strcmp(type, "ACCESS") == 0) return MONITOR_BREAKPOINT_ACCESS;
    if (strcmp(type, "MEM") == 0) return MONITOR_BREAKPOINT_MEMORY;
    return 0;
}

static int ai_parse_hex_value(const char *s, int *value)
{
    char *endptr;
    long parsed;
    if (s == NULL || s[0] == '\0')
        return FALSE;
    if (s[0] == '$')
        s++;
    parsed = strtol(s, &endptr, 16);
    if (*endptr != '\0' || parsed < 0 || parsed > 0xffff)
        return FALSE;
    *value = (int)parsed;
    return TRUE;
}

static int ai_parse_breakpoint_condition_string(const char *text, UWORD *condition, UWORD *value, UWORD *m_addr)
{
    char buf[96];
    char type[16];
    char op[3] = "=";
    char *p;
    char *v;
    int addr_value = 0;
    int parsed_value = 0;
    int type_bits;
    int op_bits;

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    p = buf;
    while (*p == ' ' || *p == '\t')
        p++;
    v = strpbrk(p, "<>!=");
    if (v == NULL)
        return FALSE;
    if (v[0] != '\0' && v[1] == '=') {
        op[0] = v[0];
        op[1] = '=';
        op[2] = '\0';
        *v++ = '\0';
        *v++ = '\0';
    }
    else {
        op[0] = *v;
        op[1] = '\0';
        *v++ = '\0';
    }
    if (strncmp(p, "MEM:", 4) == 0) {
        strcpy(type, "MEM");
        if (!ai_parse_hex_value(p + 4, &addr_value))
            return FALSE;
    }
    else {
        snprintf(type, sizeof(type), "%s", p);
    }
    type_bits = ai_breakpoint_type_bits(type);
    op_bits = ai_breakpoint_operator_bits(op);
    if (type_bits == 0 || op_bits == 0 || !ai_parse_hex_value(v, &parsed_value))
        return FALSE;
    *condition = (UWORD)(type_bits | op_bits);
    *value = (UWORD)parsed_value;
    *m_addr = (UWORD)addr_value;
    return TRUE;
}

static void ai_send_breakpoint_table(void)
{
    size_t pos = 0;
    int i;
    ai_response[0] = '\0';
    ai_append(ai_response, sizeof(ai_response), &pos,
        "{\"status\":\"ok\",\"enabled\":%s,\"size\":%d,\"max\":%d,\"breakpoints\":[",
        MONITOR_breakpoints_enabled ? "true" : "false", MONITOR_breakpoint_table_size, MONITOR_BREAKPOINT_TABLE_MAX);
    for (i = 0; i < MONITOR_breakpoint_table_size; i++) {
        MONITOR_breakpoint_cond *bp = &MONITOR_breakpoint_table[i];
        ai_append(ai_response, sizeof(ai_response), &pos,
            "%s{\"id\":%d,\"slot\":%d,\"enabled\":%s,\"condition\":\"%s\",\"raw_condition\":%u,\"value\":%u,\"m_addr\":%u}",
            i ? "," : "", i, i, bp->enabled ? "true" : "false",
            ai_breakpoint_condition_name(bp->condition), bp->condition, bp->value, bp->m_addr);
    }
    ai_append(ai_response, sizeof(ai_response), &pos, "]}");
    AI_SendResponse(ai_response);
}
#endif

static int ai_ensure_dir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return TRUE;
    return FALSE;
}

static int ai_refresh_artifact_dir(void)
{
    char resolved[PATH_MAX];

    if (!ai_ensure_dir(AI_artifact_dir)) {
        Log_print("AI: failed to create artifact dir %s: %s", AI_artifact_dir, strerror(errno));
        return FALSE;
    }
    if (realpath(AI_artifact_dir, resolved) == NULL) {
        Log_print("AI: failed to resolve artifact dir %s: %s", AI_artifact_dir, strerror(errno));
        return FALSE;
    }
    strncpy(AI_artifact_dir_resolved, resolved, sizeof(AI_artifact_dir_resolved) - 1);
    AI_artifact_dir_resolved[sizeof(AI_artifact_dir_resolved) - 1] = '\0';
    return TRUE;
}

static int ai_path_has_parent_ref(const char *path)
{
    return AI_PathHasParentRef(path) ? TRUE : FALSE;
}

static int ai_validate_output_path(const char *path, const char *field)
{
    char parent[PATH_MAX];
    char *slash;
    char resolved_parent[PATH_MAX];
    size_t root_len;

    if (path == NULL || path[0] == '\0') {
        ai_send_error("MISSING_FIELD", "output path is required", field);
        return FALSE;
    }
    if (AI_unsafe_paths)
        return TRUE;
    if (path[0] != '/') {
        ai_send_error("PATH_DENIED", "output path must be absolute", field);
        return FALSE;
    }
    if (ai_path_has_parent_ref(path)) {
        ai_send_error("PATH_DENIED", "output path may not contain .. components", field);
        return FALSE;
    }
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent) {
        strcpy(parent, "/");
    }
    else {
        *slash = '\0';
    }
    if (realpath(parent, resolved_parent) == NULL) {
        ai_send_error("PATH_DENIED", "output path parent directory is not available", field);
        return FALSE;
    }
    root_len = strlen(AI_artifact_dir_resolved);
    if (strncmp(resolved_parent, AI_artifact_dir_resolved, root_len) != 0 ||
        (resolved_parent[root_len] != '\0' && resolved_parent[root_len] != '/')) {
        ai_send_error("PATH_DENIED", "output path is outside the AI artifact directory", field);
        return FALSE;
    }
    return TRUE;
}

static void ai_default_artifact_path(char *path, size_t path_size, const char *prefix, const char *ext)
{
    char suffix[64];

    if (path_size == 0)
        return;
    snprintf(suffix, sizeof(suffix), "/%s_%ld.%s", prefix, (long)time(NULL), ext);
    path[0] = '\0';
    strncat(path, AI_artifact_dir_resolved, path_size - 1);
    strncat(path, suffix, path_size - strlen(path) - 1);
}

static void ai_send_hello(void)
{
    size_t pos = 0;

    ai_response[0] = '\0';
    ai_append(ai_response, sizeof(ai_response), &pos,
        "{\"status\":\"ok\",\"protocol_version\":%d,\"emulator\":\"atari800\","
        "\"ai_interface\":true,\"build\":{\"ai_interface\":true,\"netsio\":",
        AI_PROTOCOL_VERSION);
#ifdef NETSIO
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"monitor_break\":");
#ifdef MONITOR_BREAK
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"monitor_breakpoints\":");
#ifdef MONITOR_BREAKPOINTS
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"monitor_hints\":");
#ifdef MONITOR_HINTS
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"monitor_trace\":");
#ifdef MONITOR_TRACE
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"monitor_profile\":");
#ifdef MONITOR_PROFILE
    ai_append(ai_response, sizeof(ai_response), &pos, "true");
#else
    ai_append(ai_response, sizeof(ai_response), &pos, "false");
#endif
    ai_append(ai_response, sizeof(ai_response), &pos,
        "},\"limits\":{\"max_command_bytes\":%d,\"max_response_bytes\":%d,\"peek_max_len\":%d},"
        "\"paths\":{\"artifact_dir\":",
        AI_BUFFER_SIZE - 1, AI_MAX_RESPONSE, AI_PEEK_MAX_LEN);
    AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, AI_artifact_dir_resolved);
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"unsafe_paths\":%s},\"sockets\":{\"command\":", AI_unsafe_paths ? "true" : "false");
    AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, AI_socket_path);
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"video_push\":");
    AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, AI_video_push_socket_path);
    ai_append(ai_response, sizeof(ai_response), &pos, ",\"video_pull\":");
    AI_JSON_EscapeAppend(ai_response, sizeof(ai_response), &pos, AI_video_pull_socket_path);
    ai_append(ai_response, sizeof(ai_response), &pos,
        "},\"commands\":[\"ping\",\"hello\",\"capabilities\",\"load\",\"run\",\"frame_step\",\"step\","
        "\"pause\",\"reset\",\"key\",\"key_release\",\"joystick\",\"paddle\",\"consol\","
        "\"screenshot\",\"screen_ascii\",\"screen_raw\",\"video.enable_push\",\"video.disable_push\","
        "\"video.enable_pull\",\"video.disable_pull\",\"video.push.set_fps_cap\","
        "\"video.push.set_frameskip\",\"video.push.enable_change_triggered\",\"video.status\",\"peek\",\"poke\","
        "\"dump\",\"cpu\",\"cpu_set\",\"antic\",\"gtia\",\"pokey\",\"pia\","
        "\"debug_enable\",\"debug_read\",\"debugger.status\",\"debugger.show_state\",\"debugger.step_instruction\","
        "\"debugger.history\",\"debugger.jumps\",\"debugger.stack\",\"debugger.disassemble\",\"debugger.disassemble_loop\","
        "\"debugger.dlist\",\"debugger.search_memory\",\"debugger.search_string\",\"debugger.search_screencode_string\",\"debugger.labels\","
        "\"debugger.continue\",\"breakpoint.pc\",\"breakpoint.brk\",\"breakpoint.status\",\"breakpoint.clear\","
        "\"breakpoint.list\",\"breakpoint.add\",\"breakpoint.delete\",\"breakpoint.enable\",\"breakpoint.disable\","
        "\"save_state\",\"load_state\"],"
        "\"command_classes\":{\"read_only\":[\"ping\",\"hello\",\"capabilities\",\"screen_ascii\",\"screen_raw\","
        "\"video.status\",\"peek\",\"cpu\",\"antic\",\"gtia\",\"pokey\",\"pia\",\"debug_read\","
        "\"debugger.status\",\"debugger.show_state\",\"debugger.history\",\"debugger.jumps\",\"debugger.stack\","
        "\"debugger.disassemble\",\"debugger.disassemble_loop\",\"debugger.dlist\",\"debugger.search_memory\","
        "\"debugger.search_string\",\"debugger.search_screencode_string\",\"debugger.labels\",\"breakpoint.status\",\"breakpoint.list\"],"
        "\"mutating\":[\"load\",\"run\",\"frame_step\",\"step\",\"pause\",\"reset\",\"key\",\"key_release\","
        "\"joystick\",\"paddle\",\"consol\",\"screenshot\",\"video.enable_push\",\"video.disable_push\","
        "\"video.enable_pull\",\"video.disable_pull\",\"video.push.set_fps_cap\",\"video.push.set_frameskip\","
        "\"video.push.enable_change_triggered\",\"dump\",\"debug_enable\",\"debugger.step_instruction\","
        "\"debugger.continue\",\"breakpoint.pc\",\"breakpoint.brk\",\"breakpoint.clear\",\"breakpoint.add\","
        "\"breakpoint.delete\",\"breakpoint.enable\",\"breakpoint.disable\",\"save_state\",\"load_state\"],"
        "\"unsafe\":[\"poke\",\"cpu_set\"]}}");
    AI_SendResponse(ai_response);
}

/* Process a command */
static void process_command(const char *cmd) {
    char cmd_type[64] = "";
    char path[PATH_MAX] = "";
    int rc;

    if (!AI_JSON_IsValidObject(cmd)) {
        ai_send_error("BAD_JSON", "request must be a valid JSON object", NULL);
        return;
    }

    rc = AI_JSON_GetString(cmd, "cmd", cmd_type, sizeof(cmd_type), TRUE);
    if (rc != AI_JSON_OK) {
        ai_send_validation_error(rc, "cmd", TRUE);
        return;
    }

    /* === CONTROL === */
    if (strcmp(cmd_type, "ping") == 0) {
        AI_SendResponse("{\"status\":\"ok\",\"msg\":\"pong\"}");
    }
    else if (strcmp(cmd_type, "hello") == 0 || strcmp(cmd_type, "capabilities") == 0) {
        ai_send_hello();
    }
    else if (strcmp(cmd_type, "load") == 0) {
        rc = AI_JSON_GetString(cmd, "path", path, sizeof(path), TRUE);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "path", TRUE);
            return;
        }
        if (BINLOAD_Loader(path)) {
            ai_send_ok();
        } else {
            ai_send_error("IO_ERROR", "Failed to load path", "path");
        }
    }
    else if (strcmp(cmd_type, "run") == 0) {
        rc = AI_JSON_GetInt(cmd, "frames", &ai_frames_to_run, 1, FALSE, 1, 1000000);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "frames", FALSE);
            return;
        }
        ai_frames_requested = ai_frames_to_run;
        ai_async_response_pending = 1;
        ai_command_deadline_us = AI_command_timeout_ms > 0 ? monotonic_us() + (unsigned long long)AI_command_timeout_ms * 1000ULL : 0;
        ai_paused = 0;
        /* Response sent after frames complete */
    }
    else if (strcmp(cmd_type, "frame_step") == 0 || strcmp(cmd_type, "step") == 0) {
        int deprecated_step = strcmp(cmd_type, "step") == 0;
        const char *count_field = deprecated_step ? "instructions" : "frames";
        rc = AI_JSON_GetInt(cmd, count_field, &ai_steps_to_run, 1, FALSE, 1, 1000000);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, count_field, FALSE);
            return;
        }
        ai_steps_requested = ai_steps_to_run;
        ai_async_response_pending = 1;
        ai_command_deadline_us = AI_command_timeout_ms > 0 ? monotonic_us() + (unsigned long long)AI_command_timeout_ms * 1000ULL : 0;
        ai_paused = 0;
        /* Response sent after steps complete */
    }
    else if (strcmp(cmd_type, "pause") == 0) {
        ai_paused = 1;
        ai_async_response_pending = 0;
        ai_set_stopped_reason("manual_pause");
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "reset") == 0) {
        Atari800_Coldstart();
        ai_send_ok();
    }

    /* === INPUT === */
    else if (strcmp(cmd_type, "key") == 0) {
        int shift;
        rc = AI_JSON_GetInt(cmd, "code", &INPUT_key_code, AKEY_NONE, TRUE, 0, 65535);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "code", TRUE);
            return;
        }
        rc = AI_JSON_GetBool(cmd, "shift", &shift, 0, FALSE);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "shift", FALSE);
            return;
        }
        INPUT_key_shift = shift;
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "key_release") == 0) {
        INPUT_key_code = AKEY_NONE;
        INPUT_key_shift = 0;
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "joystick") == 0) {
        int port;
        char dir[16] = "";
        int fire;
        int stick = INPUT_STICK_CENTRE;

        rc = AI_JSON_GetInt(cmd, "port", &port, 0, FALSE, 0, 3);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "port", FALSE);
            return;
        }
        rc = AI_JSON_GetString(cmd, "direction", dir, sizeof(dir), FALSE);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "direction", FALSE);
            return;
        }
        rc = AI_JSON_GetBool(cmd, "fire", &fire, 0, FALSE);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "fire", FALSE);
            return;
        }

        if (dir[0] == '\0' || strcmp(dir, "center") == 0) stick = INPUT_STICK_CENTRE;
        else if (strcmp(dir, "up") == 0) stick = INPUT_STICK_FORWARD;
        else if (strcmp(dir, "down") == 0) stick = INPUT_STICK_BACK;
        else if (strcmp(dir, "left") == 0) stick = INPUT_STICK_LEFT;
        else if (strcmp(dir, "right") == 0) stick = INPUT_STICK_RIGHT;
        else if (strcmp(dir, "ul") == 0) stick = INPUT_STICK_UL;
        else if (strcmp(dir, "ur") == 0) stick = INPUT_STICK_UR;
        else if (strcmp(dir, "ll") == 0) stick = INPUT_STICK_LL;
        else if (strcmp(dir, "lr") == 0) stick = INPUT_STICK_LR;
        else {
            ai_send_error("BAD_ARGUMENT", "direction is invalid", "direction");
            return;
        }

        AI_joy_override[port] = (stick == INPUT_STICK_CENTRE) ? -1 : stick;
        AI_trig_override[port] = fire ? 0 : -1;
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "paddle") == 0) {
        int port;
        int value;
        rc = AI_JSON_GetInt(cmd, "port", &port, 0, FALSE, 0, 7);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "port", FALSE);
            return;
        }
        rc = AI_JSON_GetInt(cmd, "value", &value, 128, FALSE, 0, 255);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "value", FALSE);
            return;
        }
        POKEY_POT_input[port] = value;
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "consol") == 0) {
        int start, select, option;
        rc = AI_JSON_GetBool(cmd, "start", &start, 1, FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "start", FALSE); return; }
        rc = AI_JSON_GetBool(cmd, "select", &select, 1, FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "select", FALSE); return; }
        rc = AI_JSON_GetBool(cmd, "option", &option, 1, FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "option", FALSE); return; }
        INPUT_key_consol = INPUT_CONSOL_NONE;
        if (!start) INPUT_key_consol &= ~INPUT_CONSOL_START;
        if (!select) INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
        if (!option) INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
        ai_send_ok();
    }

    /* === SCREEN === */
    else if (strcmp(cmd_type, "screenshot") == 0) {
        rc = AI_JSON_GetString(cmd, "path", path, sizeof(path), FALSE);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "path", FALSE);
            return;
        }
        if (!path[0]) {
            ai_default_artifact_path(path, sizeof(path), "screenshot", "png");
        }
        if (!ai_validate_output_path(path, "path"))
            return;
        Log_print("AI screenshot: path=%s vis=%d,%d-%d,%d", path,
            Screen_visible_x1, Screen_visible_y1, Screen_visible_x2, Screen_visible_y2);
        if (Screen_SaveScreenshot(path, 0)) {
            ai_send_ok_path(path);
        } else {
            ai_send_error("IO_ERROR", "Failed to save screenshot", "path");
        }
    }
    else if (strcmp(cmd_type, "screen_ascii") == 0) {
        char ascii_data[2048];
        screen_to_ascii(ascii_data, sizeof(ascii_data));
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"width\":40,\"height\":24,\"data\":%s}", ascii_data);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "screen_raw") == 0) {
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
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "video.disable_push") == 0) {
        ai_fb_push_enabled = FALSE;
        ai_fb_close_push_client();
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "video.enable_pull") == 0) {
        ai_fb_pull_enabled = TRUE;
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "video.disable_pull") == 0) {
        ai_fb_pull_enabled = FALSE;
        ai_fb_close_pull_client();
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "video.push.set_fps_cap") == 0) {
        rc = AI_JSON_GetInt(cmd, "value", &ai_fb_fps_cap, 0, TRUE, 0, 1000);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "value", TRUE);
            return;
        }
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"fps_cap\":%d}", ai_fb_fps_cap);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "video.push.set_frameskip") == 0) {
        rc = AI_JSON_GetInt(cmd, "n", &ai_fb_send_every_n_frames, 1, TRUE, 1, 1000000);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "n", TRUE);
            return;
        }
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"send_every_n_frames\":%d}", ai_fb_send_every_n_frames);
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "video.push.enable_change_triggered") == 0) {
        rc = AI_JSON_GetBool(cmd, "enabled", &ai_fb_change_triggered, TRUE, TRUE);
        if (rc != AI_JSON_OK) {
            ai_send_validation_error(rc, "enabled", TRUE);
            return;
        }
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "video.status") == 0) {
        snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\","
            "\"push_socket\":\"%s\",\"pull_socket\":\"%s\","
            "\"push_enabled\":%s,\"pull_enabled\":%s,"
            "\"fps_cap\":%d,\"send_every_n_frames\":%d,"
            "\"change_triggered\":%s,"
            "\"width\":%d,\"height\":%d,\"stride\":%u,\"frame_no\":%u}",
            AI_video_push_socket_path, AI_video_pull_socket_path,
            ai_fb_push_enabled ? "true" : "false",
            ai_fb_pull_enabled ? "true" : "false",
            ai_fb_fps_cap, ai_fb_send_every_n_frames,
            ai_fb_change_triggered ? "true" : "false",
            ai_fb_width, ai_fb_height, ai_fb_stride, ai_fb_frame_no);
        AI_SendResponse(ai_response);
    }

    /* === MEMORY === */
    else if (strcmp(cmd_type, "peek") == 0) {
        int addr;
        int len;
        int pos;
        int i;
        rc = AI_JSON_GetInt(cmd, "addr", &addr, 0, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", TRUE); return; }
        rc = AI_JSON_GetInt(cmd, "len", &len, 1, FALSE, 1, AI_PEEK_MAX_LEN);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "len", FALSE); return; }

        pos = snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"addr\":%d,\"data\":[", addr);
        for (i = 0; i < len && pos < (int)sizeof(ai_response) - 20; i++) {
            pos += snprintf(ai_response + pos, sizeof(ai_response) - pos,
                "%s%d", i ? "," : "", MEMORY_SafeGetByte((UWORD)(addr + i)));
        }
        snprintf(ai_response + pos, sizeof(ai_response) - pos, "]}");
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "poke") == 0) {
        int addr;
        int count;
        int i;
        UBYTE data_buf[AI_BUFFER_SIZE];
        rc = AI_JSON_GetInt(cmd, "addr", &addr, 0, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", TRUE); return; }
        rc = AI_JSON_GetByteArray(cmd, "data", data_buf, sizeof(data_buf), &count, TRUE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "data", TRUE); return; }
        for (i = 0; i < count; i++) {
            MEMORY_mem[(UWORD)(addr + i)] = data_buf[i];
        }
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "dump") == 0) {
        int start_addr;
        int end_addr;
        FILE *f;
        int i;
        rc = AI_JSON_GetInt(cmd, "start", &start_addr, 0, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "start", TRUE); return; }
        rc = AI_JSON_GetInt(cmd, "end", &end_addr, 0xFFFF, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "end", TRUE); return; }
        if (end_addr < start_addr) {
            ai_send_error("BAD_ARGUMENT", "end must be greater than or equal to start", "end");
            return;
        }
        rc = AI_JSON_GetString(cmd, "path", path, sizeof(path), TRUE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "path", TRUE); return; }
        if (!ai_validate_output_path(path, "path"))
            return;
        f = fopen(path, "wb");
        if (f) {
            for (i = start_addr; i <= end_addr; i++) {
                UBYTE b = MEMORY_SafeGetByte((UWORD)i);
                fwrite(&b, 1, 1, f);
            }
            fclose(f);
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"ok\",\"bytes\":%d}", end_addr - start_addr + 1);
            AI_SendResponse(ai_response);
        } else {
            ai_send_error("IO_ERROR", "Failed to open output file", "path");
        }
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
        int value;
        if ((rc = AI_JSON_GetInt(cmd, "pc", &value, CPU_regPC, FALSE, 0, 0xFFFF)) != AI_JSON_OK) { ai_send_validation_error(rc, "pc", FALSE); return; }
        CPU_regPC = value;
        if ((rc = AI_JSON_GetInt(cmd, "a", &value, CPU_regA, FALSE, 0, 0xFF)) != AI_JSON_OK) { ai_send_validation_error(rc, "a", FALSE); return; }
        CPU_regA = value;
        if ((rc = AI_JSON_GetInt(cmd, "x", &value, CPU_regX, FALSE, 0, 0xFF)) != AI_JSON_OK) { ai_send_validation_error(rc, "x", FALSE); return; }
        CPU_regX = value;
        if ((rc = AI_JSON_GetInt(cmd, "y", &value, CPU_regY, FALSE, 0, 0xFF)) != AI_JSON_OK) { ai_send_validation_error(rc, "y", FALSE); return; }
        CPU_regY = value;
        if ((rc = AI_JSON_GetInt(cmd, "sp", &value, CPU_regS, FALSE, 0, 0xFF)) != AI_JSON_OK) { ai_send_validation_error(rc, "sp", FALSE); return; }
        CPU_regS = value;
        CPU_PutStatus();
        ai_send_ok();
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
        rc = AI_JSON_GetInt(cmd, "addr", &AI_debug_port, 0xD7FF, FALSE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", FALSE); return; }
        ai_send_ok();
    }
    else if (strcmp(cmd_type, "debug_read") == 0) {
        int pos = snprintf(ai_response, sizeof(ai_response),
            "{\"status\":\"ok\",\"data\":[");
        int i;
        for (i = 0; i < ai_debug_buffer_pos && pos < (int)sizeof(ai_response) - 100; i++) {
            pos += snprintf(ai_response + pos, sizeof(ai_response) - pos,
                "%s%d", i ? "," : "", ai_debug_buffer[i]);
        }
        pos += snprintf(ai_response + pos, sizeof(ai_response) - pos, "],\"ascii\":\"");
        for (i = 0; i < ai_debug_buffer_pos && pos < (int)sizeof(ai_response) - 10; i++) {
            UBYTE c = ai_debug_buffer[i];
            if (c >= 32 && c < 127 && c != '"' && c != '\\') {
                ai_response[pos++] = c;
            } else {
                ai_response[pos++] = '.';
            }
        }
        snprintf(ai_response + pos, sizeof(ai_response) - pos, "\"}");
        ai_debug_buffer_pos = 0;
        AI_SendResponse(ai_response);
    }
    else if (strcmp(cmd_type, "debugger.status") == 0 || strcmp(cmd_type, "debugger.show_state") == 0) {
        ai_send_debugger_state("ok");
    }
    else if (strcmp(cmd_type, "debugger.history") == 0) {
        ai_send_monitor_capture(ai_write_history, NULL);
    }
    else if (strcmp(cmd_type, "debugger.jumps") == 0) {
        ai_send_monitor_capture(ai_write_jumps, NULL);
    }
    else if (strcmp(cmd_type, "debugger.stack") == 0) {
        ai_monitor_range_ctx range;
        rc = AI_JSON_GetInt(cmd, "count", &range.count, 16, FALSE, 1, 256);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "count", FALSE); return; }
        range.addr = 0;
        ai_send_monitor_capture(ai_write_stack, &range);
    }
    else if (strcmp(cmd_type, "debugger.disassemble") == 0) {
        ai_monitor_range_ctx range;
        int addr_default;
        CPU_GetStatus();
        addr_default = CPU_regPC;
        rc = AI_JSON_GetInt(cmd, "addr", &addr_default, addr_default, FALSE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", FALSE); return; }
        rc = AI_JSON_GetInt(cmd, "count", &range.count, 24, FALSE, 1, 256);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "count", FALSE); return; }
        range.addr = (UWORD)addr_default;
        ai_send_monitor_capture(ai_write_disassemble, &range);
    }
    else if (strcmp(cmd_type, "debugger.disassemble_loop") == 0) {
        ai_monitor_range_ctx range;
        int addr_default;
        CPU_GetStatus();
        addr_default = CPU_regPC;
        rc = AI_JSON_GetInt(cmd, "addr", &addr_default, addr_default, FALSE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", FALSE); return; }
        range.addr = (UWORD)addr_default;
        range.count = 24;
        ai_send_monitor_capture(ai_write_disassemble_loop, &range);
    }
    else if (strcmp(cmd_type, "debugger.dlist") == 0) {
        ai_monitor_range_ctx range;
        int addr = ANTIC_dlist;
        rc = AI_JSON_GetInt(cmd, "addr", &addr, addr, FALSE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", FALSE); return; }
        rc = AI_JSON_GetInt(cmd, "count", &range.count, 64, FALSE, 1, 512);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "count", FALSE); return; }
        range.addr = (UWORD)addr;
        ai_send_monitor_capture(ai_write_dlist, &range);
    }
    else if (strcmp(cmd_type, "debugger.search_memory") == 0) {
        int start_addr;
        int end_addr;
        int count;
        UBYTE pattern[64];
        rc = AI_JSON_GetInt(cmd, "start", &start_addr, 0, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "start", TRUE); return; }
        rc = AI_JSON_GetInt(cmd, "end", &end_addr, 0xFFFF, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "end", TRUE); return; }
        if (end_addr < start_addr) { ai_send_error("BAD_ARGUMENT", "end must be greater than or equal to start", "end"); return; }
        rc = AI_JSON_GetByteArray(cmd, "pattern", pattern, sizeof(pattern), &count, TRUE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "pattern", TRUE); return; }
        ai_send_memory_search(start_addr, end_addr, pattern, count);
    }
    else if (strcmp(cmd_type, "debugger.search_string") == 0 || strcmp(cmd_type, "debugger.search_screencode_string") == 0) {
        int start_addr;
        int end_addr;
        char needle[256];
        UBYTE pattern[256];
        int i;
        int screencode = strcmp(cmd_type, "debugger.search_screencode_string") == 0;
        rc = AI_JSON_GetInt(cmd, "start", &start_addr, 0, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "start", TRUE); return; }
        rc = AI_JSON_GetInt(cmd, "end", &end_addr, 0xFFFF, TRUE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "end", TRUE); return; }
        if (end_addr < start_addr) { ai_send_error("BAD_ARGUMENT", "end must be greater than or equal to start", "end"); return; }
        rc = AI_JSON_GetString(cmd, "text", needle, sizeof(needle), TRUE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "text", TRUE); return; }
        if (needle[0] == '\0') { ai_send_error("BAD_ARGUMENT", "text must not be empty", "text"); return; }
        for (i = 0; needle[i] != '\0'; i++)
            pattern[i] = screencode ? (UBYTE)ai_screen_to_asc(needle[i]) : (UBYTE)needle[i];
        ai_send_memory_search(start_addr, end_addr, pattern, i);
    }
    else if (strcmp(cmd_type, "debugger.labels") == 0) {
        ai_monitor_range_ctx range;
        rc = AI_JSON_GetInt(cmd, "limit", &range.count, 256, FALSE, 1, 4096);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "limit", FALSE); return; }
        range.addr = 0;
#ifdef MONITOR_HINTS
        ai_send_monitor_capture(ai_write_labels, &range);
#else
        ai_send_error("CAPABILITY_UNAVAILABLE", "labels require MONITOR_HINTS", NULL);
#endif
    }
    else if (strcmp(cmd_type, "debugger.step_instruction") == 0) {
        int instructions;
        rc = AI_JSON_GetInt(cmd, "instructions", &instructions, 1, FALSE, 1, 1000000);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "instructions", FALSE); return; }
#ifdef MONITOR_BREAK
        ai_instruction_steps_to_run = instructions;
        ai_instruction_steps_requested = instructions;
        ai_async_response_pending = 1;
        ai_command_deadline_us = AI_command_timeout_ms > 0 ? monotonic_us() + (unsigned long long)AI_command_timeout_ms * 1000ULL : 0;
        MONITOR_break_step = TRUE;
        ai_paused = 0;
        /* Response sent after the requested instruction count completes. */
#else
        ai_send_error("CAPABILITY_UNAVAILABLE", "instruction stepping requires MONITOR_BREAK", NULL);
#endif
    }
    else if (strcmp(cmd_type, "debugger.continue") == 0) {
        ai_async_response_pending = 0;
        ai_set_stopped_reason("running");
        ai_paused = 0;
        ai_send_debugger_state("ok");
    }
    else if (strcmp(cmd_type, "breakpoint.pc") == 0) {
        int addr;
        int enabled;
        rc = AI_JSON_GetBool(cmd, "enabled", &enabled, 1, FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "enabled", FALSE); return; }
        rc = AI_JSON_GetInt(cmd, "addr", &addr, ai_break_pc_addr, enabled ? TRUE : FALSE, 0, 0xFFFF);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "addr", enabled ? TRUE : FALSE); return; }
        ai_break_pc_enabled = enabled;
        if (enabled)
            ai_break_pc_addr = (UWORD)addr;
        ai_send_debugger_state("ok");
    }
    else if (strcmp(cmd_type, "breakpoint.brk") == 0) {
        int enabled;
        rc = AI_JSON_GetBool(cmd, "enabled", &enabled, 1, FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "enabled", FALSE); return; }
        ai_break_brk_enabled = enabled;
        ai_send_debugger_state("ok");
    }
    else if (strcmp(cmd_type, "breakpoint.status") == 0) {
        ai_send_debugger_state("ok");
    }
    else if (strcmp(cmd_type, "breakpoint.list") == 0) {
#ifdef MONITOR_BREAKPOINTS
        ai_send_breakpoint_table();
#else
        ai_send_error("CAPABILITY_UNAVAILABLE", "rich breakpoint table requires MONITOR_BREAKPOINTS", NULL);
#endif
    }
    else if (strcmp(cmd_type, "breakpoint.add") == 0) {
#ifdef MONITOR_BREAKPOINTS
        int slot;
        int value = 0;
        int m_addr = 0;
        char condition_text[96] = "";
        char condition_type[16] = "";
        char op[3] = "=";
        UWORD condition;
        UWORD bp_value;
        UWORD bp_m_addr;
        if (MONITOR_breakpoint_table_size >= MONITOR_BREAKPOINT_TABLE_MAX) {
            ai_send_error("LIMIT_EXCEEDED", "breakpoint table is full", NULL);
            return;
        }
        rc = AI_JSON_GetString(cmd, "condition", condition_text, sizeof(condition_text), FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "condition", FALSE); return; }
        if (condition_text[0] != '\0') {
            if (!ai_parse_breakpoint_condition_string(condition_text, &condition, &bp_value, &bp_m_addr)) {
                ai_send_error("BAD_ARGUMENT", "condition string is invalid", "condition");
                return;
            }
        }
        else {
            int type_bits;
            int op_bits;
            rc = AI_JSON_GetString(cmd, "condition_type", condition_type, sizeof(condition_type), TRUE);
            if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "condition_type", TRUE); return; }
            rc = AI_JSON_GetString(cmd, "operator", op, sizeof(op), FALSE);
            if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "operator", FALSE); return; }
            rc = AI_JSON_GetInt(cmd, "value", &value, 0, TRUE, 0, 0xFFFF);
            if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "value", TRUE); return; }
            rc = AI_JSON_GetInt(cmd, "m_addr", &m_addr, 0, FALSE, 0, 0xFFFF);
            if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "m_addr", FALSE); return; }
            type_bits = ai_breakpoint_type_bits(condition_type);
            op_bits = ai_breakpoint_operator_bits(op);
            if (type_bits == 0) { ai_send_error("BAD_ARGUMENT", "condition_type is invalid", "condition_type"); return; }
            if (op_bits == 0) { ai_send_error("BAD_ARGUMENT", "operator is invalid", "operator"); return; }
            condition = (UWORD)(type_bits | op_bits);
            bp_value = (UWORD)value;
            bp_m_addr = (UWORD)m_addr;
        }
        slot = MONITOR_breakpoint_table_size++;
        MONITOR_breakpoint_table[slot].enabled = TRUE;
        MONITOR_breakpoint_table[slot].condition = condition;
        MONITOR_breakpoint_table[slot].value = bp_value;
        MONITOR_breakpoint_table[slot].m_addr = bp_m_addr;
        ai_send_breakpoint_table();
#else
        ai_send_error("CAPABILITY_UNAVAILABLE", "rich breakpoint table requires MONITOR_BREAKPOINTS", NULL);
#endif
    }
    else if (strcmp(cmd_type, "breakpoint.delete") == 0) {
#ifdef MONITOR_BREAKPOINTS
        int slot;
        int i;
        rc = AI_JSON_GetInt(cmd, "slot", &slot, -1, TRUE, 0, MONITOR_BREAKPOINT_TABLE_MAX - 1);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "slot", TRUE); return; }
        if (slot >= MONITOR_breakpoint_table_size) { ai_send_error("BAD_ARGUMENT", "slot does not exist", "slot"); return; }
        for (i = slot; i < MONITOR_breakpoint_table_size - 1; i++)
            MONITOR_breakpoint_table[i] = MONITOR_breakpoint_table[i + 1];
        MONITOR_breakpoint_table_size--;
        ai_send_breakpoint_table();
#else
        ai_send_error("CAPABILITY_UNAVAILABLE", "rich breakpoint table requires MONITOR_BREAKPOINTS", NULL);
#endif
    }
    else if (strcmp(cmd_type, "breakpoint.enable") == 0 || strcmp(cmd_type, "breakpoint.disable") == 0) {
#ifdef MONITOR_BREAKPOINTS
        int slot;
        int enabled = strcmp(cmd_type, "breakpoint.enable") == 0;
        rc = AI_JSON_GetInt(cmd, "slot", &slot, -1, TRUE, 0, MONITOR_BREAKPOINT_TABLE_MAX - 1);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "slot", TRUE); return; }
        if (slot >= MONITOR_breakpoint_table_size) { ai_send_error("BAD_ARGUMENT", "slot does not exist", "slot"); return; }
        MONITOR_breakpoint_table[slot].enabled = (UBYTE)enabled;
        ai_send_breakpoint_table();
#else
        ai_send_error("CAPABILITY_UNAVAILABLE", "rich breakpoint table requires MONITOR_BREAKPOINTS", NULL);
#endif
    }
    else if (strcmp(cmd_type, "breakpoint.clear") == 0) {
        char type[16] = "";
        rc = AI_JSON_GetString(cmd, "type", type, sizeof(type), FALSE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "type", FALSE); return; }
        if (type[0] == '\0' || strcmp(type, "all") == 0 || strcmp(type, "pc") == 0)
            ai_break_pc_enabled = 0;
        if (type[0] == '\0' || strcmp(type, "all") == 0 || strcmp(type, "brk") == 0)
            ai_break_brk_enabled = 0;
        if (type[0] == '\0' || strcmp(type, "all") == 0 || strcmp(type, "table") == 0 || strcmp(type, "rich") == 0) {
#ifdef MONITOR_BREAKPOINTS
            MONITOR_breakpoint_table_size = 0;
#elif 0
            ;
#endif
        }
        if (type[0] != '\0' && strcmp(type, "all") != 0 && strcmp(type, "pc") != 0 && strcmp(type, "brk") != 0
            && strcmp(type, "table") != 0 && strcmp(type, "rich") != 0) {
            ai_send_error("BAD_ARGUMENT", "type must be all, pc, brk, table, or rich", "type");
            return;
        }
        ai_send_debugger_state("ok");
    }

    /* === STATE === */
    else if (strcmp(cmd_type, "save_state") == 0) {
        rc = AI_JSON_GetString(cmd, "path", path, sizeof(path), TRUE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "path", TRUE); return; }
        if (!ai_validate_output_path(path, "path"))
            return;
        if (StateSav_SaveAtariState(path, "wb", TRUE)) {
            ai_send_ok();
        } else {
            ai_send_error("IO_ERROR", "Failed to save state", "path");
        }
    }
    else if (strcmp(cmd_type, "load_state") == 0) {
        rc = AI_JSON_GetString(cmd, "path", path, sizeof(path), TRUE);
        if (rc != AI_JSON_OK) { ai_send_validation_error(rc, "path", TRUE); return; }
        if (StateSav_ReadAtariState(path, "rb")) {
            ai_send_ok();
        } else {
            ai_send_error("IO_ERROR", "Failed to load state", "path");
        }
    }

    /* Unknown command */
    else {
        ai_send_error("UNKNOWN_COMMAND", "Unknown command", "cmd");
    }
}

/* Read command from client */
static int read_command(char *buf, int bufsize) {
    char header[32];
    int hpos = 0;
    char *endptr;
    long len;
    int total;

    if (ai_client_fd < 0) return 0;

    while (hpos < (int)sizeof(header) - 1) {
        int r = read(ai_client_fd, header + hpos, 1);
        if (r <= 0) {
            if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
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

    if (hpos >= (int)sizeof(header) - 1) {
        ai_send_error("BAD_ARGUMENT", "length prefix is too long", NULL);
        close(ai_client_fd);
        ai_client_fd = -1;
        return -1;
    }

    errno = 0;
    len = strtol(header, &endptr, 10);
    if (header == endptr || errno != 0 || *endptr != '\0' || len <= 0) {
        ai_send_error("BAD_ARGUMENT", "length prefix must be a positive integer", NULL);
        return -1;
    }
    if (!AI_CommandLengthIsAllowed(len, bufsize)) {
        ai_send_error("BAD_ARGUMENT", "command exceeds maximum size", NULL);
        close(ai_client_fd);
        ai_client_fd = -1;
        return -1;
    }

    total = 0;
    while (total < len) {
        int r = read(ai_client_fd, buf + total, len - total);
        if (r <= 0) return 0;
        total += r;
    }
    buf[len] = '\0';
    return (int)len;
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
            AI_socket_path[sizeof(AI_socket_path) - 1] = '\0';
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-video-push-socket") == 0 && i + 1 < *argc) {
            strncpy(AI_video_push_socket_path, argv[++i], sizeof(AI_video_push_socket_path) - 1);
            AI_video_push_socket_path[sizeof(AI_video_push_socket_path) - 1] = '\0';
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-video-pull-socket") == 0 && i + 1 < *argc) {
            strncpy(AI_video_pull_socket_path, argv[++i], sizeof(AI_video_pull_socket_path) - 1);
            AI_video_pull_socket_path[sizeof(AI_video_pull_socket_path) - 1] = '\0';
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-artifact-dir") == 0 && i + 1 < *argc) {
            strncpy(AI_artifact_dir, argv[++i], sizeof(AI_artifact_dir) - 1);
            AI_artifact_dir[sizeof(AI_artifact_dir) - 1] = '\0';
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-unsafe-paths") == 0) {
            AI_unsafe_paths = TRUE;
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-debug-port") == 0 && i + 1 < *argc) {
            AI_debug_port = strtol(argv[++i], NULL, 0);
            match = TRUE;
        }
        else if (strcmp(argv[i], "-ai-command-timeout-ms") == 0 && i + 1 < *argc) {
            AI_command_timeout_ms = strtol(argv[++i], NULL, 0);
            if (AI_command_timeout_ms < 0)
                AI_command_timeout_ms = 0;
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
        if (!ai_refresh_artifact_dir()) {
            AI_enabled = FALSE;
            return FALSE;
        }
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

    if ((ai_frames_to_run > 0 || ai_steps_to_run > 0) && ai_command_deadline_us > 0 && monotonic_us() > ai_command_deadline_us) {
        ai_frames_to_run = 0;
        ai_steps_to_run = 0;
        ai_frames_requested = 0;
        ai_steps_requested = 0;
        ai_instruction_steps_to_run = 0;
        ai_instruction_steps_requested = 0;
        ai_async_response_pending = 0;
        ai_command_deadline_us = 0;
        ai_paused = 1;
        ai_set_stopped_reason("timeout");
#ifdef MONITOR_BREAK
        MONITOR_break_step = FALSE;
#endif
        ai_send_error("TIMEOUT", "command timed out while waiting for emulator progress", NULL);
    }

    /* If we were running frames, decrement and check */
    if (ai_frames_to_run > 0) {
        ai_frames_to_run--;
        if (ai_frames_to_run == 0) {
            ai_paused = 1;
            ai_async_response_pending = 0;
            ai_set_stopped_reason("frame_limit");
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"ok\",\"frames_run\":%d}", ai_frames_requested);
            ai_frames_requested = 0;
            ai_command_deadline_us = 0;
            AI_SendResponse(ai_response);
        }
    }
    if (ai_steps_to_run > 0) {
        ai_steps_to_run--;
        if (ai_steps_to_run == 0) {
            ai_paused = 1;
            ai_async_response_pending = 0;
            ai_set_stopped_reason("frame_limit");
            CPU_GetStatus();
            snprintf(ai_response, sizeof(ai_response),
                "{\"status\":\"ok\",\"steps_run\":%d,\"pc\":%d}", ai_steps_requested, CPU_regPC);
            ai_steps_requested = 0;
            ai_command_deadline_us = 0;
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
