/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef TEST_SYNC_SUPPORT_H
#define TEST_SYNC_SUPPORT_H

#define _POSIX_C_SOURCE 200809L

#include "libxyzm/xyzm.h"
#include "../../../src/xyzm_common_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*test_fn_t)(void);

typedef struct test_case {
  const char *name;
  const char *description;
  test_fn_t fn;
} test_case_t;

/**
 * @brief Test-only protocol selector used by the synchronous test helpers.
 */
typedef enum test_sync_protocol {
  TEST_SYNC_PROTOCOL_XMODEM = 1,
  TEST_SYNC_PROTOCOL_XMODEM_1K = 2,
  TEST_SYNC_PROTOCOL_XMODEM_CRC = 3,
  TEST_SYNC_PROTOCOL_YMODEM = 4,
  TEST_SYNC_PROTOCOL_YMODEM_G = 5,
  TEST_SYNC_PROTOCOL_ZMODEM = 6,
  TEST_SYNC_PROTOCOL_XMODEM_AUTO = 7,
  TEST_SYNC_PROTOCOL_XMODEM_1K_CRC = 8,
  TEST_SYNC_PROTOCOL_YMODEM_AUTO = 9
} test_sync_protocol_t;

/**
 * @brief Test-only options shared by the synchronous protocol helper entry points.
 */
typedef struct test_sync_transfer_opts {
  test_sync_protocol_t protocol;
  uint32_t handshake_timeout_ms;
  uint32_t block_timeout_ms;
  uint32_t retry_limit;
  uint8_t pad_byte;
  uint16_t zmodem_packet_len;
  uint8_t zmodem_escape_ctrl;
  uint8_t zmodem_use_crc32;
} test_sync_transfer_opts_t;

typedef struct test_sync_send_request {
  const xyzm_link_ops_t *link;
  const xyzm_source_ops_t *source;
  const test_sync_transfer_opts_t *options;
} test_sync_send_request_t;

typedef struct test_sync_receive_request {
  const xyzm_link_ops_t *link;
  const xyzm_sink_ops_t *sink;
  const test_sync_transfer_opts_t *options;
} test_sync_receive_request_t;

typedef struct fake_link {
  uint8_t incoming[8192];
  size_t incoming_len;
  size_t incoming_pos;
  uint8_t outgoing[8192];
  size_t outgoing_len;
  uint64_t now_ms;
  uint32_t timeout_recv_count;
} fake_link_t;

typedef struct timed_link {
  uint8_t *incoming;
  size_t incoming_len;
  size_t incoming_pos;
  uint8_t *outgoing;
  size_t outgoing_len;
  size_t outgoing_capacity;
  uint64_t now_us;
  uint32_t send_base_us;
  uint32_t send_byte_us;
  uint32_t recv_base_us;
  uint32_t recv_byte_us;
  uint32_t send_calls;
  uint32_t recv_calls;
  size_t max_recv_capacity;
} timed_link_t;

typedef struct memory_source {
  xyzm_file_info_t info;
  const uint8_t *data;
  size_t data_len;
  size_t offset;
  bool served;
  xyzm_status_t seek_result;
  xyzm_status_t end_result;
} memory_source_t;

typedef struct memory_sink {
  xyzm_file_info_t info;
  uint8_t data[4096];
  size_t data_len;
  uint64_t resume_offset;
  bool began;
  xyzm_status_t end_result;
} memory_sink_t;

typedef struct fd_link {
#if defined(_WIN32)
  intptr_t fd;
#else
  int fd;
#endif
} fd_link_t;

typedef struct file_source {
  const char *path;
  const char *name;
  FILE *stream;
  bool served;
  size_t size;
} file_source_t;

typedef struct file_sink {
  const char *directory;
  const char *fallback_name;
  char path[512];
  FILE *stream;
} file_sink_t;

typedef struct child_process {
#if defined(_WIN32)
  void *process_handle;
  void *thread_handle;
  intptr_t fd;
  intptr_t stderr_fd;
#else
  pid_t pid;
  int fd;
  int stderr_fd;
#endif
} child_process_t;

enum {
  TEST_ZPAD = '*',
  TEST_ZDLE = 0x18,
  TEST_ZBIN = 'A',
  TEST_ZBIN32 = 'C',
  TEST_ZHEX = 'B',
  TEST_ZRQINIT = 0,
  TEST_ZRINIT = 1,
  TEST_ZACK = 3,
  TEST_ZFILE = 4,
  TEST_ZSKIP = 5,
  TEST_ZFIN = 8,
  TEST_ZRPOS = 9,
  TEST_ZDATA = 10,
  TEST_ZEOF = 11,
  TEST_ZCRCE = 'h',
  TEST_ZCRCG = 'i',
  TEST_ZCRCW = 'k',
};

#define ASSERT_TRUE(expr) \
  do { \
    if (!(expr)) { \
      fail_now(__FILE__, __LINE__, "assertion failed: %s", #expr); \
    } \
  } while (0)

#define ASSERT_EQ_U8(actual, expected) \
  ASSERT_TRUE((uint8_t)(actual) == (uint8_t)(expected))

#define ASSERT_EQ_SIZE(actual, expected) \
  ASSERT_TRUE((size_t)(actual) == (size_t)(expected))

#define ASSERT_EQ_STATUS(actual, expected) \
  do { \
    const xyzm_status_t actual_status__ = (actual); \
    const xyzm_status_t expected_status__ = (expected); \
    if (actual_status__ != expected_status__) { \
      fail_now( \
          __FILE__, \
          __LINE__, \
          "assertion failed: %s == %s (actual=%d expected=%d)", \
          #actual, \
          #expected, \
          (int)actual_status__, \
          (int)expected_status__); \
    } \
  } while (0)

#define TEST_CASE(fn, description_text) \
  { #fn, description_text, fn }

void fail_now(const char *file, int line, const char *fmt, ...);
void test_logf(const char *fmt, ...);
void test_create_results_dir(char *path, size_t capacity);
int test_run_cases(
    const char *results_dir,
    const test_case_t *cases,
    size_t count,
    unsigned *passed,
    unsigned *failed);
xyzm_status_t test_sync_send(
    const test_sync_send_request_t *request,
    xyzm_transfer_report_t *report);
xyzm_status_t test_sync_send_observed(
    const test_sync_send_request_t *request,
    const xyzm_transfer_observer_t *observer,
    xyzm_transfer_report_t *report);
xyzm_status_t test_sync_receive(
    const test_sync_receive_request_t *request,
    xyzm_transfer_report_t *report);
xyzm_status_t test_sync_receive_observed(
    const test_sync_receive_request_t *request,
    const xyzm_transfer_observer_t *observer,
    xyzm_transfer_report_t *report);

xyzm_status_t fake_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len);
xyzm_status_t fake_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len);
uint64_t fake_now_ms(void *opaque);

xyzm_status_t timed_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len);
xyzm_status_t timed_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len);
uint64_t timed_now_ms(void *opaque);

xyzm_status_t memory_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque);
xyzm_status_t memory_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len);
xyzm_status_t memory_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset);
xyzm_status_t memory_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result);
xyzm_status_t memory_sink_begin(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque);
xyzm_status_t memory_sink_write(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len);
xyzm_status_t memory_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result);

xyzm_status_t fd_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len);
xyzm_status_t fd_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len);
uint64_t fd_now_ms(void *opaque);

size_t file_size_for_path(const char *path);
xyzm_status_t file_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque);
xyzm_status_t file_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len);
xyzm_status_t file_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset);
xyzm_status_t file_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result);
xyzm_status_t file_sink_begin(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque);
xyzm_status_t file_sink_write(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len);
xyzm_status_t file_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result);
void append_xmodem_block(
    uint8_t *buffer,
    size_t *offset,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool use_crc,
    uint8_t header);
void remove_tree(const char *path);
void write_file(const char *path, const uint8_t *data, size_t len);
size_t read_file(const char *path, uint8_t *buf, size_t capacity);
void fill_pattern(uint8_t *buf, size_t len, uint8_t seed);
uint8_t *read_file_alloc(const char *path, size_t *len);
size_t xmodem_1k_padded_size(size_t data_len);
child_process_t spawn_child(char *const argv[], const char *working_directory);
void read_child_stderr(int fd, char *buf, size_t capacity);
void wait_child_ok(child_process_t *child);
void populate_sync_default_options(test_sync_transfer_opts_t *options, test_sync_protocol_t protocol);
uint64_t bytes_per_second(uint64_t payload_bytes, uint64_t elapsed_us);
void append_repeated_byte(uint8_t *buf, size_t *used_len, uint8_t value, size_t count);
void fill_xmodem_speed_incoming(
    uint8_t *buf,
    size_t *used_len,
    uint8_t start_char,
    size_t block_count);
void fill_ymodem_speed_incoming(
    uint8_t *buf,
    size_t *used_len,
    bool streaming,
    size_t block_count);
void fill_zmodem_speed_incoming(uint8_t *buf, size_t *used_len);
uint16_t test_zmodem_crc16_entry(uint8_t index);
uint16_t test_zmodem_crc16_update(uint16_t crc, uint8_t value);
uint32_t test_zmodem_crc32_update(uint32_t crc, uint8_t value);
void append_zmodem_escaped_byte(
    uint8_t *buf,
    size_t *used_len,
    uint8_t value,
    bool escape_ctrl);
void append_zmodem_hex_header(
    uint8_t *buf,
    size_t *used_len,
    uint8_t type,
    const uint8_t hdr[4]);
void append_zmodem_binary_header(
    uint8_t *buf,
    size_t *used_len,
    uint8_t type,
    const uint8_t hdr[4],
    bool use_crc32,
    bool escape_ctrl);
void append_zmodem_data_subpacket(
    uint8_t *buf,
    size_t *used_len,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl);
size_t find_bytes(
    const uint8_t *haystack,
    size_t haystack_len,
    const uint8_t *needle,
    size_t needle_len);
size_t count_bytes_pattern(
    const uint8_t *haystack,
    size_t haystack_len,
    const uint8_t *needle,
    size_t needle_len);
void make_temp_dir(char *path, size_t capacity, const char *prefix);

#ifdef __cplusplus
}
#endif

#endif
