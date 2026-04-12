/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

typedef struct observed_transfer_event {
  xyzm_transfer_event_kind_t kind;
  xyzm_transfer_protocol_t protocol;
  xyzm_transfer_direction_t direction;
  xyzm_status_t status;
  uint64_t payload_delta;
  uint32_t file_valid_mask;
  uint64_t file_size;
  char file_name[64];
  uint64_t report_payload_bytes;
  uint32_t report_completed_files;
  uint32_t report_retry_count;
  xyzm_status_t report_final_status;
} observed_transfer_event_t;

typedef struct observed_transfer_events {
  observed_transfer_event_t events[32];
  size_t count;
} observed_transfer_events_t;

static void record_transfer_event(
    void *opaque,
    const xyzm_transfer_event_t *event) {
  observed_transfer_events_t *events = opaque;
  observed_transfer_event_t *record;
  size_t name_len;

  ASSERT_TRUE(events->count < (sizeof(events->events) / sizeof(events->events[0])));
  record = &events->events[events->count++];
  memset(record, 0, sizeof(*record));
  record->kind = event->kind;
  record->protocol = event->protocol;
  record->direction = event->direction;
  record->status = event->status;
  record->payload_delta = event->payload_delta;
  if (event->file_info != NULL) {
    record->file_valid_mask = event->file_info->valid_mask;
    record->file_size = event->file_info->size_bytes;
    if (event->file_info->name != NULL) {
      name_len = strlen(event->file_info->name);
      if (name_len >= sizeof(record->file_name)) {
        name_len = sizeof(record->file_name) - 1u;
      }
      memcpy(record->file_name, event->file_info->name, name_len);
      record->file_name[name_len] = '\0';
    }
  }
  if (event->report != NULL) {
    record->report_payload_bytes = event->report->payload_bytes;
    record->report_completed_files = event->report->completed_files;
    record->report_retry_count = event->report->retry_count;
    record->report_final_status = event->report->final_status;
  }
}

static void assert_observed_events(
    const observed_transfer_events_t *events,
    xyzm_transfer_protocol_t protocol,
    xyzm_transfer_direction_t direction,
    uint64_t expected_payload_bytes) {
  size_t index;
  size_t started_count = 0u;
  size_t file_started_count = 0u;
  size_t progress_count = 0u;
  size_t retry_count = 0u;
  size_t file_finished_count = 0u;
  size_t finished_count = 0u;
  uint64_t payload_delta_sum = 0u;
  size_t file_started_index = (size_t)-1;
  size_t file_finished_index = (size_t)-1;

  ASSERT_TRUE(events->count >= 6u);
  ASSERT_TRUE(events->events[0].kind == XYZM_TRANSFER_EVENT_TRANSFER_STARTED);
  ASSERT_TRUE(events->events[events->count - 1u].kind == XYZM_TRANSFER_EVENT_TRANSFER_FINISHED);

  for (index = 0u; index < events->count; ++index) {
    const observed_transfer_event_t *event = &events->events[index];
    ASSERT_TRUE(event->protocol == protocol);
    ASSERT_TRUE(event->direction == direction);
    switch (event->kind) {
      case XYZM_TRANSFER_EVENT_TRANSFER_STARTED:
        ++started_count;
        ASSERT_EQ_STATUS(event->status, XYZM_STATUS_OK);
        break;
      case XYZM_TRANSFER_EVENT_FILE_STARTED:
        ++file_started_count;
        file_started_index = index;
        ASSERT_EQ_STATUS(event->status, XYZM_STATUS_OK);
        break;
      case XYZM_TRANSFER_EVENT_PROGRESS:
        ++progress_count;
        ASSERT_EQ_STATUS(event->status, XYZM_STATUS_OK);
        ASSERT_TRUE(event->payload_delta > 0u);
        payload_delta_sum += event->payload_delta;
        break;
      case XYZM_TRANSFER_EVENT_RETRY:
        ++retry_count;
        ASSERT_EQ_STATUS(event->status, XYZM_STATUS_OK);
        break;
      case XYZM_TRANSFER_EVENT_FILE_FINISHED:
        ++file_finished_count;
        file_finished_index = index;
        ASSERT_EQ_STATUS(event->status, XYZM_STATUS_OK);
        break;
      case XYZM_TRANSFER_EVENT_TRANSFER_FINISHED:
        ++finished_count;
        ASSERT_EQ_STATUS(event->status, XYZM_STATUS_OK);
        ASSERT_EQ_STATUS(event->report_final_status, XYZM_STATUS_OK);
        break;
      default:
        ASSERT_TRUE(false);
        break;
    }
  }

  ASSERT_EQ_SIZE(started_count, 1u);
  ASSERT_EQ_SIZE(file_started_count, 1u);
  ASSERT_TRUE(progress_count >= 1u);
  ASSERT_TRUE(retry_count >= 1u);
  ASSERT_EQ_SIZE(file_finished_count, 1u);
  ASSERT_EQ_SIZE(finished_count, 1u);
  ASSERT_TRUE(file_started_index < file_finished_index);
  ASSERT_TRUE(file_finished_index < (events->count - 1u));
  ASSERT_TRUE(payload_delta_sum == expected_payload_bytes);
}

static xyzm_transfer_observer_t build_observer(observed_transfer_events_t *events) {
  xyzm_transfer_observer_t observer;
  observer.opaque = events;
  observer.notify = record_transfer_event;
  return observer;
}

static void build_common_source(
    memory_source_t *source,
    xyzm_source_ops_t *source_ops,
    const char *name,
    const uint8_t *data,
    size_t data_len) {
  memset(source, 0, sizeof(*source));
  source->info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source->info.name = name;
  source->info.size_bytes = data_len;
  source->data = data;
  source->data_len = data_len;
  source_ops->opaque = source;
  source_ops->next = memory_source_next;
  source_ops->read = memory_source_read;
  source_ops->end = memory_source_end;
}

static void build_common_sink(
    memory_sink_t *sink,
    xyzm_sink_ops_t *sink_ops) {
  memset(sink, 0, sizeof(*sink));
  sink_ops->opaque = sink;
  sink_ops->begin = memory_sink_begin;
  sink_ops->write = memory_sink_write;
  sink_ops->end = memory_sink_end;
}

static void build_common_link(
    fake_link_t *link,
    xyzm_link_ops_t *link_ops) {
  link_ops->opaque = link;
  link_ops->send = fake_send;
  link_ops->recv = fake_recv;
  link_ops->now_ms = fake_now_ms;
}

static void observe_send(
    test_sync_protocol_t protocol,
    xyzm_transfer_protocol_t event_protocol,
    fake_link_t *link,
    const uint8_t *data,
    size_t data_len) {
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  observed_transfer_events_t events;
  xyzm_transfer_observer_t observer;
  xyzm_status_t status;

  build_common_link(link, &link_ops);
  build_common_source(&source, &source_ops, "event.bin", data, data_len);
  populate_sync_default_options(&options, protocol);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;
  memset(&events, 0, sizeof(events));
  observer = build_observer(&events);

  status = test_sync_send_observed(&request, &observer, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == data_len);
  assert_observed_events(
      &events,
      event_protocol,
      XYZM_TRANSFER_DIRECTION_SEND,
      data_len);
}

static void observe_receive(
    test_sync_protocol_t protocol,
    xyzm_transfer_protocol_t event_protocol,
    fake_link_t *link,
    uint64_t expected_payload_bytes) {
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  observed_transfer_events_t events;
  xyzm_transfer_observer_t observer;
  xyzm_status_t status;

  build_common_link(link, &link_ops);
  build_common_sink(&sink, &sink_ops);
  populate_sync_default_options(&options, protocol);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;
  memset(&events, 0, sizeof(events));
  observer = build_observer(&events);

  status = test_sync_receive_observed(&request, &observer, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == expected_payload_bytes);
  assert_observed_events(
      &events,
      event_protocol,
      XYZM_TRANSFER_DIRECTION_RECEIVE,
      expected_payload_bytes);
}

static void build_xmodem_send_link(fake_link_t *link) {
  memset(link, 0, sizeof(*link));
  link->timeout_recv_count = 1u;
  link->incoming[0] = XYZM_NAK;
  link->incoming[1] = XYZM_ACK;
  link->incoming[2] = XYZM_NAK;
  link->incoming[3] = XYZM_ACK;
  link->incoming_len = 4u;
}

static void build_xmodem_receive_link(fake_link_t *link) {
  uint8_t payload[128];
  size_t offset = 0u;

  memset(link, 0, sizeof(*link));
  link->timeout_recv_count = 1u;
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "event", 5u);
  append_xmodem_block(link->incoming, &offset, 1u, payload, sizeof(payload), false, XYZM_SOH);
  link->incoming[offset++] = XYZM_EOT;
  link->incoming[offset++] = XYZM_EOT;
  link->incoming_len = offset;
}

static void build_ymodem_send_link(fake_link_t *link) {
  static const uint8_t incoming[] = {
      XYZM_CRC_REQ, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK,
      XYZM_NAK, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK};

  memset(link, 0, sizeof(*link));
  link->timeout_recv_count = 1u;
  memcpy(link->incoming, incoming, sizeof(incoming));
  link->incoming_len = sizeof(incoming);
}

static void build_ymodem_receive_link(fake_link_t *link) {
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset = 0u;

  memset(link, 0, sizeof(*link));
  link->incoming[offset++] = XYZM_CRC_REQ;
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "event.bin", 9u);
  memcpy(metadata + 10u, "3", 1u);
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "ymd", 3u);
  append_xmodem_block(link->incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  append_xmodem_block(link->incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link->incoming[offset++] = XYZM_EOT;
  link->incoming[offset++] = XYZM_EOT;
  memset(metadata, 0, sizeof(metadata));
  append_xmodem_block(link->incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link->incoming_len = offset;
}

static void build_zmodem_send_link(fake_link_t *link) {
  uint8_t hdr[4];

  memset(link, 0, sizeof(*link));
  link->timeout_recv_count = 1u;
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link->incoming, &link->incoming_len, TEST_ZRINIT, hdr);
  append_zmodem_hex_header(link->incoming, &link->incoming_len, TEST_ZRPOS, hdr);
  append_zmodem_hex_header(link->incoming, &link->incoming_len, TEST_ZRINIT, hdr);
  append_zmodem_hex_header(link->incoming, &link->incoming_len, TEST_ZFIN, hdr);
}

static void build_zmodem_receive_link(fake_link_t *link) {
  uint8_t hdr[4];
  uint8_t metadata[32];
  static const uint8_t payload[] = {'z', 'm', 'o', 'd'};

  memset(link, 0, sizeof(*link));
  link->timeout_recv_count = 1u;
  memset(hdr, 0, sizeof(hdr));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "event.bin", 9u);
  metadata[10] = '4';
  append_zmodem_binary_header(link->incoming, &link->incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(link->incoming, &link->incoming_len, metadata, 12u, TEST_ZCRCW, false, false);
  append_zmodem_binary_header(link->incoming, &link->incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(link->incoming, &link->incoming_len, payload, sizeof(payload), TEST_ZCRCE, false, false);
  hdr[0] = (uint8_t)sizeof(payload);
  append_zmodem_binary_header(link->incoming, &link->incoming_len, TEST_ZEOF, hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link->incoming, &link->incoming_len, TEST_ZFIN, hdr);
  link->incoming[link->incoming_len++] = 'O';
  link->incoming[link->incoming_len++] = 'O';
}

static void test_transfer_events(void) {
  fake_link_t link;
  static const uint8_t xmodem_data[] = {'x', 'm', 'o', 'd'};
  static const uint8_t ymodem_data[] = {'y', 'm', 'o', 'd'};
  static const uint8_t zmodem_data[] = {'z', 'm', 'o', 'd'};

  build_xmodem_send_link(&link);
  observe_send(
      TEST_SYNC_PROTOCOL_XMODEM,
      XYZM_TRANSFER_PROTOCOL_XMODEM,
      &link,
      xmodem_data,
      sizeof(xmodem_data));

  build_xmodem_receive_link(&link);
  observe_receive(
      TEST_SYNC_PROTOCOL_XMODEM,
      XYZM_TRANSFER_PROTOCOL_XMODEM,
      &link,
      128u);

  build_ymodem_send_link(&link);
  observe_send(
      TEST_SYNC_PROTOCOL_YMODEM,
      XYZM_TRANSFER_PROTOCOL_YMODEM,
      &link,
      ymodem_data,
      sizeof(ymodem_data));

  build_ymodem_receive_link(&link);
  observe_receive(
      TEST_SYNC_PROTOCOL_YMODEM,
      XYZM_TRANSFER_PROTOCOL_YMODEM,
      &link,
      3u);

  build_zmodem_send_link(&link);
  observe_send(
      TEST_SYNC_PROTOCOL_ZMODEM,
      XYZM_TRANSFER_PROTOCOL_ZMODEM,
      &link,
      zmodem_data,
      sizeof(zmodem_data));

  build_zmodem_receive_link(&link);
  observe_receive(
      TEST_SYNC_PROTOCOL_ZMODEM,
      XYZM_TRANSFER_PROTOCOL_ZMODEM,
      &link,
      4u);
}

static void test_transfer_events_sync_unit(void) {
  test_transfer_events();
}

const test_case_t xyzm_transfer_event_tests[] = {
  TEST_CASE(test_transfer_events_sync_unit, "Synchronous X/Y/ZMODEM send and receive emit transfer lifecycle events."),
};

const size_t xyzm_transfer_event_tests_count =
    sizeof(xyzm_transfer_event_tests) / sizeof(xyzm_transfer_event_tests[0]);
