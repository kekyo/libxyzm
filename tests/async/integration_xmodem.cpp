/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_async_support.h"

#include <algorithm>
#include <array>
#include <cstring>

static void test_async_xmodem_send_uses_cardio_fd_and_file_io(void) {
  char temp_dir[512];
  char input_path[768];
  char output_path[768];
  std::array<uint8_t, 200> data{};
  test_async_child_process_t child{};
  test_async_fd_link_t fd_link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_file_source_t source{};
  xyzm_async_source_ops_t source_ops{};
  test_async_io_counters_t counters{};
  auto options = test_async_xmodem_send_options(
      XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
      XYZM_XMODEM_PACKET_SIZE_128);
  xyzm_xmodem_send_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  char rx[] = "/usr/bin/rx";
  char mode[] = "-X";
  char binary[] = "-b";
  char quiet[] = "-q";
  char overwrite[] = "-y";
  char received_name[] = "received.bin";
  char *argv[] = {rx, mode, binary, quiet, overwrite, received_name, nullptr};

  test_async_make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-async-xsend");
  ASYNC_ASSERT_TRUE(std::snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  ASYNC_ASSERT_TRUE(std::snprintf(output_path, sizeof(output_path), "%s/received.bin", temp_dir) > 0);
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<uint8_t>(index);
  }
  test_async_write_file(input_path, std::span<const uint8_t>(data.data(), data.size()));

  child = test_async_spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  fd_link.counters = &counters;
  test_async_build_fd_link_ops(&fd_link, &link_ops);

#if defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
  cardio::io_uring file_io;
  source.io = &file_io;
#endif
  source.path = input_path;
  source.name = "input.bin";
  source.counters = &counters;
  test_async_build_file_source_ops(&source, &source_ops);

  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  test_async_run([&]() {
    return xyzm_xmodem_send_async(&request, &report, {});
  });
  test_async_wait_child_ok(&child);

  const auto received = test_async_read_file(output_path);
  ASYNC_ASSERT_EQ_SIZE(received.size(), 256u);
  ASYNC_ASSERT_TRUE(std::memcmp(received.data(), data.data(), data.size()) == 0);
  for (std::size_t index = data.size(); index < received.size(); ++index) {
    ASYNC_ASSERT_EQ_U8(received[index], 0x1au);
  }
  ASYNC_ASSERT_TRUE(counters.transport_wait_read_count > 0u);
  ASYNC_ASSERT_TRUE(counters.transport_wait_write_count > 0u);
  ASYNC_ASSERT_TRUE(counters.transport_read_count > 0u);
  ASYNC_ASSERT_TRUE(counters.transport_write_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_stat_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_open_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_read_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_close_count > 0u);
  ASYNC_ASSERT_TRUE(report.final_result.has_value());

  test_async_remove_tree(temp_dir);
}

static void test_async_xmodem_receive_uses_cardio_fd_and_file_io(void) {
  char temp_dir[512];
  char input_path[768];
  std::array<uint8_t, 180> data{};
  test_async_child_process_t child{};
  test_async_fd_link_t fd_link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_file_sink_t sink{};
  xyzm_async_sink_ops_t sink_ops{};
  test_async_io_counters_t counters{};
  auto options = test_async_xmodem_receive_options(XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM);
  xyzm_xmodem_receive_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  char sx[] = "/usr/bin/sx";
  char mode[] = "-X";
  char binary[] = "-b";
  char quiet[] = "-q";
  char input_name[] = "input.bin";
  char *argv[] = {sx, mode, binary, quiet, input_name, nullptr};

  test_async_make_temp_dir(temp_dir, sizeof(temp_dir), "libxyzm-async-xrecv");
  ASYNC_ASSERT_TRUE(std::snprintf(input_path, sizeof(input_path), "%s/input.bin", temp_dir) > 0);
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<uint8_t>(255u - index);
  }
  test_async_write_file(input_path, std::span<const uint8_t>(data.data(), data.size()));

  child = test_async_spawn_child(argv, temp_dir);
  fd_link.fd = child.fd;
  fd_link.counters = &counters;
  test_async_build_fd_link_ops(&fd_link, &link_ops);

#if defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
  cardio::io_uring file_io;
  sink.io = &file_io;
#endif
  sink.directory = temp_dir;
  sink.fallback_name = "received.bin";
  sink.counters = &counters;
  test_async_build_file_sink_ops(&sink, &sink_ops);

  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  test_async_run([&]() {
    return xyzm_xmodem_receive_async(&request, &report, {});
  });
  test_async_wait_child_ok(&child);

  const auto received = test_async_read_file(sink.path.c_str());
  ASYNC_ASSERT_EQ_SIZE(received.size(), 256u);
  ASYNC_ASSERT_TRUE(std::memcmp(received.data(), data.data(), data.size()) == 0);
  for (std::size_t index = data.size(); index < received.size(); ++index) {
    ASYNC_ASSERT_EQ_U8(received[index], 0x1au);
  }
  ASYNC_ASSERT_TRUE(counters.transport_wait_read_count > 0u);
  ASYNC_ASSERT_TRUE(counters.transport_wait_write_count > 0u);
  ASYNC_ASSERT_TRUE(counters.transport_read_count > 0u);
  ASYNC_ASSERT_TRUE(counters.transport_write_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_open_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_write_count > 0u);
  ASYNC_ASSERT_TRUE(counters.file_close_count > 0u);
  ASYNC_ASSERT_TRUE(report.final_result.has_value());

  test_async_remove_tree(temp_dir);
}

extern const test_async_case_t xyzm_async_xmodem_integration_tests[] = {
  ASYNC_TEST_CASE(test_async_xmodem_send_uses_cardio_fd_and_file_io, "Asynchronous XMODEM send uses cardio fd waits and async file reads."),
  ASYNC_TEST_CASE(test_async_xmodem_receive_uses_cardio_fd_and_file_io, "Asynchronous XMODEM receive uses cardio fd waits and async file writes."),
};

extern const std::size_t xyzm_async_xmodem_integration_tests_count =
    sizeof(xyzm_async_xmodem_integration_tests) /
    sizeof(xyzm_async_xmodem_integration_tests[0]);
