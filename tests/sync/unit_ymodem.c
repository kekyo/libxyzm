/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

typedef struct batch_memory_file {
  xyzm_file_info_t info;
  const uint8_t *data;
  size_t data_len;
  size_t offset;
  xyzm_status_t end_result;
} batch_memory_file_t;

typedef struct batch_memory_source {
  batch_memory_file_t *files;
  size_t file_count;
  size_t next_index;
} batch_memory_source_t;

static size_t count_ymodem_terminal_header_frames(
    const uint8_t *data,
    size_t data_len) {
  uint8_t frame[3u + 128u + 2u];

  memset(frame, 0, sizeof(frame));
  frame[0] = XYZM_SOH;
  frame[1] = 0u;
  frame[2] = 0xffu;
  return count_bytes_pattern(data, data_len, frame, sizeof(frame));
}

static xyzm_status_t batch_memory_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  batch_memory_source_t *source = opaque;
  batch_memory_file_t *file;

  if (source->next_index >= source->file_count) {
    return XYZM_STATUS_END;
  }
  file = &source->files[source->next_index];
  source->next_index += 1u;
  file->offset = 0u;
  *info = file->info;
  *file_opaque = file;
  return XYZM_STATUS_OK;
}

static xyzm_status_t batch_memory_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  batch_memory_file_t *file = file_opaque;
  size_t available_len;
  size_t copy_len;

  (void)opaque;
  available_len = file->data_len - file->offset;
  if (available_len == 0u) {
    *read_len = 0u;
    return XYZM_STATUS_END;
  }
  copy_len = (available_len < capacity) ? available_len : capacity;
  memcpy(buf, file->data + file->offset, copy_len);
  file->offset += copy_len;
  *read_len = copy_len;
  return XYZM_STATUS_OK;
}

static xyzm_status_t batch_memory_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset) {
  batch_memory_file_t *file = file_opaque;

  (void)opaque;
  if (offset > file->data_len) {
    return XYZM_STATUS_IO_ERROR;
  }
  file->offset = (size_t)offset;
  return XYZM_STATUS_OK;
}

static xyzm_status_t batch_memory_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  batch_memory_file_t *file = file_opaque;

  (void)opaque;
  (void)info;
  file->end_result = result;
  return XYZM_STATUS_OK;
}

static void test_ymodem_empty_variants(void) {
  struct {
    test_sync_protocol_t protocol;
    const uint8_t *incoming;
    size_t incoming_len;
  } cases[] = {
      {TEST_SYNC_PROTOCOL_YMODEM,
          (const uint8_t[]){XYZM_CRC_REQ, XYZM_ACK, XYZM_CRC_REQ, XYZM_NAK, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK},
          7u},
      {TEST_SYNC_PROTOCOL_YMODEM_G,
          (const uint8_t[]){XYZM_G_REQ, XYZM_G_REQ, XYZM_ACK, XYZM_G_REQ},
          4u},
  };
  size_t case_index;

  for (case_index = 0u; case_index < (sizeof(cases) / sizeof(cases[0])); ++case_index) {
    fake_link_t link;
    memory_source_t source;
    xyzm_link_ops_t link_ops;
    xyzm_source_ops_t source_ops;
    test_sync_transfer_opts_t options;
    test_sync_send_request_t request;
    xyzm_transfer_report_t report;
    xyzm_status_t status;

    memset(&link, 0, sizeof(link));
    memcpy(link.incoming, cases[case_index].incoming, cases[case_index].incoming_len);
    link.incoming_len = cases[case_index].incoming_len;

    memset(&source, 0, sizeof(source));
    source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
    source.info.name = "empty.bin";
    source.info.size_bytes = 0u;
    source.data = (const uint8_t *)"";
    source.data_len = 0u;

    link_ops.opaque = &link;
    link_ops.send = fake_send;
    link_ops.recv = fake_recv;
    link_ops.now_ms = fake_now_ms;

    source_ops.opaque = &source;
    source_ops.next = memory_source_next;
    source_ops.read = memory_source_read;
    source_ops.end = memory_source_end;

    populate_sync_default_options(&options, cases[case_index].protocol);
    request.link = &link_ops;
    request.source = &source_ops;
    request.options = &options;

    status = test_sync_send(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
    ASSERT_TRUE(report.payload_bytes == 0u);
    ASSERT_TRUE(report.completed_files == 1u);
    ASSERT_TRUE(memchr(link.outgoing, XYZM_EOT, link.outgoing_len) != NULL);
    ASSERT_TRUE(memchr(link.outgoing, XYZM_STX, link.outgoing_len) == NULL);
  }
}

static void test_ymodem_receive_terminal_header(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_PROTOCOL_ERROR);
  ASSERT_TRUE(!sink.began);
}

static void test_ymodem_receive_size_mismatch(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  memset(payload, 0x1au, sizeof(payload));
  memcpy(metadata, "size.bin", 8u);
  memcpy(metadata + 9u, "1025", 4u);
  memset(payload, 'A', sizeof(payload));
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_PROTOCOL_ERROR);
  ASSERT_EQ_SIZE(sink.data_len, 1024u);
  ASSERT_TRUE(report.payload_bytes == 1024u);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_PROTOCOL_ERROR);
}

static void test_ymodem_receive_timeout_after_metadata(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "timeout.bin", 11u);
  memcpy(metadata + 12u, "1", 1u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_TIMEOUT);
  ASSERT_TRUE(sink.began);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_TIMEOUT);
}

static void test_ymodem_send(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];

  memset(&link, 0, sizeof(link));
  link.incoming[0] = XYZM_CRC_REQ;
  link.incoming[1] = XYZM_ACK;
  link.incoming[2] = XYZM_CRC_REQ;
  link.incoming[3] = XYZM_ACK;
  link.incoming[4] = XYZM_NAK;
  link.incoming[5] = XYZM_ACK;
  link.incoming[6] = XYZM_CRC_REQ;
  link.incoming[7] = XYZM_ACK;
  link.incoming_len = 8u;

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "demo.bin";
  source.info.size_bytes = 3u;
  source.data = (const uint8_t *)"abc";
  source.data_len = 3u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == 3u);
  ASSERT_TRUE(report.completed_files == 1u);

  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "demo.bin", 8u);
  memcpy(metadata + 9u, "3", 1u);
  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, "abc", 3u);

  ASSERT_TRUE(link.outgoing_len > (3u + 128u + 2u));
  ASSERT_EQ_U8(link.outgoing[0], XYZM_SOH);
  ASSERT_TRUE(memcmp(link.outgoing + 3u, metadata, sizeof(metadata)) == 0);
  ASSERT_TRUE(memchr(link.outgoing, XYZM_EOT, link.outgoing_len) != NULL);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_OK);
}

static void test_ymodem_send_terminal_header_ack_timeout(void) {
  static const uint8_t incoming[] = {
      XYZM_CRC_REQ, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK,
      XYZM_NAK, XYZM_ACK, XYZM_CRC_REQ,
  };
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memcpy(link.incoming, incoming, sizeof(incoming));
  link.incoming_len = sizeof(incoming);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "demo.bin";
  source.info.size_bytes = 3u;
  source.data = (const uint8_t *)"abc";
  source.data_len = 3u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == 3u);
  ASSERT_TRUE(report.completed_files == 1u);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(
      count_ymodem_terminal_header_frames(link.outgoing, link.outgoing_len),
      1u);
}

static void test_ymodem_receive(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "recv.bin", 8u);
  memcpy(metadata + 9u, "3", 1u);
  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, "xyz", 3u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  link.incoming[offset++] = XYZM_EOT;
  memset(metadata, 0, sizeof(metadata));
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, 3u);
  ASSERT_TRUE(memcmp(sink.data, "xyz", 3u) == 0);
  ASSERT_TRUE((sink.info.valid_mask & XYZM_FILE_INFO_NAME) != 0u);
  ASSERT_TRUE(strcmp(sink.info.name, "recv.bin") == 0);
}

static void test_ymodem_receive_ignores_request_echo(void) {
  struct {
    test_sync_protocol_t protocol;
    uint8_t echo_byte;
    const char *name;
    const uint8_t *data;
    size_t data_len;
  } cases[] = {
      {TEST_SYNC_PROTOCOL_YMODEM, XYZM_CRC_REQ, "echo-c.bin", (const uint8_t *)"abc", 3u},
      {TEST_SYNC_PROTOCOL_YMODEM_G, XYZM_G_REQ, "echo-g.bin", (const uint8_t *)"data", 4u},
  };
  size_t case_index;

  for (case_index = 0u; case_index < (sizeof(cases) / sizeof(cases[0])); ++case_index) {
    fake_link_t link;
    memory_sink_t sink;
    xyzm_link_ops_t link_ops;
    xyzm_sink_ops_t sink_ops;
    test_sync_transfer_opts_t options;
    test_sync_receive_request_t request;
    xyzm_transfer_report_t report;
    xyzm_status_t status;
    uint8_t metadata[128];
    uint8_t payload[1024];
    size_t offset;

    memset(&link, 0, sizeof(link));
    memset(&sink, 0, sizeof(sink));
    memset(metadata, 0, sizeof(metadata));
    memcpy(metadata, cases[case_index].name, strlen(cases[case_index].name));
    memcpy(
        metadata + strlen(cases[case_index].name) + 1u,
        cases[case_index].data_len == 3u ? "3" : "4",
        1u);
    memset(payload, 0x1au, sizeof(payload));
    memcpy(payload, cases[case_index].data, cases[case_index].data_len);
    offset = 0u;
    link.incoming[offset++] = cases[case_index].echo_byte;
    append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
    link.incoming[offset++] = cases[case_index].echo_byte;
    append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
    link.incoming[offset++] = XYZM_EOT;
    if (cases[case_index].protocol == TEST_SYNC_PROTOCOL_YMODEM) {
      link.incoming[offset++] = XYZM_EOT;
    }
    link.incoming[offset++] = cases[case_index].echo_byte;
    memset(metadata, 0, sizeof(metadata));
    append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
    link.incoming_len = offset;

    link_ops.opaque = &link;
    link_ops.send = fake_send;
    link_ops.recv = fake_recv;
    link_ops.now_ms = fake_now_ms;

    sink_ops.opaque = &sink;
    sink_ops.begin = memory_sink_begin;
    sink_ops.write = memory_sink_write;
    sink_ops.end = memory_sink_end;

    populate_sync_default_options(&options, cases[case_index].protocol);
    request.link = &link_ops;
    request.sink = &sink_ops;
    request.options = &options;

    status = test_sync_receive(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
    ASSERT_EQ_SIZE(sink.data_len, cases[case_index].data_len);
    ASSERT_TRUE(memcmp(sink.data, cases[case_index].data, cases[case_index].data_len) == 0);
  }
}

static void test_ymodem_receive_terminal_noise(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "recv.bin", 8u);
  memcpy(metadata + 9u, "3", 1u);
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "xyz", 3u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  link.incoming[offset++] = XYZM_EOT;
  link.incoming[offset++] = XYZM_CRC_REQ;
  link.incoming[offset++] = '\r';
  link.incoming[offset++] = '\n';
  memset(metadata, 0, sizeof(metadata));
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, 3u);
  ASSERT_TRUE(memcmp(sink.data, "xyz", 3u) == 0);
  ASSERT_TRUE(report.retry_count >= 1u);
}

static void test_ymodem_g_send(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  link.incoming[0] = XYZM_G_REQ;
  link.incoming[1] = XYZM_G_REQ;
  link.incoming[2] = XYZM_ACK;
  link.incoming[3] = XYZM_G_REQ;
  link.incoming_len = 4u;

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "stream.bin";
  source.info.size_bytes = 5u;
  source.data = (const uint8_t *)"12345";
  source.data_len = 5u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM_G);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(memchr(link.outgoing, XYZM_EOT, link.outgoing_len) != NULL);
}

static void test_ymodem_auto_send(void) {
  struct {
    const uint8_t *incoming;
    size_t incoming_len;
    bool streaming;
  } cases[] = {
      {
          (const uint8_t[]){XYZM_CRC_REQ, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK, XYZM_NAK, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK},
          8u,
          false},
      {
          (const uint8_t[]){XYZM_G_REQ, XYZM_G_REQ, XYZM_ACK, XYZM_G_REQ},
          4u,
          true},
  };
  size_t case_index;

  for (case_index = 0u; case_index < (sizeof(cases) / sizeof(cases[0])); ++case_index) {
    fake_link_t link;
    memory_source_t source;
    xyzm_link_ops_t link_ops;
    xyzm_source_ops_t source_ops;
    test_sync_transfer_opts_t options;
    test_sync_send_request_t request;
    xyzm_transfer_report_t report;
    xyzm_status_t status;

    memset(&link, 0, sizeof(link));
    memcpy(link.incoming, cases[case_index].incoming, cases[case_index].incoming_len);
    link.incoming_len = cases[case_index].incoming_len;

    memset(&source, 0, sizeof(source));
    source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
    source.info.name = cases[case_index].streaming ? "auto-g.bin" : "auto.bin";
    source.info.size_bytes = 4u;
    source.data = (const uint8_t *)"auto";
    source.data_len = 4u;

    link_ops.opaque = &link;
    link_ops.send = fake_send;
    link_ops.recv = fake_recv;
    link_ops.now_ms = fake_now_ms;

    source_ops.opaque = &source;
    source_ops.next = memory_source_next;
    source_ops.read = memory_source_read;
    source_ops.end = memory_source_end;

    populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM_AUTO);
    request.link = &link_ops;
    request.source = &source_ops;
    request.options = &options;

    status = test_sync_send(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
    ASSERT_TRUE(report.payload_bytes == 4u);
    ASSERT_TRUE(report.completed_files == 1u);
    ASSERT_TRUE(memchr(link.outgoing, XYZM_EOT, link.outgoing_len) != NULL);
  }
}

static void test_ymodem_g_receive(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "g.bin", 5u);
  memcpy(metadata + 6u, "4", 1u);
  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, "data", 4u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  memset(metadata, 0, sizeof(metadata));
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM_G);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, 4u);
  ASSERT_TRUE(memcmp(sink.data, "data", 4u) == 0);
}

static void test_ymodem_g_receive_missing_terminal_header(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "lost-terminal.bin", 17u);
  memcpy(metadata + 18u, "4", 1u);
  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, "data", 4u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM_G);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, 4u);
  ASSERT_TRUE(memcmp(sink.data, "data", 4u) == 0);
  ASSERT_TRUE(report.completed_files == 1u);
}

static void test_ymodem_auto_receive_rejected(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_YMODEM_AUTO);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_UNSUPPORTED);
  ASSERT_TRUE(!sink.began);
}

static void test_ymodem_send_batch_sync_unit(void) {
  static const uint8_t first_data[] = {'A', 'B', 'C'};
  static const uint8_t second_data[] = {'D', 'E'};
  static const uint8_t incoming[] = {
      XYZM_CRC_REQ, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK,
      XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK, XYZM_CRC_REQ,
      XYZM_ACK, XYZM_ACK, XYZM_CRC_REQ, XYZM_ACK,
  };
  fake_link_t link;
  batch_memory_file_t files[2];
  batch_memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  xyzm_ymodem_opts_t options;
  xyzm_ymodem_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memcpy(link.incoming, incoming, sizeof(incoming));
  link.incoming_len = sizeof(incoming);
  memset(files, 0, sizeof(files));
  files[0].info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  files[0].info.name = "first.bin";
  files[0].info.size_bytes = sizeof(first_data);
  files[0].data = first_data;
  files[0].data_len = sizeof(first_data);
  files[1].info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  files[1].info.name = "second.bin";
  files[1].info.size_bytes = sizeof(second_data);
  files[1].data = second_data;
  files[1].data_len = sizeof(second_data);
  source.files = files;
  source.file_count = 2u;
  source.next_index = 0u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;
  source_ops.opaque = &source;
  source_ops.next = batch_memory_source_next;
  source_ops.read = batch_memory_source_read;
  source_ops.seek = batch_memory_source_seek;
  source_ops.end = batch_memory_source_end;
  options.handshake_timeout_ms = 1000u;
  options.block_timeout_ms = 1000u;
  options.retry_limit = 10u;
  options.pad_byte = 0x1au;
  options.variant = XYZM_YMODEM_VARIANT_STANDARD;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;
  request.observer = NULL;

  status = xyzm_ymodem_send_batch(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.completed_files == 2u);
  ASSERT_TRUE(report.payload_bytes == sizeof(first_data) + sizeof(second_data));
  ASSERT_EQ_STATUS(files[0].end_result, XYZM_STATUS_OK);
  ASSERT_EQ_STATUS(files[1].end_result, XYZM_STATUS_OK);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, (const uint8_t *)"first.bin", 9u) < link.outgoing_len);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, (const uint8_t *)"second.bin", 10u) < link.outgoing_len);
}

static void test_ymodem_receive_batch_sync_unit(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  xyzm_ymodem_opts_t options;
  xyzm_ymodem_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t metadata[128];
  uint8_t payload[1024];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  offset = 0u;
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "first.bin", 9u);
  memcpy(metadata + 10u, "3", 1u);
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "ABC", 3u);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  link.incoming[offset++] = XYZM_EOT;
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "second.bin", 10u);
  memcpy(metadata + 11u, "2", 1u);
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "DE", 2u);
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
  link.incoming[offset++] = XYZM_EOT;
  memset(metadata, 0, sizeof(metadata));
  append_xmodem_block(link.incoming, &offset, 0u, metadata, sizeof(metadata), true, XYZM_SOH);
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;
  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;
  options.handshake_timeout_ms = 1000u;
  options.block_timeout_ms = 1000u;
  options.retry_limit = 10u;
  options.pad_byte = 0x1au;
  options.variant = XYZM_YMODEM_VARIANT_STANDARD;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;
  request.observer = NULL;

  status = xyzm_ymodem_receive_batch(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.completed_files == 2u);
  ASSERT_TRUE(report.payload_bytes == 5u);
  ASSERT_EQ_SIZE(sink.data_len, 5u);
  ASSERT_TRUE(memcmp(sink.data, "ABCDE", 5u) == 0);
}

static void test_ymodem_empty_variants_sync_unit(void) {
  test_ymodem_empty_variants();
}

static void test_ymodem_receive_terminal_header_sync_unit(void) {
  test_ymodem_receive_terminal_header();
}

static void test_ymodem_receive_size_mismatch_sync_unit(void) {
  test_ymodem_receive_size_mismatch();
}

static void test_ymodem_receive_timeout_after_metadata_sync_unit(void) {
  test_ymodem_receive_timeout_after_metadata();
}

static void test_ymodem_send_sync_unit(void) {
  test_ymodem_send();
}

static void test_ymodem_send_terminal_header_ack_timeout_sync_unit(void) {
  test_ymodem_send_terminal_header_ack_timeout();
}

static void test_ymodem_receive_sync_unit(void) {
  test_ymodem_receive();
}

static void test_ymodem_receive_request_echo_sync_unit(void) {
  test_ymodem_receive_ignores_request_echo();
}

static void test_ymodem_receive_terminal_noise_sync_unit(void) {
  test_ymodem_receive_terminal_noise();
}

static void test_ymodem_g_send_sync_unit(void) {
  test_ymodem_g_send();
}

static void test_ymodem_auto_send_sync_unit(void) {
  test_ymodem_auto_send();
}

static void test_ymodem_g_receive_sync_unit(void) {
  test_ymodem_g_receive();
}

static void test_ymodem_g_receive_missing_terminal_header_sync_unit(void) {
  test_ymodem_g_receive_missing_terminal_header();
}

static void test_ymodem_auto_receive_rejected_sync_unit(void) {
  test_ymodem_auto_receive_rejected();
}

const test_case_t xyzm_ymodem_unit_tests[] = {
  TEST_CASE(test_ymodem_empty_variants_sync_unit, "Synchronous YMODEM and YMODEM-g handle empty files without data blocks."),
  TEST_CASE(test_ymodem_receive_terminal_header_sync_unit, "Synchronous YMODEM receive rejects a terminal header as the first metadata block."),
  TEST_CASE(test_ymodem_receive_size_mismatch_sync_unit, "Synchronous YMODEM receive reports protocol error when EOF arrives before the declared size."),
  TEST_CASE(test_ymodem_receive_timeout_after_metadata_sync_unit, "Synchronous YMODEM receive times out after metadata when payload never arrives."),
  TEST_CASE(test_ymodem_send_sync_unit, "Synchronous YMODEM send emits metadata and a padded data block."),
  TEST_CASE(test_ymodem_send_terminal_header_ack_timeout_sync_unit, "Synchronous YMODEM send does not retry the terminal header after final ACK loss."),
  TEST_CASE(test_ymodem_receive_sync_unit, "Synchronous YMODEM receive parses metadata and trims data by declared size."),
  TEST_CASE(test_ymodem_receive_request_echo_sync_unit, "Synchronous YMODEM receive ignores echoed receiver request bytes before metadata."),
  TEST_CASE(test_ymodem_receive_terminal_noise_sync_unit, "Synchronous YMODEM receive tolerates terminal-header noise before the null pathname block."),
  TEST_CASE(test_ymodem_g_send_sync_unit, "Synchronous YMODEM-g send streams without waiting for per-block ACK."),
  TEST_CASE(test_ymodem_auto_send_sync_unit, "Synchronous YMODEM auto send resolves either classic or streaming receiver requests."),
  TEST_CASE(test_ymodem_g_receive_sync_unit, "Synchronous YMODEM-g receive accepts the streaming payload path."),
  TEST_CASE(test_ymodem_g_receive_missing_terminal_header_sync_unit, "Synchronous YMODEM-g receive tolerates lrzsz terminal header loss after EOT."),
  TEST_CASE(test_ymodem_auto_receive_rejected_sync_unit, "Synchronous YMODEM receive rejects the auto send-only variant."),
  TEST_CASE(test_ymodem_send_batch_sync_unit, "Synchronous YMODEM batch send enumerates multiple source files."),
  TEST_CASE(test_ymodem_receive_batch_sync_unit, "Synchronous YMODEM batch receive accepts multiple metadata entries before the terminal header."),
};

const size_t xyzm_ymodem_unit_tests_count = sizeof(xyzm_ymodem_unit_tests) / sizeof(xyzm_ymodem_unit_tests[0]);
