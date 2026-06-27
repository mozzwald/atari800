#include "config.h"
#include <string.h>
#include <time.h>
#include "netsio.h"
#include "netsio_monitor.h"

#ifdef HAVE_WINDOWS_H
#include <windows.h>
static CRITICAL_SECTION monitor_mutex;
static INIT_ONCE monitor_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK monitor_init_lock(PINIT_ONCE once, PVOID param, PVOID *context)
{
	(void)once;
	(void)param;
	(void)context;
	InitializeCriticalSection(&monitor_mutex);
	return TRUE;
}

static void monitor_lock(void)
{
	InitOnceExecuteOnce(&monitor_once, monitor_init_lock, NULL, NULL);
	EnterCriticalSection(&monitor_mutex);
}

static void monitor_unlock(void)
{
	LeaveCriticalSection(&monitor_mutex);
}

static uint64_t monitor_timestamp_us(void)
{
	return (uint64_t)GetTickCount64() * 1000ULL;
}
#else
#include <pthread.h>
static pthread_mutex_t monitor_mutex = PTHREAD_MUTEX_INITIALIZER;

static void monitor_lock(void)
{
	pthread_mutex_lock(&monitor_mutex);
}

static void monitor_unlock(void)
{
	pthread_mutex_unlock(&monitor_mutex);
}

static uint64_t monitor_timestamp_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

static NETSIO_MONITOR_Snapshot monitor;
static NETSIO_MONITOR_TraceEntry trace_entries[NETSIO_TRACE_CAPACITY];
static size_t trace_head;

static void monitor_add_trace(uint8_t event, uint8_t direction, uint8_t id, const uint8_t *data, size_t len)
{
	NETSIO_MONITOR_TraceEntry *entry;
	size_t copy_len;

	if (!monitor.trace_enabled)
		return;
	if (monitor.trace_count == NETSIO_TRACE_CAPACITY) {
		trace_head = (trace_head + 1) % NETSIO_TRACE_CAPACITY;
		monitor.trace_dropped++;
	}
	else {
		monitor.trace_count++;
	}
	entry = &trace_entries[(trace_head + monitor.trace_count - 1) % NETSIO_TRACE_CAPACITY];
	memset(entry, 0, sizeof(*entry));
	entry->seq = monitor.trace_next_seq++;
	entry->timestamp_us = monitor_timestamp_us();
	entry->event = event;
	entry->direction = direction;
	entry->id = id;
	entry->packet_len = len > 0xffff ? 0xffff : (uint16_t)len;
	copy_len = len < NETSIO_TRACE_DATA_MAX ? len : NETSIO_TRACE_DATA_MAX;
	entry->data_len = (uint8_t)copy_len;
	if (copy_len > 0 && data != NULL)
		memcpy(entry->data, data, copy_len);
}

void NETSIO_MONITOR_Init(uint16_t port)
{
	monitor_lock();
	memset(&monitor, 0, sizeof(monitor));
	memset(trace_entries, 0, sizeof(trace_entries));
	trace_head = 0;
	monitor.port = port;
	monitor.trace_next_seq = 1;
	monitor_unlock();
}

void NETSIO_MONITOR_SetInitialized(int initialized)
{
	monitor_lock();
	monitor.initialized = initialized ? 1 : 0;
	monitor_unlock();
}

void NETSIO_MONITOR_ObservePacket(int direction, const uint8_t *data, size_t len, int success)
{
	uint8_t id = len > 0 && data != NULL ? data[0] : 0;
	uint64_t now = monitor_timestamp_us();

	monitor_lock();
	if (direction == NETSIO_TRACE_DIRECTION_FUJINET_TO_ATARI) {
		monitor.rx_datagrams++;
		monitor.rx_bytes += len;
		monitor.last_rx_timestamp_us = now;
		monitor.rx_by_id[id]++;
		if (id == NETSIO_SYNC_RESPONSE && len >= 6) {
			monitor.last_sync_response = data[1];
			monitor.last_ack_type = data[2];
			monitor.last_ack_byte = data[3];
			monitor.last_write_size = (uint16_t)data[4] | (uint16_t)data[5] << 8;
		}
		else if (id == NETSIO_SPEED_CHANGE && len >= 5) {
			monitor.requested_baud = (uint32_t)data[1]
				| (uint32_t)data[2] << 8
				| (uint32_t)data[3] << 16
				| (uint32_t)data[4] << 24;
		}
		else if (id == NETSIO_CREDIT_STATUS && len >= 2) {
			monitor.last_credit = data[1];
		}
	}
	else {
		monitor.tx_datagrams++;
		monitor.tx_bytes += len;
		monitor.last_tx_timestamp_us = now;
		monitor.tx_by_id[id]++;
		if (!success) {
			monitor.send_errors++;
			monitor_add_trace(NETSIO_TRACE_EVENT_SEND_ERROR, (uint8_t)direction, id, data, len);
		}
	}
	monitor_add_trace(NETSIO_TRACE_EVENT_PACKET, (uint8_t)direction, id, data, len);
	monitor_unlock();
}

void NETSIO_MONITOR_ObserveCommandFrame(const uint8_t *data, size_t len)
{
	monitor_lock();
	monitor_add_trace(NETSIO_TRACE_EVENT_SIO_COMMAND_FRAME,
		NETSIO_TRACE_DIRECTION_ATARI_TO_FUJINET, 0, data, len);
	monitor_unlock();
}

void NETSIO_MONITOR_ObserveSyncTimeout(void)
{
	monitor_lock();
	monitor.sync_timeouts++;
	monitor_add_trace(NETSIO_TRACE_EVENT_SYNC_TIMEOUT,
		NETSIO_TRACE_DIRECTION_INTERNAL, 0, NULL, 0);
	monitor_unlock();
}

void NETSIO_MONITOR_SetTraceEnabled(int enabled)
{
	monitor_lock();
	monitor.trace_enabled = enabled ? 1 : 0;
	monitor_unlock();
}

void NETSIO_MONITOR_ClearTrace(void)
{
	monitor_lock();
	memset(trace_entries, 0, sizeof(trace_entries));
	trace_head = 0;
	monitor.trace_count = 0;
	monitor.trace_dropped = 0;
	monitor.trace_next_seq = 1;
	monitor_unlock();
}

void NETSIO_MONITOR_GetSnapshot(NETSIO_MONITOR_Snapshot *snapshot)
{
	if (snapshot == NULL)
		return;
	monitor_lock();
	*snapshot = monitor;
	monitor_unlock();
}

size_t NETSIO_MONITOR_ReadTrace(uint64_t since_seq, NETSIO_MONITOR_TraceEntry *entries, size_t max_entries)
{
	size_t i;
	size_t count = 0;

	if (entries == NULL || max_entries == 0)
		return 0;
	monitor_lock();
	for (i = 0; i < monitor.trace_count && count < max_entries; i++) {
		NETSIO_MONITOR_TraceEntry *entry = &trace_entries[(trace_head + i) % NETSIO_TRACE_CAPACITY];
		if (entry->seq > since_seq)
			entries[count++] = *entry;
	}
	monitor_unlock();
	return count;
}
