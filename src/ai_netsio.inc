#ifdef NETSIO
static const char *ai_netsio_type_name(unsigned int id)
{
	switch (id) {
	case NETSIO_DATA_BYTE: return "DATA_BYTE";
	case NETSIO_DATA_BLOCK: return "DATA_BLOCK";
	case NETSIO_DATA_BYTE_SYNC: return "DATA_BYTE_SYNC";
	case NETSIO_COMMAND_OFF: return "COMMAND_OFF";
	case NETSIO_COMMAND_ON: return "COMMAND_ON";
	case NETSIO_COMMAND_OFF_SYNC: return "COMMAND_OFF_SYNC";
	case NETSIO_MOTOR_OFF: return "MOTOR_OFF";
	case NETSIO_MOTOR_ON: return "MOTOR_ON";
	case NETSIO_PROCEED_OFF: return "PROCEED_OFF";
	case NETSIO_PROCEED_ON: return "PROCEED_ON";
	case NETSIO_INTERRUPT_OFF: return "INTERRUPT_OFF";
	case NETSIO_INTERRUPT_ON: return "INTERRUPT_ON";
	case NETSIO_SPEED_CHANGE: return "SPEED_CHANGE";
	case NETSIO_SYNC_RESPONSE: return "SYNC_RESPONSE";
	case NETSIO_DEVICE_DISCONNECTED: return "DEVICE_DISCONNECTED";
	case NETSIO_DEVICE_CONNECTED: return "DEVICE_CONNECTED";
	case NETSIO_PING_REQUEST: return "PING_REQUEST";
	case NETSIO_PING_RESPONSE: return "PING_RESPONSE";
	case NETSIO_ALIVE_REQUEST: return "ALIVE_REQUEST";
	case NETSIO_ALIVE_RESPONSE: return "ALIVE_RESPONSE";
	case NETSIO_CREDIT_STATUS: return "CREDIT_STATUS";
	case NETSIO_CREDIT_UPDATE: return "CREDIT_UPDATE";
	case NETSIO_WARM_RESET: return "WARM_RESET";
	case NETSIO_COLD_RESET: return "COLD_RESET";
	default: return "UNKNOWN";
	}
}

static const char *ai_netsio_direction_name(unsigned int direction)
{
	if (direction == NETSIO_TRACE_DIRECTION_ATARI_TO_FUJINET)
		return "atari_to_fujinet";
	if (direction == NETSIO_TRACE_DIRECTION_FUJINET_TO_ATARI)
		return "fujinet_to_atari";
	return "internal";
}

static const char *ai_netsio_event_name(const NETSIO_MONITOR_TraceEntry *entry)
{
	if (entry->event == NETSIO_TRACE_EVENT_SIO_COMMAND_FRAME)
		return "SIO_COMMAND_FRAME";
	if (entry->event == NETSIO_TRACE_EVENT_SYNC_TIMEOUT)
		return "SYNC_TIMEOUT";
	if (entry->event == NETSIO_TRACE_EVENT_SEND_ERROR)
		return "SEND_ERROR";
	return ai_netsio_type_name(entry->id);
}

static void ai_append_netsio_counts(size_t *pos, const uint64_t *counts)
{
	int id;
	int first = TRUE;

	ai_append(ai_response, sizeof(ai_response), pos, "{");
	for (id = 0; id < 256; id++) {
		if (counts[id] == 0)
			continue;
		ai_append(ai_response, sizeof(ai_response), pos, "%s\"0x%02x\":%llu",
			first ? "" : ",", id, (unsigned long long)counts[id]);
		first = FALSE;
	}
	ai_append(ai_response, sizeof(ai_response), pos, "}");
}

static void ai_send_netsio_status(void)
{
	NetSIOStatus status;
	size_t pos = 0;

	netsio_get_status(&status);
	ai_response[0] = 0;
	ai_append(ai_response, sizeof(ai_response), &pos,
		"{\"status\":\"ok\",\"compiled\":true,\"initialized\":%s,\"enabled\":%s,"
		"\"fujinet_known\":%s,\"port\":%u,\"peer\":{\"address\":\"%s\",\"port\":%u},"
		"\"sync\":{\"number\":%u,\"wait_active\":%s,\"last_response\":%u,"
		"\"last_ack_type\":%u,\"last_ack_byte\":%u,\"timeouts\":%llu},"
		"\"queue\":{\"rx_available\":%d,\"capacity\":%d,\"next_write_size\":%d},"
		"\"pins\":{\"proceed_ca1\":%d,\"interrupt_cb1\":%d,\"active_low\":true,"
		"\"proceed_ids\":{\"off\":48,\"on\":49},\"interrupt_ids\":{\"off\":64,\"on\":65}},"
		"\"credit\":{\"last_status\":%u},"
		"\"timestamps\":{\"last_rx_us\":%llu,\"last_tx_us\":%llu},"
		"\"counters\":{\"rx_datagrams\":%llu,\"tx_datagrams\":%llu,"
		"\"rx_bytes\":%llu,\"tx_bytes\":%llu,\"send_errors\":%llu},"
		"\"netstream\":{\"active\":%s,\"pending_enable\":%s,\"fuji_enable_accepted\":%s,"
		"\"motor_asserted\":%s,\"pokey_compatible\":%s,\"current_baud\":%u,"
		"\"final_audf3\":%u,\"final_audf4\":%u,\"transport\":\"udp\","
		"\"requested_flags\":null,\"final_flags\":null,\"register_enabled\":null,"
		"\"udp_sequencing_enabled\":null,\"detected_video_standard\":null},"
		"\"trace\":{\"enabled\":%s,\"count\":%lu,\"capacity\":%d,"
		"\"next_seq\":%llu,\"dropped\":%llu},\"message_counts\":{\"rx\":",
		status.initialized ? "true" : "false", status.enabled ? "true" : "false",
		status.fujinet_known ? "true" : "false", (unsigned)status.port,
		status.peer_address, (unsigned)status.peer_port, (unsigned)status.sync_num,
		status.sync_wait ? "true" : "false", (unsigned)status.monitor.last_sync_response,
		(unsigned)status.monitor.last_ack_type, (unsigned)status.monitor.last_ack_byte,
		(unsigned long long)status.monitor.sync_timeouts,
		status.fifo_available, status.fifo_capacity, status.next_write_size,
		status.proceed_ca1, status.interrupt_cb1, (unsigned)status.monitor.last_credit,
		(unsigned long long)status.monitor.last_rx_timestamp_us,
		(unsigned long long)status.monitor.last_tx_timestamp_us,
		(unsigned long long)status.monitor.rx_datagrams,
		(unsigned long long)status.monitor.tx_datagrams,
		(unsigned long long)status.monitor.rx_bytes,
		(unsigned long long)status.monitor.tx_bytes,
		(unsigned long long)status.monitor.send_errors,
		status.netstream_active ? "true" : "false",
		status.netstream_pending_enable ? "true" : "false",
		status.netstream_fuji_enabled ? "true" : "false",
		status.netstream_motor_on ? "true" : "false",
		status.netstream_pokey_compatible ? "true" : "false",
		(unsigned)status.monitor.requested_baud, (unsigned)status.audf3, (unsigned)status.audf4,
		status.monitor.trace_enabled ? "true" : "false",
		(unsigned long)status.monitor.trace_count, NETSIO_TRACE_CAPACITY,
		(unsigned long long)status.monitor.trace_next_seq,
		(unsigned long long)status.monitor.trace_dropped);
	ai_append_netsio_counts(&pos, status.monitor.rx_by_id);
	ai_append(ai_response, sizeof(ai_response), &pos, ",\"tx\":");
	ai_append_netsio_counts(&pos, status.monitor.tx_by_id);
	ai_append(ai_response, sizeof(ai_response), &pos,
		"},\"handler_fields_note\":\"requested/final flags, REGISTER, UDP sequencing, and video standard require Atari-side handler telemetry\"}");
	AI_SendResponse(ai_response);
}

static void ai_send_netsio_trace_status(void)
{
	NETSIO_MONITOR_Snapshot snapshot;
	NETSIO_MONITOR_GetSnapshot(&snapshot);
	snprintf(ai_response, sizeof(ai_response),
		"{\"status\":\"ok\",\"enabled\":%s,\"count\":%lu,\"capacity\":%d,"
		"\"next_seq\":%llu,\"dropped\":%llu}",
		snapshot.trace_enabled ? "true" : "false",
		(unsigned long)snapshot.trace_count, NETSIO_TRACE_CAPACITY,
		(unsigned long long)snapshot.trace_next_seq,
		(unsigned long long)snapshot.trace_dropped);
	AI_SendResponse(ai_response);
}

static void ai_send_netsio_trace_read(uint64_t since_seq, size_t limit)
{
	NETSIO_MONITOR_TraceEntry entries[NETSIO_TRACE_CAPACITY];
	size_t count;
	size_t i;
	size_t pos = 0;
	uint64_t last_seq = since_seq;

	if (limit == 0 || limit > NETSIO_TRACE_CAPACITY)
		limit = NETSIO_TRACE_CAPACITY;
	count = NETSIO_MONITOR_ReadTrace(since_seq, entries, limit);
	ai_response[0] = 0;
	ai_append(ai_response, sizeof(ai_response), &pos, "{\"status\":\"ok\",\"entries\":[");
	for (i = 0; i < count; i++) {
		NETSIO_MONITOR_TraceEntry *entry = &entries[i];
		size_t j;
		ai_append(ai_response, sizeof(ai_response), &pos,
			"%s{\"seq\":%llu,\"timestamp_us\":%llu,\"direction\":\"%s\","
			"\"type\":\"%s\",\"id\":%u,\"packet_len\":%u,\"data_hex\":\"",
			i ? "," : "", (unsigned long long)entry->seq,
			(unsigned long long)entry->timestamp_us,
			ai_netsio_direction_name(entry->direction), ai_netsio_event_name(entry),
			(unsigned)entry->id, (unsigned)entry->packet_len);
		for (j = 0; j < entry->data_len; j++)
			ai_append(ai_response, sizeof(ai_response), &pos, "%s%02x", j ? " " : "", entry->data[j]);
		ai_append(ai_response, sizeof(ai_response), &pos, "\",\"decoded\":");
		if (entry->event == NETSIO_TRACE_EVENT_SIO_COMMAND_FRAME && entry->data_len >= 5) {
			ai_append(ai_response, sizeof(ai_response), &pos,
				"{\"device\":%u,\"command\":%u,\"aux1\":%u,\"aux2\":%u,"
				"\"sector\":%u,\"checksum\":%u}",
				(unsigned)entry->data[0], (unsigned)entry->data[1],
				(unsigned)entry->data[2], (unsigned)entry->data[3],
				(unsigned)(entry->data[2] | (uint16_t)entry->data[3] << 8),
				(unsigned)entry->data[4]);
		}
		else if (entry->id == NETSIO_SYNC_RESPONSE && entry->data_len >= 6) {
			ai_append(ai_response, sizeof(ai_response), &pos,
				"{\"sync\":%u,\"ack_type\":%u,\"ack_byte\":%u,\"write_size\":%u}",
				(unsigned)entry->data[1], (unsigned)entry->data[2],
				(unsigned)entry->data[3],
				(unsigned)(entry->data[4] | (uint16_t)entry->data[5] << 8));
		}
		else if (entry->id == NETSIO_SPEED_CHANGE && entry->data_len >= 5) {
			unsigned long baud = (unsigned long)entry->data[1]
				| (unsigned long)entry->data[2] << 8
				| (unsigned long)entry->data[3] << 16
				| (unsigned long)entry->data[4] << 24;
			ai_append(ai_response, sizeof(ai_response), &pos, "{\"baud\":%lu}", baud);
		}
		else if (entry->id == NETSIO_CREDIT_STATUS && entry->data_len >= 2) {
			ai_append(ai_response, sizeof(ai_response), &pos,
				"{\"credit\":%u}", (unsigned)entry->data[1]);
		}
		else {
			ai_append(ai_response, sizeof(ai_response), &pos, "{}");
		}
		ai_append(ai_response, sizeof(ai_response), &pos, "}");
		last_seq = entry->seq;
	}
	ai_append(ai_response, sizeof(ai_response), &pos,
		"],\"last_seq\":%llu,\"count\":%lu}",
		(unsigned long long)last_seq, (unsigned long)count);
	AI_SendResponse(ai_response);
}
#endif
