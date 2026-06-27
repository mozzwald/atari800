#ifndef NETSIO_MONITOR_H
#define NETSIO_MONITOR_H

#include <stddef.h>
#include <stdint.h>

#define NETSIO_TRACE_CAPACITY 256
#define NETSIO_TRACE_DATA_MAX 32

enum {
	NETSIO_TRACE_DIRECTION_ATARI_TO_FUJINET = 0,
	NETSIO_TRACE_DIRECTION_FUJINET_TO_ATARI = 1,
	NETSIO_TRACE_DIRECTION_INTERNAL = 2
};

enum {
	NETSIO_TRACE_EVENT_PACKET = 0,
	NETSIO_TRACE_EVENT_SIO_COMMAND_FRAME = 1,
	NETSIO_TRACE_EVENT_SYNC_TIMEOUT = 2,
	NETSIO_TRACE_EVENT_SEND_ERROR = 3
};

typedef struct NETSIO_MONITOR_TraceEntry {
	uint64_t seq;
	uint64_t timestamp_us;
	uint8_t event;
	uint8_t direction;
	uint8_t id;
	uint16_t packet_len;
	uint8_t data_len;
	uint8_t data[NETSIO_TRACE_DATA_MAX];
} NETSIO_MONITOR_TraceEntry;

typedef struct NETSIO_MONITOR_Snapshot {
	uint16_t port;
	uint8_t initialized;
	uint8_t trace_enabled;
	uint64_t rx_datagrams;
	uint64_t tx_datagrams;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t send_errors;
	uint64_t sync_timeouts;
	uint64_t last_rx_timestamp_us;
	uint64_t last_tx_timestamp_us;
	uint8_t last_sync_response;
	uint8_t last_ack_type;
	uint8_t last_ack_byte;
	uint16_t last_write_size;
	uint32_t requested_baud;
	uint8_t last_credit;
	uint64_t rx_by_id[256];
	uint64_t tx_by_id[256];
	uint64_t trace_next_seq;
	size_t trace_count;
	uint64_t trace_dropped;
} NETSIO_MONITOR_Snapshot;

void NETSIO_MONITOR_Init(uint16_t port);
void NETSIO_MONITOR_SetInitialized(int initialized);
void NETSIO_MONITOR_ObservePacket(int direction, const uint8_t *data, size_t len, int success);
void NETSIO_MONITOR_ObserveCommandFrame(const uint8_t *data, size_t len);
void NETSIO_MONITOR_ObserveSyncTimeout(void);
void NETSIO_MONITOR_SetTraceEnabled(int enabled);
void NETSIO_MONITOR_ClearTrace(void);
void NETSIO_MONITOR_GetSnapshot(NETSIO_MONITOR_Snapshot *snapshot);
size_t NETSIO_MONITOR_ReadTrace(uint64_t since_seq, NETSIO_MONITOR_TraceEntry *entries, size_t max_entries);

#endif
