#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "netsio.h"

int main(void)
{
	NETSIO_MONITOR_Snapshot snapshot;
	NETSIO_MONITOR_TraceEntry entries[NETSIO_TRACE_CAPACITY];
	uint8_t command_frame[5] = {0x70, 0xf0, 0x01, 0x02, 0x63};
	uint8_t sync_response[6] = {NETSIO_SYNC_RESPONSE, 7, 1, 'A', 0x34, 0x12};
	uint8_t speed[5] = {NETSIO_SPEED_CHANGE, 0x00, 0xe1, 0x00, 0x00};
	uint8_t credit[2] = {NETSIO_CREDIT_STATUS, 3};
	uint8_t ping = NETSIO_PING_REQUEST;
	size_t count;
	int i;

	assert(NETSIO_PROCEED_OFF == 0x30);
	assert(NETSIO_PROCEED_ON == 0x31);
	assert(NETSIO_INTERRUPT_OFF == 0x40);
	assert(NETSIO_INTERRUPT_ON == 0x41);

	NETSIO_MONITOR_Init(20044);
	NETSIO_MONITOR_SetInitialized(1);
	NETSIO_MONITOR_SetTraceEnabled(1);
	NETSIO_MONITOR_ObserveCommandFrame(command_frame, sizeof(command_frame));
	NETSIO_MONITOR_ObservePacket(NETSIO_TRACE_DIRECTION_FUJINET_TO_ATARI,
		sync_response, sizeof(sync_response), 1);
	NETSIO_MONITOR_ObservePacket(NETSIO_TRACE_DIRECTION_FUJINET_TO_ATARI,
		speed, sizeof(speed), 1);
	NETSIO_MONITOR_ObservePacket(NETSIO_TRACE_DIRECTION_FUJINET_TO_ATARI,
		credit, sizeof(credit), 1);
	NETSIO_MONITOR_ObservePacket(NETSIO_TRACE_DIRECTION_ATARI_TO_FUJINET,
		&ping, sizeof(ping), 0);
	NETSIO_MONITOR_ObserveSyncTimeout();

	NETSIO_MONITOR_GetSnapshot(&snapshot);
	assert(snapshot.port == 20044);
	assert(snapshot.initialized == 1);
	assert(snapshot.rx_datagrams == 3);
	assert(snapshot.tx_datagrams == 1);
	assert(snapshot.send_errors == 1);
	assert(snapshot.sync_timeouts == 1);
	assert(snapshot.last_sync_response == 7);
	assert(snapshot.last_ack_type == 1);
	assert(snapshot.last_ack_byte == 'A');
	assert(snapshot.last_write_size == 0x1234);
	assert(snapshot.requested_baud == 57600);
	assert(snapshot.last_credit == 3);
	assert(snapshot.rx_by_id[NETSIO_SYNC_RESPONSE] == 1);
	assert(snapshot.tx_by_id[NETSIO_PING_REQUEST] == 1);

	count = NETSIO_MONITOR_ReadTrace(0, entries, NETSIO_TRACE_CAPACITY);
	assert(count == 7);
	assert(entries[0].event == NETSIO_TRACE_EVENT_SIO_COMMAND_FRAME);
	assert(entries[0].data_len == sizeof(command_frame));
	assert(memcmp(entries[0].data, command_frame, sizeof(command_frame)) == 0);

	for (i = 0; i < NETSIO_TRACE_CAPACITY + 5; i++)
		NETSIO_MONITOR_ObservePacket(NETSIO_TRACE_DIRECTION_ATARI_TO_FUJINET,
			&ping, sizeof(ping), 1);
	NETSIO_MONITOR_GetSnapshot(&snapshot);
	assert(snapshot.trace_count == NETSIO_TRACE_CAPACITY);
	assert(snapshot.trace_dropped >= 5);

	NETSIO_MONITOR_ClearTrace();
	NETSIO_MONITOR_GetSnapshot(&snapshot);
	assert(snapshot.trace_count == 0);
	assert(snapshot.trace_dropped == 0);
	return 0;
}
