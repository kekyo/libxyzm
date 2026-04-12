/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

#include "../../samples/support/minizm_posix.h"

#include <unistd.h>

static void test_samples_posix_fd_link_send_unit(void) {
  int pipe_fds[2];
  minizm_posix_fd_link_t link_state;
  xyzm_link_ops_t link;
  const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
  uint8_t actual[sizeof(payload)];
  size_t written_len;
  ssize_t read_len;

  ASSERT_TRUE(pipe(pipe_fds) == 0);
  minizm_posix_init_fd_link(&link, &link_state, pipe_fds[0], pipe_fds[1]);

  ASSERT_EQ_STATUS(
      link.send(link.opaque, payload, sizeof(payload), 100u, &written_len),
      XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(written_len, sizeof(payload));

  read_len = read(pipe_fds[0], actual, sizeof(actual));
  ASSERT_TRUE(read_len == (ssize_t)sizeof(actual));
  ASSERT_TRUE(memcmp(actual, payload, sizeof(payload)) == 0);

  minizm_posix_cleanup_fd_link(&link_state);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_samples_posix_fd_link_recv_unit(void) {
  int pipe_fds[2];
  minizm_posix_fd_link_t link_state;
  xyzm_link_ops_t link;
  const uint8_t payload[] = {'w', 'o', 'r', 'l', 'd'};
  uint8_t actual[sizeof(payload)];
  size_t read_len;
  ssize_t written_len;

  ASSERT_TRUE(pipe(pipe_fds) == 0);
  minizm_posix_init_fd_link(&link, &link_state, pipe_fds[0], pipe_fds[1]);

  written_len = write(pipe_fds[1], payload, sizeof(payload));
  ASSERT_TRUE(written_len == (ssize_t)sizeof(payload));

  ASSERT_EQ_STATUS(
      link.recv(link.opaque, actual, sizeof(actual), 100u, &read_len),
      XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(read_len, sizeof(payload));
  ASSERT_TRUE(memcmp(actual, payload, sizeof(payload)) == 0);

  minizm_posix_cleanup_fd_link(&link_state);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

static void test_samples_posix_fd_link_timeout_unit(void) {
  int pipe_fds[2];
  minizm_posix_fd_link_t link_state;
  xyzm_link_ops_t link;
  uint8_t actual[8];
  size_t read_len;

  ASSERT_TRUE(pipe(pipe_fds) == 0);
  minizm_posix_init_fd_link(&link, &link_state, pipe_fds[0], pipe_fds[1]);

  ASSERT_EQ_STATUS(
      link.recv(link.opaque, actual, sizeof(actual), 10u, &read_len),
      XYZM_STATUS_TIMEOUT);
  ASSERT_EQ_SIZE(read_len, 0u);

  minizm_posix_cleanup_fd_link(&link_state);
  ASSERT_TRUE(close(pipe_fds[0]) == 0);
  ASSERT_TRUE(close(pipe_fds[1]) == 0);
}

const test_case_t xyzm_samples_posix_tests[] = {
  TEST_CASE(
      test_samples_posix_fd_link_send_unit,
      "Default POSIX sample transport writes bytes through its configured file descriptor."),
  TEST_CASE(
      test_samples_posix_fd_link_recv_unit,
      "Default POSIX sample transport reads bytes through its configured file descriptor."),
  TEST_CASE(
      test_samples_posix_fd_link_timeout_unit,
      "Default POSIX sample transport reports timeout when no input becomes readable."),
};

const size_t xyzm_samples_posix_tests_count =
    sizeof(xyzm_samples_posix_tests) / sizeof(xyzm_samples_posix_tests[0]);
