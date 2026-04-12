/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

static void test_xmodem_1k_block_boundary(void) {
  struct {
    size_t data_len;
    size_t expected_frames;
    uint8_t first_header;
    uint8_t second_header;
  } cases[] = {
      {128u, 1u, XYZM_SOH, 0u},
      {129u, 1u, XYZM_STX, 0u},
      {1024u, 1u, XYZM_STX, 0u},
      {1025u, 2u, XYZM_STX, XYZM_SOH},
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
    uint8_t data[1025];
    size_t index;

    memset(&link, 0, sizeof(link));
    link.incoming[0] = XYZM_NAK;
    for (index = 0u; index < cases[case_index].expected_frames; ++index) {
      link.incoming[1u + index] = XYZM_ACK;
    }
    link.incoming[1u + cases[case_index].expected_frames] = XYZM_NAK;
    link.incoming[2u + cases[case_index].expected_frames] = XYZM_ACK;
    link.incoming_len = 3u + cases[case_index].expected_frames;

    for (index = 0u; index < sizeof(data); ++index) {
      data[index] = (uint8_t)(index ^ 0x5au);
    }

    memset(&source, 0, sizeof(source));
    source.info.valid_mask = XYZM_FILE_INFO_SIZE;
    source.info.size_bytes = cases[case_index].data_len;
    source.data = data;
    source.data_len = cases[case_index].data_len;

    link_ops.opaque = &link;
    link_ops.send = fake_send;
    link_ops.recv = fake_recv;
    link_ops.now_ms = fake_now_ms;

    source_ops.opaque = &source;
    source_ops.next = memory_source_next;
    source_ops.read = memory_source_read;
    source_ops.end = memory_source_end;

    populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K);
    request.link = &link_ops;
    request.source = &source_ops;
    request.options = &options;

    status = test_sync_send(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
    ASSERT_TRUE(report.payload_bytes == cases[case_index].data_len);
    ASSERT_EQ_U8(link.outgoing[0], cases[case_index].first_header);
    if (cases[case_index].expected_frames > 1u) {
      ASSERT_EQ_U8(link.outgoing[1028], cases[case_index].second_header);
    }
  }
}

static void test_xmodem_send_empty_variants(void) {
  struct {
    test_sync_protocol_t protocol;
    uint8_t start_char;
  } cases[] = {
      {TEST_SYNC_PROTOCOL_XMODEM, XYZM_NAK},
      {TEST_SYNC_PROTOCOL_XMODEM_CRC, XYZM_CRC_REQ},
      {TEST_SYNC_PROTOCOL_XMODEM_1K, XYZM_NAK},
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
    link.incoming[0] = cases[case_index].start_char;
    link.incoming[1] = XYZM_NAK;
    link.incoming[2] = XYZM_ACK;
    link.incoming_len = 3u;

    memset(&source, 0, sizeof(source));
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
    ASSERT_EQ_SIZE(link.outgoing_len, 2u);
    ASSERT_EQ_U8(link.outgoing[0], XYZM_EOT);
    ASSERT_EQ_U8(link.outgoing[1], XYZM_EOT);
    ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_OK);
  }
}

static void test_xmodem_send_timeout(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memset(&source, 0, sizeof(source));
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

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  options.retry_limit = 2u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_TIMEOUT);
  ASSERT_TRUE(report.retry_count == 3u);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_TIMEOUT);
}

static void test_xmodem_receive_empty_transfer(void) {
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
  link.incoming[0] = XYZM_EOT;
  link.incoming[1] = XYZM_EOT;
  link.incoming_len = 2u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(sink.began);
  ASSERT_TRUE(report.payload_bytes == 0u);
  ASSERT_TRUE(report.completed_files == 0u);
  ASSERT_EQ_SIZE(sink.data_len, 0u);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(link.outgoing_len, 3u);
  ASSERT_EQ_U8(link.outgoing[0], XYZM_NAK);
  ASSERT_EQ_U8(link.outgoing[1], XYZM_NAK);
  ASSERT_EQ_U8(link.outgoing[2], XYZM_ACK);
}

static void test_xmodem_receive_bad_block_header(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t payload[128];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "hello", 5u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), false, XYZM_SOH);
  link.incoming[2] ^= 0x01u;
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), false, XYZM_SOH);
  link.incoming[offset - (1u + sizeof(payload) + 2u)] ^= 0x01u;
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  options.retry_limit = 1u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_PROTOCOL_ERROR);
  ASSERT_TRUE(report.retry_count == 1u);
  ASSERT_EQ_SIZE(sink.data_len, 0u);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_PROTOCOL_ERROR);
}

static void test_xmodem_send(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t expected[256];
  uint8_t payload[128];
  size_t expected_len;
  static const uint8_t file_data[] = "hello";

  memset(&link, 0, sizeof(link));
  link.incoming[0] = XYZM_NAK;
  link.incoming[1] = XYZM_ACK;
  link.incoming[2] = XYZM_NAK;
  link.incoming[3] = XYZM_ACK;
  link.incoming_len = 4u;

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = 0u;
  source.data = file_data;
  source.data_len = sizeof(file_data) - 1u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == (sizeof(file_data) - 1u));
  ASSERT_TRUE(report.completed_files == 1u);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_OK);

  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, file_data, sizeof(file_data) - 1u);
  expected_len = 0u;
  append_xmodem_block(expected, &expected_len, 1u, payload, sizeof(payload), false, XYZM_SOH);
  expected[expected_len++] = XYZM_EOT;
  expected[expected_len++] = XYZM_EOT;

  ASSERT_EQ_SIZE(link.outgoing_len, expected_len);
  ASSERT_TRUE(memcmp(link.outgoing, expected, expected_len) == 0);
}

static void test_xmodem_receive_retries_bad_checksum(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t payload[128];
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, "hello", 5u);
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), false, XYZM_SOH);
  link.incoming[offset - 1u] ^= 0xffu;
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), false, XYZM_SOH);
  link.incoming[offset++] = XYZM_EOT;
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

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.retry_count >= 1u);
  ASSERT_EQ_SIZE(sink.data_len, 128u);
  ASSERT_TRUE(memcmp(sink.data, payload, 128u) == 0);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_OK);
}

static void test_xmodem_crc_send(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t expected[256];
  uint8_t payload[128];
  size_t expected_len;
  static const uint8_t file_data[] = "crc";

  memset(&link, 0, sizeof(link));
  link.incoming[0] = XYZM_CRC_REQ;
  link.incoming[1] = XYZM_ACK;
  link.incoming[2] = XYZM_NAK;
  link.incoming[3] = XYZM_ACK;
  link.incoming_len = 4u;

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = 0u;
  source.data = file_data;
  source.data_len = sizeof(file_data) - 1u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_CRC);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);

  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, file_data, sizeof(file_data) - 1u);
  expected_len = 0u;
  append_xmodem_block(expected, &expected_len, 1u, payload, sizeof(payload), true, XYZM_SOH);
  expected[expected_len++] = XYZM_EOT;
  expected[expected_len++] = XYZM_EOT;

  ASSERT_EQ_SIZE(link.outgoing_len, expected_len);
  ASSERT_TRUE(memcmp(link.outgoing, expected, expected_len) == 0);
}

static void test_xmodem_auto_send(void) {
  struct {
    uint8_t start_char;
    bool use_crc;
  } cases[] = {
      {XYZM_NAK, false},
      {XYZM_CRC_REQ, true},
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
    uint8_t expected[256];
    uint8_t payload[128];
    size_t expected_len;
    static const uint8_t file_data[] = "auto";

    memset(&link, 0, sizeof(link));
    link.incoming[0] = cases[case_index].start_char;
    link.incoming[1] = XYZM_ACK;
    link.incoming[2] = XYZM_NAK;
    link.incoming[3] = XYZM_ACK;
    link.incoming_len = 4u;

    memset(&source, 0, sizeof(source));
    source.info.valid_mask = 0u;
    source.data = file_data;
    source.data_len = sizeof(file_data) - 1u;

    link_ops.opaque = &link;
    link_ops.send = fake_send;
    link_ops.recv = fake_recv;
    link_ops.now_ms = fake_now_ms;

    source_ops.opaque = &source;
    source_ops.next = memory_source_next;
    source_ops.read = memory_source_read;
    source_ops.end = memory_source_end;

    populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_AUTO);
    request.link = &link_ops;
    request.source = &source_ops;
    request.options = &options;

    status = test_sync_send(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);

    memset(payload, 0x1a, sizeof(payload));
    memcpy(payload, file_data, sizeof(file_data) - 1u);
    expected_len = 0u;
    append_xmodem_block(
        expected,
        &expected_len,
        1u,
        payload,
        sizeof(payload),
        cases[case_index].use_crc,
        XYZM_SOH);
    expected[expected_len++] = XYZM_EOT;
    expected[expected_len++] = XYZM_EOT;

    ASSERT_EQ_SIZE(link.outgoing_len, expected_len);
    ASSERT_TRUE(memcmp(link.outgoing, expected, expected_len) == 0);
  }
}

static void test_xmodem_1k_send(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t payload[1024];
  uint8_t data[300];
  size_t index;

  memset(&link, 0, sizeof(link));
  link.incoming[0] = XYZM_NAK;
  link.incoming[1] = XYZM_ACK;
  link.incoming[2] = XYZM_NAK;
  link.incoming[3] = XYZM_ACK;
  link.incoming_len = 4u;

  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index + 1u);
  }

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_SIZE;
  source.info.size_bytes = sizeof(data);
  source.data = data;
  source.data_len = sizeof(data);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);

  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, data, sizeof(data));
  ASSERT_EQ_U8(link.outgoing[0], XYZM_STX);
  ASSERT_EQ_U8(link.outgoing[1], 1u);
  ASSERT_EQ_U8(link.outgoing[2], 0xfeu);
  ASSERT_TRUE(memcmp(link.outgoing + 3u, payload, sizeof(payload)) == 0);
}

static void test_xmodem_1k_crc_send(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t expected[1032];
  uint8_t payload[1024];
  size_t expected_len;
  uint8_t data[300];
  size_t index;

  memset(&link, 0, sizeof(link));
  link.incoming[0] = XYZM_CRC_REQ;
  link.incoming[1] = XYZM_ACK;
  link.incoming[2] = XYZM_NAK;
  link.incoming[3] = XYZM_ACK;
  link.incoming_len = 4u;

  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index + 11u);
  }

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_SIZE;
  source.info.size_bytes = sizeof(data);
  source.data = data;
  source.data_len = sizeof(data);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K_CRC);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);

  memset(payload, 0x1a, sizeof(payload));
  memcpy(payload, data, sizeof(data));
  expected_len = 0u;
  append_xmodem_block(expected, &expected_len, 1u, payload, sizeof(payload), true, XYZM_STX);
  expected[expected_len++] = XYZM_EOT;
  expected[expected_len++] = XYZM_EOT;

  ASSERT_EQ_SIZE(link.outgoing_len, expected_len);
  ASSERT_TRUE(memcmp(link.outgoing, expected, expected_len) == 0);
}

static void test_xmodem_1k_crc_receive(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t payload[1024];
  size_t offset;
  size_t index;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  for (index = 0u; index < sizeof(payload); ++index) {
    payload[index] = (uint8_t)(index ^ 0x3cu);
  }
  offset = 0u;
  append_xmodem_block(link.incoming, &offset, 1u, payload, sizeof(payload), true, XYZM_STX);
  link.incoming[offset++] = XYZM_EOT;
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

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K_CRC);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == sizeof(payload));
  ASSERT_EQ_SIZE(sink.data_len, sizeof(payload));
  ASSERT_TRUE(memcmp(sink.data, payload, sizeof(payload)) == 0);
}

static void test_xmodem_1k_block_boundary_sync_unit(void) {
  test_xmodem_1k_block_boundary();
}

static void test_xmodem_send_empty_variants_sync_unit(void) {
  test_xmodem_send_empty_variants();
}

static void test_xmodem_send_timeout_sync_unit(void) {
  test_xmodem_send_timeout();
}

static void test_xmodem_receive_empty_transfer_sync_unit(void) {
  test_xmodem_receive_empty_transfer();
}

static void test_xmodem_receive_bad_block_header_sync_unit(void) {
  test_xmodem_receive_bad_block_header();
}

static void test_xmodem_send_sync_unit(void) {
  test_xmodem_send();
}

static void test_xmodem_receive_retries_bad_checksum_sync_unit(void) {
  test_xmodem_receive_retries_bad_checksum();
}

static void test_xmodem_crc_send_sync_unit(void) {
  test_xmodem_crc_send();
}

static void test_xmodem_auto_send_sync_unit(void) {
  test_xmodem_auto_send();
}

static void test_xmodem_1k_send_sync_unit(void) {
  test_xmodem_1k_send();
}

static void test_xmodem_1k_crc_send_sync_unit(void) {
  test_xmodem_1k_crc_send();
}

static void test_xmodem_1k_crc_receive_sync_unit(void) {
  test_xmodem_1k_crc_receive();
}

const test_case_t xyzm_xmodem_unit_tests[] = {
  TEST_CASE(test_xmodem_1k_block_boundary_sync_unit, "Synchronous XMODEM-1K switches correctly between 128-byte and 1KiB blocks."),
  TEST_CASE(test_xmodem_send_empty_variants_sync_unit, "Synchronous XMODEM send variants complete empty transfers with only EOT sequence."),
  TEST_CASE(test_xmodem_send_timeout_sync_unit, "Synchronous XMODEM send times out after the retry limit when start is missing."),
  TEST_CASE(test_xmodem_receive_empty_transfer_sync_unit, "Synchronous XMODEM receive accepts an immediate EOT handshake as an empty transfer."),
  TEST_CASE(test_xmodem_receive_bad_block_header_sync_unit, "Synchronous XMODEM receive rejects bad block number complement pairs."),
  TEST_CASE(test_xmodem_send_sync_unit, "Synchronous XMODEM checksum mode emits the expected padded frame."),
  TEST_CASE(test_xmodem_receive_retries_bad_checksum_sync_unit, "Synchronous XMODEM receive retries a corrupted checksum block and completes."),
  TEST_CASE(test_xmodem_crc_send_sync_unit, "Synchronous XMODEM-CRC emits a CRC protected 128-byte frame."),
  TEST_CASE(test_xmodem_auto_send_sync_unit, "Synchronous XMODEM auto mode accepts checksum and CRC startup bytes."),
  TEST_CASE(test_xmodem_1k_send_sync_unit, "Synchronous XMODEM-1K emits a 1KiB frame for medium payloads."),
  TEST_CASE(test_xmodem_1k_crc_send_sync_unit, "Synchronous XMODEM-1K with CRC emits a CRC-protected STX frame."),
  TEST_CASE(test_xmodem_1k_crc_receive_sync_unit, "Synchronous XMODEM receive accepts CRC-protected 1KiB frames."),
};

const size_t xyzm_xmodem_unit_tests_count = sizeof(xyzm_xmodem_unit_tests) / sizeof(xyzm_xmodem_unit_tests[0]);
