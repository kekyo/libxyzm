/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

static void test_xmodem_send_integration(void) {
  char temp_dir[256];
  char input_path[512];
  char output_path[512];
  uint8_t data[200];
  uint8_t received[256];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  file_source_t source_state;
  xyzm_source_ops_t source_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/rx", "-X", "-b", "-q", "-y", "received.bin", NULL};
  size_t read_len;

  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-xsend");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  ASSERT_TRUE(snprintf(output_path, sizeof(output_path), "%s/received.bin", temp_dir) > 0);
  for (read_len = 0u; read_len < sizeof(data); ++read_len) {
    data[read_len] = (uint8_t)read_len;
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&source_state, 0, sizeof(source_state));
  source_state.path = input_path;
  source_state.name = "input.bin";
  source_ops.opaque = &source_state;
  source_ops.next = file_source_next;
  source_ops.read = file_source_read;
  source_ops.end = file_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(output_path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, 256u);
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  for (read_len = sizeof(data); read_len < 256u; ++read_len) {
    ASSERT_EQ_U8(received[read_len], 0x1au);
  }
  remove_tree(temp_dir);
}

static void test_xmodem_receive_integration(void) {
  char temp_dir[256];
  char input_path[512];
  uint8_t data[180];
  uint8_t received[256];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  file_sink_t sink_state;
  xyzm_sink_ops_t sink_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/sx", "-X", "-b", "-q", "input.bin", NULL};
  size_t read_len;

  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-xrecv");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  for (read_len = 0u; read_len < sizeof(data); ++read_len) {
    data[read_len] = (uint8_t)(255u - read_len);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&sink_state, 0, sizeof(sink_state));
  sink_state.directory = temp_dir;
  sink_state.fallback_name = "received.bin";
  sink_ops.opaque = &sink_state;
  sink_ops.begin = file_sink_begin;
  sink_ops.write = file_sink_write;
  sink_ops.end = file_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(sink_state.path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, 256u);
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  for (read_len = sizeof(data); read_len < 256u; ++read_len) {
    ASSERT_EQ_U8(received[read_len], 0x1au);
  }
  remove_tree(temp_dir);
}

static void test_xmodem_crc_send_integration(void) {
  char temp_dir[256];
  char input_path[512];
  char output_path[512];
  uint8_t data[190];
  uint8_t received[256];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  file_source_t source_state;
  xyzm_source_ops_t source_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/rx", "-X", "-c", "-b", "-q", "-y", "received.bin", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-xcrcsend");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  ASSERT_TRUE(snprintf(output_path, sizeof(output_path), "%s/received.bin", temp_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index ^ 0xa5u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&source_state, 0, sizeof(source_state));
  source_state.path = input_path;
  source_state.name = "input.bin";
  source_ops.opaque = &source_state;
  source_ops.next = file_source_next;
  source_ops.read = file_source_read;
  source_ops.end = file_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_CRC);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(output_path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, 256u);
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  for (read_len = sizeof(data); read_len < 256u; ++read_len) {
    ASSERT_EQ_U8(received[read_len], 0x1au);
  }
  remove_tree(temp_dir);
}

static void test_xmodem_crc_receive_integration(void) {
  char temp_dir[256];
  char input_path[512];
  uint8_t data[190];
  uint8_t received[256];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  file_sink_t sink_state;
  xyzm_sink_ops_t sink_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/sx", "-X", "-b", "-q", "input.bin", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-xcrcrecv");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index + 7u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&sink_state, 0, sizeof(sink_state));
  sink_state.directory = temp_dir;
  sink_state.fallback_name = "received.bin";
  sink_ops.opaque = &sink_state;
  sink_ops.begin = file_sink_begin;
  sink_ops.write = file_sink_write;
  sink_ops.end = file_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_CRC);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(sink_state.path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, 256u);
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  for (read_len = sizeof(data); read_len < 256u; ++read_len) {
    ASSERT_EQ_U8(received[read_len], 0x1au);
  }
  remove_tree(temp_dir);
}

static void test_xmodem_1k_send_integration(void) {
  char temp_dir[256];
  char input_path[512];
  char output_path[512];
  uint8_t data[1500];
  uint8_t received[2048];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  file_source_t source_state;
  xyzm_source_ops_t source_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/rx", "-X", "-b", "-q", "-y", "received.bin", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-x1ksend");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  ASSERT_TRUE(snprintf(output_path, sizeof(output_path), "%s/received.bin", temp_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index * 5u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&source_state, 0, sizeof(source_state));
  source_state.path = input_path;
  source_state.name = "input.bin";
  source_ops.opaque = &source_state;
  source_ops.next = file_source_next;
  source_ops.read = file_source_read;
  source_ops.end = file_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(output_path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, 2048u);
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  for (read_len = sizeof(data); read_len < 2048u; ++read_len) {
    ASSERT_EQ_U8(received[read_len], 0x1au);
  }
  remove_tree(temp_dir);
}

static void test_xmodem_1k_receive_integration(void) {
  char temp_dir[256];
  char input_path[512];
  uint8_t data[1500];
  uint8_t received[2048];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  file_sink_t sink_state;
  xyzm_sink_ops_t sink_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/sx", "-X", "-k", "-b", "-q", "input.bin", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-x1krecv");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index * 7u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&sink_state, 0, sizeof(sink_state));
  sink_state.directory = temp_dir;
  sink_state.fallback_name = "received.bin";
  sink_ops.opaque = &sink_state;
  sink_ops.begin = file_sink_begin;
  sink_ops.write = file_sink_write;
  sink_ops.end = file_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(sink_state.path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, 1536u);
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  for (read_len = sizeof(data); read_len < 1536u; ++read_len) {
    ASSERT_EQ_U8(received[read_len], 0x1au);
  }
  remove_tree(temp_dir);
}

static void test_xmodem_1k_large_integration(void) {
  char temp_dir[256];
  char input_path[512];
  char output_path[512];
  uint8_t *data;
  uint8_t *received;
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  file_source_t source_state;
  xyzm_source_ops_t source_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/rx", "-X", "-b", "-q", "-y", "received.bin", NULL};
  size_t data_len;
  size_t expected_len;
  size_t read_len;

  data_len = 16385u;
  make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-x1klarge");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  ASSERT_TRUE(snprintf(output_path, sizeof(output_path), "%s/received.bin", temp_dir) > 0);
  data = malloc(data_len);
  ASSERT_TRUE(data != NULL);
  fill_pattern(data, data_len, 0x31u);
  write_file(input_path, data, data_len);

  child = spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&source_state, 0, sizeof(source_state));
  source_state.path = input_path;
  source_state.name = "input.bin";
  source_ops.opaque = &source_state;
  source_ops.next = file_source_next;
  source_ops.read = file_source_read;
  source_ops.end = file_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_XMODEM_1K);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);

  expected_len = xmodem_1k_padded_size(data_len);
  received = read_file_alloc(output_path, &read_len);
  ASSERT_EQ_SIZE(read_len, expected_len);
  ASSERT_TRUE(memcmp(received, data, data_len) == 0);
  for (; data_len < read_len; ++data_len) {
    ASSERT_EQ_U8(received[data_len], 0x1au);
  }

  free(received);
  free(data);
  remove_tree(temp_dir);
}

static void test_xmodem_send_sync_integration(void) {
  test_xmodem_send_integration();
}

static void test_xmodem_receive_sync_integration(void) {
  test_xmodem_receive_integration();
}

static void test_xmodem_crc_send_sync_integration(void) {
  test_xmodem_crc_send_integration();
}

static void test_xmodem_crc_receive_sync_integration(void) {
  test_xmodem_crc_receive_integration();
}

static void test_xmodem_1k_send_sync_integration(void) {
  test_xmodem_1k_send_integration();
}

static void test_xmodem_1k_receive_sync_integration(void) {
  test_xmodem_1k_receive_integration();
}

static void test_xmodem_1k_large_sync_integration(void) {
  test_xmodem_1k_large_integration();
}

const test_case_t xyzm_xmodem_integration_tests[] = {
  TEST_CASE(test_xmodem_send_sync_integration, "Synchronous libxyzm XMODEM send interoperates with rx -X."),
  TEST_CASE(test_xmodem_receive_sync_integration, "Synchronous libxyzm XMODEM receive interoperates with sx -X."),
  TEST_CASE(test_xmodem_crc_send_sync_integration, "Synchronous libxyzm XMODEM-CRC send interoperates with rx -X -c."),
  TEST_CASE(test_xmodem_crc_receive_sync_integration, "Synchronous libxyzm XMODEM-CRC receive interoperates with sx -X."),
  TEST_CASE(test_xmodem_1k_send_sync_integration, "Synchronous libxyzm XMODEM-1K send interoperates with rx -X."),
  TEST_CASE(test_xmodem_1k_receive_sync_integration, "Synchronous libxyzm XMODEM-1K receive interoperates with sx -X -k."),
  TEST_CASE(test_xmodem_1k_large_sync_integration, "Synchronous libxyzm XMODEM-1K send handles large transfers."),
};

const size_t xyzm_xmodem_integration_tests_count = sizeof(xyzm_xmodem_integration_tests) / sizeof(xyzm_xmodem_integration_tests[0]);
