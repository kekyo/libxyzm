/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

static xyzm_status_t failing_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  (void)opaque;
  (void)file_opaque;
  (void)info;
  (void)result;
  return XYZM_STATUS_IO_ERROR;
}

static xyzm_status_t failing_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  (void)opaque;
  (void)file_opaque;
  (void)info;
  (void)result;
  return XYZM_STATUS_IO_ERROR;
}

static void test_crc_helpers(void) {
  static const uint8_t sample[] = "123456789";
  static const uint8_t single[] = "a";
  static const uint8_t short_sample[] = "abc";
  uint8_t checksum;
  uint16_t crc16;
  uint32_t crc32;

  ASSERT_EQ_U8(xyzm_checksum8_update_buffer(0u, sample, sizeof(sample) - 1u), 0xdd);
  ASSERT_TRUE(xyzm_crc16_xmodem_update_buffer(0u, sample, sizeof(sample) - 1u) == 0x31c3u);
  ASSERT_TRUE((uint32_t)~xyzm_crc32_zmodem_update_buffer(0xffffffffu, sample, 0u) == 0x00000000u);
  ASSERT_TRUE((uint32_t)~xyzm_crc32_zmodem_update_buffer(0xffffffffu, single, sizeof(single) - 1u) == 0xe8b7be43u);
  ASSERT_TRUE((uint32_t)~xyzm_crc32_zmodem_update_buffer(0xffffffffu, short_sample, sizeof(short_sample) - 1u) == 0x352441c2u);
  ASSERT_TRUE((uint32_t)~xyzm_crc32_zmodem_update_buffer(0xffffffffu, sample, sizeof(sample) - 1u) == 0xcbf43926u);

  checksum = 0u;
  checksum = xyzm_checksum8_update_buffer(checksum, sample, 3u);
  checksum = xyzm_checksum8_update_buffer(checksum, sample + 3u, (sizeof(sample) - 1u) - 3u);
  ASSERT_EQ_U8(checksum, 0xdd);

  crc16 = 0u;
  crc16 = xyzm_crc16_xmodem_update_buffer(crc16, sample, 3u);
  crc16 = xyzm_crc16_xmodem_update_buffer(crc16, sample + 3u, (sizeof(sample) - 1u) - 3u);
  ASSERT_TRUE(crc16 == 0x31c3u);

  crc32 = 0xffffffffu;
  crc32 = xyzm_crc32_zmodem_update_buffer(crc32, sample, 3u);
  crc32 = xyzm_crc32_zmodem_update_buffer(crc32, sample + 3u, (sizeof(sample) - 1u) - 3u);
  ASSERT_TRUE((uint32_t)~crc32 == 0xcbf43926u);
}

static void test_protocol_send_speed(void) {
  static const size_t data_len = 262144u;
  struct speed_case {
    const char *name;
    test_sync_protocol_t protocol;
    uint64_t min_bps;
    uint64_t max_bps;
  } cases[] = {
      {"xmodem", TEST_SYNC_PROTOCOL_XMODEM, 10000u, 14000u},
      {"xmodem-crc", TEST_SYNC_PROTOCOL_XMODEM_CRC, 10000u, 14000u},
      {"xmodem-1k", TEST_SYNC_PROTOCOL_XMODEM_1K, 45000u, 55000u},
      {"ymodem", TEST_SYNC_PROTOCOL_YMODEM, 45000u, 55000u},
      {"ymodem-g", TEST_SYNC_PROTOCOL_YMODEM_G, 90000u, 110000u},
      {"zmodem", TEST_SYNC_PROTOCOL_ZMODEM, 70000u, 110000u},
  };
  uint64_t observed_bps[sizeof(cases) / sizeof(cases[0])];
  uint8_t *data;
  size_t case_index;

  data = malloc(data_len);
  ASSERT_TRUE(data != NULL);
  memset(data, 'A', data_len);

  for (case_index = 0u; case_index < (sizeof(cases) / sizeof(cases[0])); ++case_index) {
    timed_link_t link;
    memory_source_t source;
    xyzm_link_ops_t link_ops;
    xyzm_source_ops_t source_ops;
    test_sync_transfer_opts_t options;
    test_sync_send_request_t request;
    xyzm_transfer_report_t report;
    xyzm_status_t status;
    uint8_t incoming[4096];
    uint8_t *outgoing;
    size_t incoming_len;
    size_t block_count;

    memset(&link, 0, sizeof(link));
    memset(&source, 0, sizeof(source));
    incoming_len = 0u;
    block_count = 0u;
    outgoing = malloc(data_len * 2u);
    ASSERT_TRUE(outgoing != NULL);

    switch (cases[case_index].protocol) {
      case TEST_SYNC_PROTOCOL_XMODEM:
        block_count = data_len / 128u;
        fill_xmodem_speed_incoming(incoming, &incoming_len, XYZM_NAK, block_count);
        source.info.valid_mask = XYZM_FILE_INFO_SIZE;
        break;
      case TEST_SYNC_PROTOCOL_XMODEM_CRC:
        block_count = data_len / 128u;
        fill_xmodem_speed_incoming(incoming, &incoming_len, XYZM_CRC_REQ, block_count);
        source.info.valid_mask = XYZM_FILE_INFO_SIZE;
        break;
      case TEST_SYNC_PROTOCOL_XMODEM_1K:
        block_count = data_len / 1024u;
        fill_xmodem_speed_incoming(incoming, &incoming_len, XYZM_NAK, block_count);
        source.info.valid_mask = XYZM_FILE_INFO_SIZE;
        break;
      case TEST_SYNC_PROTOCOL_YMODEM:
        block_count = data_len / 1024u;
        fill_ymodem_speed_incoming(incoming, &incoming_len, false, block_count);
        source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
        source.info.name = "speed.bin";
        break;
      case TEST_SYNC_PROTOCOL_YMODEM_G:
        block_count = data_len / 1024u;
        fill_ymodem_speed_incoming(incoming, &incoming_len, true, block_count);
        source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
        source.info.name = "speed.bin";
        break;
      case TEST_SYNC_PROTOCOL_ZMODEM:
        fill_zmodem_speed_incoming(incoming, &incoming_len);
        source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
        source.info.name = "speed.bin";
        break;
      default:
        fail_now(__FILE__, __LINE__, "unexpected protocol in speed test");
    }

    source.info.size_bytes = data_len;
    source.data = data;
    source.data_len = data_len;
    link.incoming = incoming;
    link.incoming_len = incoming_len;
    link.outgoing = outgoing;
    link.outgoing_capacity = data_len * 2u;
    link.send_base_us = 0u;
    link.send_byte_us = 10u;
    link.recv_base_us = 10000u;
    link.recv_byte_us = 0u;

    link_ops.opaque = &link;
    link_ops.send = timed_send;
    link_ops.recv = timed_recv;
    link_ops.now_ms = timed_now_ms;

    source_ops.opaque = &source;
    source_ops.next = memory_source_next;
    source_ops.read = memory_source_read;
    source_ops.end = memory_source_end;

    populate_sync_default_options(&options, cases[case_index].protocol);
    if (cases[case_index].protocol == TEST_SYNC_PROTOCOL_ZMODEM) {
      options.zmodem_use_crc32 = 0u;
      options.zmodem_escape_ctrl = 0u;
      options.zmodem_packet_len = 1024u;
    }
    request.link = &link_ops;
    request.source = &source_ops;
    request.options = &options;

    status = test_sync_send(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
    ASSERT_TRUE(report.payload_bytes == data_len);
    observed_bps[case_index] = bytes_per_second(report.payload_bytes, link.now_us);
    if ((observed_bps[case_index] < cases[case_index].min_bps) ||
        (observed_bps[case_index] > cases[case_index].max_bps)) {
      fail_now(
          __FILE__,
          __LINE__,
          "%s speed out of range: bps=%llu send_calls=%u recv_calls=%u elapsed_us=%llu",
          cases[case_index].name,
          (unsigned long long)observed_bps[case_index],
          link.send_calls,
          link.recv_calls,
          (unsigned long long)link.now_us);
    }
    if ((cases[case_index].protocol == TEST_SYNC_PROTOCOL_ZMODEM) &&
        (link.send_calls > 4096u)) {
      fail_now(
          __FILE__,
          __LINE__,
          "%s ZMODEM send calls too high: send_calls=%u",
          cases[case_index].name,
          link.send_calls);
    }
    free(outgoing);
  }

  ASSERT_TRUE(observed_bps[2] > observed_bps[0]);
  ASSERT_TRUE(observed_bps[3] > observed_bps[1]);
  ASSERT_TRUE(observed_bps[4] > observed_bps[3]);
  ASSERT_TRUE(observed_bps[5] > observed_bps[3]);
  free(data);
}

static void test_protocol_send_speed_sync_unit(void) {
  test_protocol_send_speed();
}

static void test_api_argument_validation_unit(void) {
  fake_link_t link;
  memory_source_t source;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  xyzm_source_ops_t invalid_source_ops;
  xyzm_sink_ops_t sink_ops;
  xyzm_sink_ops_t invalid_sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t send_request;
  test_sync_receive_request_t receive_request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memset(&source, 0, sizeof(source));
  memset(&sink, 0, sizeof(sink));

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;
  invalid_source_ops = source_ops;
  invalid_source_ops.read = NULL;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;
  invalid_sink_ops = sink_ops;
  invalid_sink_ops.write = NULL;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);

  send_request.link = &link_ops;
  send_request.source = &source_ops;
  send_request.options = &options;
  receive_request.link = &link_ops;
  receive_request.sink = &sink_ops;
  receive_request.options = &options;

  status = test_sync_send(NULL, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);

  send_request.link = NULL;
  status = test_sync_send(&send_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);
  send_request.link = &link_ops;

  send_request.source = &invalid_source_ops;
  status = test_sync_send(&send_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);
  send_request.source = &source_ops;

  send_request.options = NULL;
  status = test_sync_send(&send_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);
  send_request.options = &options;

  status = test_sync_receive(NULL, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);

  receive_request.link = NULL;
  status = test_sync_receive(&receive_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);
  receive_request.link = &link_ops;

  receive_request.sink = &invalid_sink_ops;
  status = test_sync_receive(&receive_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);
  receive_request.sink = &sink_ops;

  receive_request.options = NULL;
  status = test_sync_receive(&receive_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_ARGUMENT_ERROR);
}

static void test_report_null_and_completion_callback_failure(void) {
  fake_link_t send_link;
  fake_link_t receive_link;
  memory_source_t source;
  memory_sink_t sink;
  xyzm_link_ops_t send_link_ops;
  xyzm_link_ops_t receive_link_ops;
  xyzm_source_ops_t source_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t send_options;
  test_sync_transfer_opts_t receive_options;
  test_sync_send_request_t send_request;
  test_sync_receive_request_t receive_request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t payload[128];
  size_t offset;

  memset(&send_link, 0, sizeof(send_link));
  memset(&receive_link, 0, sizeof(receive_link));
  memset(&source, 0, sizeof(source));
  memset(&sink, 0, sizeof(sink));
  memset(payload, 0x1au, sizeof(payload));
  memcpy(payload, "hello", 5u);

  send_link.incoming[0] = XYZM_NAK;
  send_link.incoming[1] = XYZM_ACK;
  send_link.incoming[2] = XYZM_NAK;
  send_link.incoming[3] = XYZM_ACK;
  send_link.incoming[4] = XYZM_NAK;
  send_link.incoming[5] = XYZM_ACK;
  send_link.incoming[6] = XYZM_NAK;
  send_link.incoming[7] = XYZM_ACK;
  send_link.incoming_len = 8u;

  source.data = (const uint8_t *)"abc";
  source.data_len = 3u;

  send_link_ops.opaque = &send_link;
  send_link_ops.send = fake_send;
  send_link_ops.recv = fake_recv;
  send_link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&send_options, TEST_SYNC_PROTOCOL_XMODEM);
  send_request.link = &send_link_ops;
  send_request.source = &source_ops;
  send_request.options = &send_options;

  status = test_sync_send(&send_request, NULL);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);

  source.offset = 0u;
  source.served = false;
  source.end_result = XYZM_STATUS_OK;
  source_ops.end = failing_source_end;
  status = test_sync_send(&send_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_IO_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_IO_ERROR);

  offset = 0u;
  append_xmodem_block(receive_link.incoming, &offset, 1u, payload, sizeof(payload), false, XYZM_SOH);
  receive_link.incoming[offset++] = XYZM_EOT;
  receive_link.incoming[offset++] = XYZM_EOT;
  receive_link.incoming_len = offset;

  receive_link_ops.opaque = &receive_link;
  receive_link_ops.send = fake_send;
  receive_link_ops.recv = fake_recv;
  receive_link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = failing_sink_end;

  populate_sync_default_options(&receive_options, TEST_SYNC_PROTOCOL_XMODEM);
  receive_request.link = &receive_link_ops;
  receive_request.sink = &sink_ops;
  receive_request.options = &receive_options;

  status = test_sync_receive(&receive_request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_IO_ERROR);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_IO_ERROR);
  ASSERT_TRUE(sink.began);
}

static void test_report_null_and_completion_callback_failure_sync_unit(void) {
  test_report_null_and_completion_callback_failure();
}

const test_case_t xyzm_core_tests[] = {
  TEST_CASE(test_crc_helpers, "CRC helper outputs match known vectors."),
  TEST_CASE(test_protocol_send_speed_sync_unit, "Synchronous protocol send speeds stay within deterministic expected bands."),
  TEST_CASE(test_api_argument_validation_unit, "Synchronous public entry points reject incomplete requests with argument errors."),
  TEST_CASE(test_report_null_and_completion_callback_failure_sync_unit, "Synchronous public entry points tolerate NULL reports and surface completion callback failures."),
};

const size_t xyzm_core_tests_count = sizeof(xyzm_core_tests) / sizeof(xyzm_core_tests[0]);
