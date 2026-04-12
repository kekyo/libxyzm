/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

static void test_zmodem_send_integration(void) {
  char source_dir[256];
  char receive_dir[256];
  char input_path[512];
  char output_path[512];
  uint8_t data[600];
  uint8_t received[1024];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  file_source_t source_state;
  xyzm_source_ops_t source_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/rz", "-Z", "-b", "-q", "-y", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(source_dir, sizeof(source_dir), "libxyzm-zsend-src");
  make_temp_dir(receive_dir, sizeof(receive_dir), "libxyzm-zsend-dst");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", source_dir) > 0);
  ASSERT_TRUE(snprintf(output_path, sizeof(output_path), "%s/input.bin", receive_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index * 11u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, receive_dir);
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

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(output_path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, sizeof(data));
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  remove_tree(source_dir);
  remove_tree(receive_dir);
}

static void test_zmodem_receive_integration(void) {
  char source_dir[256];
  char receive_dir[256];
  char input_path[512];
  uint8_t data[620];
  uint8_t received[1024];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  file_sink_t sink_state;
  xyzm_sink_ops_t sink_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/sz", "--zmodem", "-b", "-q", "input.bin", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(source_dir, sizeof(source_dir), "libxyzm-zrecv-src");
  make_temp_dir(receive_dir, sizeof(receive_dir), "libxyzm-zrecv-dst");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", source_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index ^ 0x96u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, source_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&sink_state, 0, sizeof(sink_state));
  sink_state.directory = receive_dir;
  sink_state.fallback_name = "fallback.bin";
  sink_ops.opaque = &sink_state;
  sink_ops.begin = file_sink_begin;
  sink_ops.write = file_sink_write;
  sink_ops.end = file_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(sink_state.path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, sizeof(data));
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  remove_tree(source_dir);
  remove_tree(receive_dir);
}

static void test_zmodem_receive_crc16_integration(void) {
  char source_dir[256];
  char receive_dir[256];
  char input_path[512];
  uint8_t data[300];
  uint8_t received[512];
  child_process_t child;
  fd_link_t fd_link;
  xyzm_link_ops_t link_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  file_sink_t sink_state;
  xyzm_sink_ops_t sink_ops;
  xyzm_status_t status;
  char *argv[] = {"/usr/bin/sz", "--zmodem", "-o", "-b", "-q", "input.bin", NULL};
  size_t index;
  size_t read_len;

  make_temp_dir(source_dir, sizeof(source_dir), "libxyzm-zrecv16-src");
  make_temp_dir(receive_dir, sizeof(receive_dir), "libxyzm-zrecv16-dst");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", source_dir) > 0);
  for (index = 0u; index < sizeof(data); ++index) {
    data[index] = (uint8_t)(index * 3u);
  }
  write_file(input_path, data, sizeof(data));

  child = spawn_child(argv, source_dir);
  fd_link.fd = child.fd;
  link_ops.opaque = &fd_link;
  link_ops.send = fd_send;
  link_ops.recv = fd_recv;
  link_ops.now_ms = fd_now_ms;

  memset(&sink_state, 0, sizeof(sink_state));
  sink_state.directory = receive_dir;
  sink_state.fallback_name = "fallback.bin";
  sink_ops.opaque = &sink_state;
  sink_ops.begin = file_sink_begin;
  sink_ops.write = file_sink_write;
  sink_ops.end = file_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);
  read_len = read_file(sink_state.path, received, sizeof(received));
  ASSERT_EQ_SIZE(read_len, sizeof(data));
  ASSERT_TRUE(memcmp(received, data, sizeof(data)) == 0);
  remove_tree(source_dir);
  remove_tree(receive_dir);
}

static void test_zmodem_large_send_integration(void) {
  char source_dir[256];
  char receive_dir[256];
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
  char *argv[] = {"/usr/bin/rz", "-Z", "-b", "-q", "-y", NULL};
  size_t data_len;
  size_t read_len;

  data_len = 131075u;
  make_temp_dir(source_dir, sizeof(source_dir), "libxyzm-zlarge-src");
  make_temp_dir(receive_dir, sizeof(receive_dir), "libxyzm-zlarge-dst");
  ASSERT_TRUE(snprintf(input_path, sizeof(input_path), "%s/input.bin", source_dir) > 0);
  ASSERT_TRUE(snprintf(output_path, sizeof(output_path), "%s/input.bin", receive_dir) > 0);
  data = malloc(data_len);
  ASSERT_TRUE(data != NULL);
  fill_pattern(data, data_len, 0x63u);
  write_file(input_path, data, data_len);

  child = spawn_child(argv, receive_dir);
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

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  wait_child_ok(&child);

  received = read_file_alloc(output_path, &read_len);
  ASSERT_EQ_SIZE(read_len, data_len);
  ASSERT_TRUE(memcmp(received, data, data_len) == 0);

  free(received);
  free(data);
  remove_tree(source_dir);
  remove_tree(receive_dir);
}

static void test_zmodem_send_sync_integration(void) {
  test_zmodem_send_integration();
}

static void test_zmodem_receive_sync_integration(void) {
  test_zmodem_receive_integration();
}

static void test_zmodem_receive_crc16_sync_integration(void) {
  test_zmodem_receive_crc16_integration();
}

static void test_zmodem_large_send_sync_integration(void) {
  test_zmodem_large_send_integration();
}


const test_case_t xyzm_zmodem_integration_tests[] = {
  TEST_CASE(test_zmodem_send_sync_integration, "Synchronous libxyzm ZMODEM send interoperates with rz."),
  TEST_CASE(test_zmodem_receive_sync_integration, "Synchronous libxyzm ZMODEM receive interoperates with sz --zmodem."),
  TEST_CASE(test_zmodem_receive_crc16_sync_integration, "Synchronous libxyzm ZMODEM receive interoperates with a 16-bit FCS sender."),
  TEST_CASE(test_zmodem_large_send_sync_integration, "Synchronous libxyzm ZMODEM send handles large transfers."),
};

const size_t xyzm_zmodem_integration_tests_count = sizeof(xyzm_zmodem_integration_tests) / sizeof(xyzm_zmodem_integration_tests[0]);
