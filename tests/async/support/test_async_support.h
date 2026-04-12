/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef TEST_ASYNC_SUPPORT_H
#define TEST_ASYNC_SUPPORT_H

#include "libxyzm/async.h"
#include "../../../src/xyzm_common_internal.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <cstdint>
#else
#include <sys/types.h>
#endif

using test_async_fn_t = void (*)(void);

struct test_async_case_t {
  const char *name;
  const char *description;
  test_async_fn_t fn;
};

struct test_async_io_counters_t {
  std::size_t transport_wait_read_count = 0;
  std::size_t transport_wait_write_count = 0;
  std::size_t transport_read_count = 0;
  std::size_t transport_write_count = 0;
  std::size_t file_open_count = 0;
  std::size_t file_stat_count = 0;
  std::size_t file_read_count = 0;
  std::size_t file_write_count = 0;
  std::size_t file_close_count = 0;
};

struct test_async_fake_link_t {
  std::vector<uint8_t> incoming;
  std::size_t incoming_pos = 0;
  std::vector<uint8_t> outgoing;
  uint64_t now_ms = 0;
  std::size_t send_count = 0;
  std::size_t recv_count = 0;
  std::size_t max_send_len = 0;
  std::size_t max_recv_len = 0;
  uint32_t timeout_recv_count = 0;
};

struct test_async_memory_source_t {
  xyzm_async_file_info_t info;
  std::vector<uint8_t> data;
  std::size_t offset = 0;
  bool served = false;
  std::exception_ptr end_error;
};

struct test_async_memory_sink_t {
  xyzm_async_file_info_t info;
  std::vector<uint8_t> data;
  uint64_t resume_offset = 0;
  std::size_t write_count = 0;
  std::size_t max_write_len = 0;
  bool began = false;
  std::exception_ptr end_error;
};

struct test_async_fd_link_t {
#if defined(_WIN32)
  intptr_t fd = 0;
#else
  int fd = -1;
#endif
  test_async_io_counters_t *counters = nullptr;
};

struct test_async_file_source_t {
  std::string path;
  std::string name;
  bool served = false;
  uint64_t size = 0;
  uint64_t offset = 0;
  test_async_io_counters_t *counters = nullptr;
#if defined(_WIN32)
  void *handle = nullptr;
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
  int fd = -1;
  cardio::io_uring *io = nullptr;
#else
  int fd = -1;
#endif
};

struct test_async_file_sink_t {
  std::string directory;
  std::string fallback_name;
  std::string path;
  uint64_t offset = 0;
  test_async_io_counters_t *counters = nullptr;
#if defined(_WIN32)
  void *handle = nullptr;
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
  int fd = -1;
  cardio::io_uring *io = nullptr;
#else
  int fd = -1;
#endif
};

struct test_async_child_process_t {
#if defined(_WIN32)
  void *process_handle = nullptr;
  void *thread_handle = nullptr;
  intptr_t fd = 0;
  intptr_t stderr_fd = 0;
#else
  pid_t pid = -1;
  int fd = -1;
  int stderr_fd = -1;
#endif
};

#define ASYNC_ASSERT_TRUE(expr) \
  do { \
    if (!(expr)) { \
      test_async_fail_now(__FILE__, __LINE__, "assertion failed: %s", #expr); \
    } \
  } while (false)

#define ASYNC_ASSERT_EQ_U8(actual, expected) \
  ASYNC_ASSERT_TRUE(static_cast<uint8_t>(actual) == static_cast<uint8_t>(expected))

#define ASYNC_ASSERT_EQ_SIZE(actual, expected) \
  ASYNC_ASSERT_TRUE(static_cast<std::size_t>(actual) == static_cast<std::size_t>(expected))

#define ASYNC_TEST_CASE(fn, description_text) \
  { #fn, description_text, fn }

[[noreturn]] void test_async_fail_now(
    const char *file,
    int line,
    const char *fmt,
    ...);
void test_async_logf(const char *fmt, ...);
void test_async_create_results_dir(char *path, std::size_t capacity);
int test_async_run_cases(
    const char *results_dir,
    const test_async_case_t *cases,
    std::size_t count,
    unsigned *passed,
    unsigned *failed);

template <typename Start>
void test_async_run(Start start) {
  cardio::dispatcher_host dispatcher;
  auto root = [&]() -> cardio::promise<void> {
    co_await start();
  }();
  dispatcher.park();
  ASYNC_ASSERT_TRUE(root.is_ready());
  root.unsafe_result();
}

void test_async_build_fake_link_ops(
    test_async_fake_link_t *link,
    xyzm_async_link_ops_t *ops);
void test_async_build_memory_source_ops(
    test_async_memory_source_t *source,
    xyzm_async_source_ops_t *ops);
void test_async_build_memory_sink_ops(
    test_async_memory_sink_t *sink,
    xyzm_async_sink_ops_t *ops);
void test_async_build_fd_link_ops(
    test_async_fd_link_t *link,
    xyzm_async_link_ops_t *ops);
void test_async_build_file_source_ops(
    test_async_file_source_t *source,
    xyzm_async_source_ops_t *ops);
void test_async_build_file_sink_ops(
    test_async_file_sink_t *sink,
    xyzm_async_sink_ops_t *ops);

xyzm_xmodem_send_opts_t test_async_xmodem_send_options(
    xyzm_xmodem_checksum_mode_t checksum_mode,
    xyzm_xmodem_packet_size_t packet_size);
xyzm_xmodem_receive_opts_t test_async_xmodem_receive_options(
    xyzm_xmodem_checksum_mode_t checksum_mode);
xyzm_ymodem_opts_t test_async_ymodem_options(xyzm_ymodem_variant_t variant);
xyzm_zmodem_opts_t test_async_zmodem_options(void);

void test_async_append_xmodem_block(
    std::vector<uint8_t> *buffer,
    uint8_t block_number,
    std::span<const uint8_t> payload,
    bool use_crc,
    uint8_t header);
void test_async_fill_pattern(uint8_t *buf, std::size_t len, uint8_t seed);
std::size_t test_async_xmodem_1k_padded_size(std::size_t data_len);
void test_async_make_temp_dir(char *path, std::size_t capacity, const char *prefix);
void test_async_remove_tree(const char *path);
void test_async_write_file(const char *path, std::span<const uint8_t> data);
std::vector<uint8_t> test_async_read_file(const char *path);
test_async_child_process_t test_async_spawn_child(
    char *const argv[],
    const char *working_directory);
void test_async_wait_child_ok(test_async_child_process_t *child);

#endif
