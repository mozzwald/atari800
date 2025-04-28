/*
* netsio.c - NetSIO interface for FujiNet-PC <-> Atari800 Emulator
*
* Uses two threads:
*  - fujinet_rx_thread: receive from FujiNet-PC, respond to pings/alives, queue complete packets to emulator
*  - emu_tx_thread: receive from emulator FIFO, queue complete packets to FujiNet-PC
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include "atari.h"
#include "netsio.h"
#include "log.h"
#include "pia.h" /* For toggling PROC & INT */
#include "pokey.h"

#define SIO_ACK           0x41  /* “A” */
#define SIO_COMPLETE      0x43  /* “C” */
typedef enum {
    ST_IDLE,           /* waiting for first command byte */
    ST_CMD,            /* collecting 5-byte command frame */
    ST_WAIT_ACK,       /* sent cmd, waiting for 0x41 */
    ST_ACK,            /* got ACK */
    ST_FRAME,          /* frame response to atari */
    ST_FRAME_FINAL,    /* chksum byte */
    ST_DATA,           /* now streaming payload */
} sio_state_t;

static sio_state_t state;
static UBYTE     cmd_frame[5];
static UBYTE     DataBuffer[260];    /* big enough for 128+1 or 4+1 */
static int       cmd_index;
static int       DataIndex;
static int       ExpectedBytes;

/* Flag to know when netsio is enabled */
volatile int netsio_enabled = 0;
/* Holds sync to fujinet-pc incremented number */
uint8_t netsio_sync_num = 0;
/* if we have heard from fujinet-pc or not */
int fujinet_known = 0;
/* wait for fujinet sync if true */
int netsio_sync_wait = 0;
/* true if cmd line pulled */
int netsio_cmd_state = 0;
/* holds how big the next emu -> netsio write size will be*/
uint16_t netsio_next_write_size = 0;

/* FIFO pipes:
* fds0: FujiNet->emulator
* fds1: emulator->FujiNet
*/
int fds0[2], fds1[2];

/* UDP socket for NetSIO and return address holder */
static int sockfd = -1;
static struct sockaddr_storage fujinet_addr;
static socklen_t fujinet_addr_len = sizeof(fujinet_addr);

/* Thread declarations */
static void *fujinet_rx_thread(void *arg);
static void *emu_tx_thread(void *arg);

char *buf_to_hex(const uint8_t *buf, size_t offset, size_t len) {
    /* each byte takes "XX " == 3 chars, +1 for trailing NUL */
    size_t needed = len * 3 + 1;
    char *s = malloc(needed);
    size_t i = 0;
    if (!s) return NULL;
    char *p = s;
    for (i = 0; i < len; i++) {
        sprintf(p, "%02X ", buf[offset + i]);
        p += 3;
    }
    if (len) {
        p[-1] = '\0';
    } else {
        *p = '\0';
    }
    return s;
}

/* write data to emulator FIFO (fujinet_rx_thread) */
static void enqueue_to_emulator(const uint8_t *pkt, size_t len) {
    ssize_t n;
    while (len > 0) {
        n = write(fds0[1], pkt, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("netsio: write to emulator FIFO");
            /*exit(1);*/
        }
        pkt += n;
        len -= n;
    }
}

/* send a packet to FujiNet socket */
static void send_to_fujinet(const uint8_t *pkt, size_t len) {
    ssize_t n;

    /* if we never received a ping from FujiNet or we have no address to reply to */
    if (!fujinet_known || fujinet_addr.ss_family != AF_INET) {
        Log_print("netsio: can't send_to_fujinet, no address");
        return;
    }

    n = sendto(
        sockfd,
        pkt, len, 0,
        (struct sockaddr *)&fujinet_addr,
        fujinet_addr_len
    );
    if (n < 0) {
        if (errno == EINTR) {
            /* transient, try once more */
            n = sendto(
                sockfd,
                pkt, len, 0,
                (struct sockaddr *)&fujinet_addr,
                fujinet_addr_len
            );
        }
        if (n < 0) {
            perror("netsio: sendto FujiNet");
            return;
        }
    } else if ((size_t)n != len) {
        Log_print("netsio: partial send (%zd of %zu bytes)", n, len);
        return;
    }

    /* build a hex string: each byte "XX " */
    size_t buf_size = len * 3 + 1;
    char hexdump[buf_size];
    size_t pos = 0;
    size_t i = 0;
    for (i = 0; i < len; i++) {
        /* snprintf returns number of chars (excluding trailing NUL) */
        int written = snprintf(&hexdump[pos], buf_size - pos, "%02X ", pkt[i]);
        if (written < 0 || (size_t)written >= buf_size - pos) {
            break;
        }
        pos += written;
    }
    hexdump[pos] = '\0';
    /* Log_print("netsio: send: %zu bytes → %s", len, hexdump); */
}


/* Send a single byte as a DATA_BYTE packet */
void send_byte_to_fujinet(uint8_t data_byte) {
    uint8_t packet[2];
    packet[0] = NETSIO_DATA_BYTE;
    packet[1] = data_byte;
    send_to_fujinet(packet, sizeof(packet));
}

/* Send up to 512 bytes as a DATA_BLOCK packet */
void send_block_to_fujinet(const uint8_t *block, size_t len) {
    if (len == 0 || len > 512) return;  /* sanity check */

    uint8_t packet[512 + 2];
    packet[0] = NETSIO_DATA_BLOCK;
    memcpy(&packet[1], block, len);
    /* Pad the end with a junk byte or FN-PC won't accept the packet */
    packet[1 + len] = 0xFF;
    send_to_fujinet(packet, len + 2);
}

/* Initialize NetSIO:
*   - connect to FujiNet socket
*   - create FIFOs
*   - spawn the two threads
*/
int netsio_init(uint16_t port) {
    struct sockaddr_in addr;
    pthread_t rx_thread, tx_thread;

    /* create emulator <-> netsio FIFOs */
    if (pipe(fds0) < 0 || pipe(fds1) < 0) {
        perror("netsio: pipe");
        return -1;
    }
    /* fds0[0] = emulator reads here (FujiNet->emu)
    fds0[1] = netsio_rx_thread writes here */
    /* fds1[0] = netsio_tx_thread reads here
    fds1[1] = emulator writes here (emu->FujiNet) */

    /* connect socket to FujiNet */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("netsio: socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* Bind to the socket on requested port */
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("netsio bind");
    close(sockfd);
    }

    /* spawn receiver thread */
    if (pthread_create(&rx_thread, NULL, fujinet_rx_thread, NULL) != 0) {
        perror("netsio: pthread_create rx");
        return -1;
    }
    pthread_detach(rx_thread);

    return 0;
}

/* Return number of bytes waiting from FujiNet to emulator */
int netsio_available(void) {
    int avail = 0;
    if (fds0[0] >= 0) {
        if (ioctl(fds0[0], FIONREAD, &avail) < 0) {
                Log_print("netsio_avail: ioctl error");
                return -1;
        }
    }
    return avail;
}

/* COMMAND ON */
int netsio_cmd_on(void)
{
    state = ST_CMD; /* set state machine */
    Log_print("netsio: CMD ON");
    netsio_cmd_state = 1;
    uint8_t p = NETSIO_COMMAND_ON;
    send_to_fujinet(&p, 1);
    return 0;
}

/* COMMAND OFF */
int netsio_cmd_off(void)
{
    Log_print("netsio: CMD OFF");
    uint8_t p = NETSIO_COMMAND_OFF;
    send_to_fujinet(&p, 1);
    return 0;
}

/* COMMAND OFF with SYNC */
int netsio_cmd_off_sync(void)
{
    /* Send the whole command frame */
    netsio_send_block((const uint8_t*)cmd_frame, sizeof(cmd_frame));
    /* Send command off sync */
    Log_print("netsio: CMD OFF SYNC");
    uint8_t p[2] = { NETSIO_COMMAND_OFF_SYNC, netsio_sync_num };
    send_to_fujinet(&p, sizeof(p));
    netsio_sync_num++;
    /* freeze emulation til we hear back or timeout occurs */
    netsio_sync_wait = 1;
    return 0;
}

/* Toggle Command Line */
void netsio_toggle_cmd(int v)
{
    if (!v)
        netsio_cmd_off_sync();
    else
        netsio_cmd_on();
}

/* The emulator calls this to send a data byte out to FujiNet */
int netsio_send_byte(uint8_t b) {
    uint8_t pkt[2] = { NETSIO_DATA_BYTE, b };
    Log_print("netsio: send byte: %02X", b);
    send_to_fujinet(&pkt, 2);
    return 0;
}

/* The emulator calls this to send a data block out to FujiNet */
int netsio_send_block(const uint8_t *block, ssize_t len) {
    /* ssize_t len = sizeof(block);*/ 
    send_block_to_fujinet(block, len);
    Log_print("netsio: send block, %i bytes:\n  %s", len, buf_to_hex(block, 0, len));
}

/* The emulator calls this to receive a data byte from FujiNet */
int netsio_recv_byte(uint8_t *b) {
    ssize_t n = read(fds0[0], b, 1);
    if (n < 0) {
        if (errno == EINTR) return netsio_recv_byte(b);
        perror("netsio: read from rx FIFO");
        return -1;
    }
    if (n == 0) {
        /* FIFO closed? */
        return -1;
    }
    Log_print("netsio: read to emu: %02X", (unsigned)*b);
    return 0;
}

/* Send a test command frame to fujinet-pc */
void netsio_test_cmd(void)
{
    uint8_t p[6] = { 0x70, 0xE8, 0x00, 0x00, 0x59 }; /* Send fujidev get adapter config request */
    netsio_cmd_on(); /* Turn on CMD */
    send_block_to_fujinet(p, sizeof(p));
    netsio_cmd_off_sync(); /* Turn off CMD */
}

/* Thread: receive from FujiNet socket (one packet == one command) */
static void *fujinet_rx_thread(void *arg) {
    uint8_t buf[4096];
    uint8_t packet[65536];

    for (;;) {
        fujinet_addr_len = sizeof(fujinet_addr);
        ssize_t n = recvfrom(sockfd,
                             buf, sizeof(buf),
                             0,
                             (struct sockaddr *)&fujinet_addr,
                             &fujinet_addr_len);
        if (n <= 0) {
            perror("netsio: recv");
            continue;
        }
        fujinet_known = 1;

        /* Every packet must be at least one byte (the command) */
        if (n < 1) {
            Log_print("netsio: empty packet");
            continue;
        }

        uint8_t cmd = buf[0];

        switch (cmd) {
            case NETSIO_PING_REQUEST: {
                uint8_t r = NETSIO_PING_RESPONSE;
                send_to_fujinet(&r, 1);
                Log_print("netsio: recv: PING→PONG");
                break;
            }

            case NETSIO_DEVICE_CONNECTED: {
                Log_print("netsio: recv: device connected");
                /* give it some credits 
                uint8_t reply[2] = { NETSIO_CREDIT_UPDATE, 3 };
                send_to_fujinet(reply, sizeof(reply)); */
                netsio_enabled = 1;
                break;
            }

            case NETSIO_DEVICE_DISCONNECTED: {
                Log_print("netsio: recv: device disconnected");
                netsio_enabled = 0;
                break;
            }
            
            case NETSIO_ALIVE_REQUEST: {
                uint8_t r = NETSIO_ALIVE_RESPONSE;
                send_to_fujinet(&r, 1);
                Log_print("netsio: recv: IT'S ALIVE!");
                break;
            }

            case NETSIO_CREDIT_STATUS: {
                /* packet should be 2 bytes long */
                if (n < 2) {
                    Log_print("netsio: recv: CREDIT_STATUS packet too short (%zd)", n);
                }
                uint8_t reply[2] = { NETSIO_CREDIT_UPDATE, 3 };
                send_to_fujinet(reply, sizeof(reply));
                Log_print("netsio: recv: credit status & response");
                break;
            }

            case NETSIO_SPEED_CHANGE: {
                /* packet: [cmd][baud32le] */
                if (n < 5) {
                    Log_print("netsio: recv: SPEED_CHANGE packet too short (%zd)", n);
                    break;
                }
                uint32_t baud = buf[1]
                              | (uint32_t)buf[2] << 8
                              | (uint32_t)buf[3] << 16
                              | (uint32_t)buf[4] << 24;
                Log_print("netsio: recv: requested baud rate %u", baud);
                /* TODO: apply baud somehow */
                break;
            }

            case NETSIO_SYNC_RESPONSE: {
                /* packet: [cmd][sync#][ack_type][ack_byte][write_lo][write_hi] */
                if (n < 6) {
                    Log_print("netsio: recv: SYNC_RESPONSE too short (%zd)", n);
                    break;
                }
                uint8_t  resp_sync  = buf[1];
                uint8_t  ack_type   = buf[2];
                uint8_t  ack_byte   = buf[3];
                netsio_next_write_size = buf[4] | (uint16_t)buf[5] << 8;

                if (resp_sync != netsio_sync_num - 1) {
                    Log_print("netsio: recv: sync-response: got %u, want %u",
                              resp_sync, netsio_sync_num - 1);
                } else {
                    if (ack_type == 0) {
                        Log_print("netsio: recv: sync %u NAK, dropping", resp_sync);
                        state = ST_IDLE;
                    } else if (ack_type == 1) {
                        Log_print("netsio: recv: sync %u ACK byte=0x%02X",
                                  resp_sync, ack_byte);
                        enqueue_to_emulator(&ack_byte, 1);
                        state = ST_ACK;
                    } else {
                        Log_print("netsio: recv: sync %u unknown ack_type %u",
                                  resp_sync, ack_type);
                        state = ST_IDLE;
                    }
                }
                netsio_sync_wait = 0; /* unfreeze emulation */
                break;
            }

            /* set_CA1 */
            case NETSIO_PROCEED_ON: {

                break;
            }
            case NETSIO_PROCEED_OFF: {

                break;
            }

            /* set_CB1 */
            case NETSIO_INTERRUPT_ON: {

                break;
            }
            case NETSIO_INTERRUPT_OFF: {

                break;
            }
            case NETSIO_DATA_BYTE: {
                /* packet: [cmd][data] */
                if (n < 2) {
                    Log_print("netsio: recv: DATA_BYTE too short (%zd)", n);
                    break;
                }
                uint8_t data = buf[1];
                Log_print("netsio: recv: data byte: 0x%02X", data);
                enqueue_to_emulator(&data, 1);
                break;
            }

            case NETSIO_DATA_BLOCK: {
                /* packet: [cmd][payload...] */
                if (n < 2) {
                    Log_print("netsio: recv: data block too short (%zd)", n);
                    break;
                }
                /* payload length is everything after the command byte */
                size_t payload_len = n - 1;
                Log_print("netsio: recv: data block %zu bytes:\n  %s", payload_len, buf_to_hex(buf, 1, payload_len));
                /* forward only buf[1]..buf[n-1] */
                enqueue_to_emulator(buf + 1, payload_len);
                break;
            }            

            default:
                Log_print("netsio: recv: unknown cmd 0x%02X, length %zd", cmd, n);
                break;
        }
    }
    return NULL;
}

/* NetSIO State Machine */
void SIO_Net_PutByte(UBYTE out_byte)
{
/* failed raw send/recv disabled
    if (netsio_cmd_state)
    {
        /* capturing the command frame from atari 
        cmd_frame[cmd_index++] = out_byte;

        if (cmd_index == 5)
        {
            state = ST_ACK;
            POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
        }
    }
    else
    {
        /* Send out the byte to netsio 
        netsio_send_byte(out_byte);
    }

    /* POKEY_DELAYED_SEROUT_IRQ = SIO_SEROUT_INTERVAL; /* according to sio.c this is already set in pokey.c
    return; */

    /* state machine */
    switch (state) {
    case ST_IDLE:
        /* beginning of a new frame */
        cmd_index = 0;
        /* fall through */

    case ST_CMD:
        /* buffer it */
        cmd_frame[cmd_index++] = out_byte;
        /* command frame send moved to netsio_cmd_off_sync() so we can
           send a block instead of individual bytes */        

        /* once we have all 5 bytes, wait for ACK */
        if (cmd_index == 5) {
            state = ST_ACK;
            POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
        }
        break;
    case ST_FRAME_FINAL:

        break;
    default:
        break;
    }
}

UBYTE SIO_Net_GetByte(void)
{
    UBYTE b, ack;
    int i = 0;

    /* failed raw send/recv disabled
    if (netsio_available())
    {
        netsio_recv_byte(&b);
        return b;
    }

    return 0; */

    /* state machine */
    switch (state) {
    case ST_ACK:
        /* Got ACK (0x41) */
        if (netsio_recv_byte(&b) < 0) return 0;
        if (b == 0x41) {
            ack = b;
            /* queue the data  */
            switch(cmd_frame[1])
            {
                case 0x4e: /* Read Status */
                    POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    break;
                case 0x52: /* read */
                    POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL << 2;
                    break;
                case 0x53: /* status */
                case 0xD3: /* xf551 hispeed */
                    Log_print("netsio: ACK! 0x53 STATUS");
        
                    netsio_recv_byte(&b);
                    if (b != 0x43)
                    {
                        state = ST_IDLE;
                        Log_print("netsio: state: ACK, no complete");
                        return 0;       /* no Complete, bail out */
                    }

                    /* queue data into the buffer, 4 bytes */
                    for (i = 0; i < 4; i++) {
                        netsio_recv_byte(&b);
                        DataBuffer[1 + i] = b;
                    }
        
                    DataBuffer[0] = 'C';   /* Complete */
                    netsio_recv_byte(&b);  /* get chksum */
                    DataBuffer[5] = b;     /* put chksum */
                
                    DataIndex      = 0;
                    ExpectedBytes  = 6;    /* 'C' + 4 data + chksum */
                    POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    state = ST_FRAME;
                    return 'A';
            }
        } else {
            /* unexpected—abort */
            state = ST_IDLE;
        }
        Log_print("netsio: state ACK: return 0 failure");
        return 0;
    case ST_FRAME:
        if (DataIndex < ExpectedBytes)
        {
            b = DataBuffer[DataIndex++];
            
            if (DataIndex >= ExpectedBytes)
            {
                state = ST_IDLE;
                Log_print("netsio: state change FRAME->IDLE");
            }
            else
            {
                /* set delay using the expected transfer speed */
                POKEY_DELAYED_SERIN_IRQ = (DataIndex == 1) ? SIO_SERIN_INTERVAL
                    : ((SIO_SERIN_INTERVAL * POKEY_AUDF[POKEY_CHAN3] - 1) / 0x28 + 1);
            }
        }
        else
        {
            Log_print("Invalid read frame!");
            state = ST_IDLE;
        }
        Log_print("netsio: state FRAME: to emu: %02X", b);
        return b;
        /* and older try at ST_FRAME state, we never get here */
        if (DataIndex < ExpectedBytes) {
            /* grab the next byte out of our buffer */
            b = DataBuffer[DataIndex++];
            /* if this was the very last byte (the checksum), idle afterwards */
            if (DataIndex == ExpectedBytes)
            {
                state = ST_IDLE;
                /* last byte needs the extra ACK‐interval delay */
                /* Log_print("netsio: state FRAME: pokey ACK interval"); */
                POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
            }
            else
            {
                /* every other byte is paced at the normal serial‐in rate */
                POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
            }
            /* set delay using the expected transfer speed */
            POKEY_DELAYED_SERIN_IRQ = (DataIndex == 1) ? SIO_SERIN_INTERVAL
            : ((SIO_SERIN_INTERVAL * POKEY_AUDF[POKEY_CHAN3] - 1) / 0x28 + 1);

            Log_print("netsio: state FRAME: pass byte %02X", b);
            return b;
        }
        /* shouldn’t ever hit this, but if we do, bail out */
        Log_print("netsio: state FRAME: Unexpected extra byte");
        state = ST_IDLE;
        return 0;
    case ST_FRAME_FINAL:
        /* This state seems useless and is never entered */
        state = ST_IDLE;
        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
        return 0;
    case ST_DATA:
        /* stream payload bytes */
        if (netsio_recv_byte(&b) < 0) return 0;
        
        /*data_buffer[data_index++] = b;
        if (data_index >= expected_bytes) {
            state = ST_IDLE;
        }*/
        return b;

    default:
        return 0;
    }
}
