/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "minizm_sync_transport.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Returns a monotonic timestamp for the custom synchronous transport.
 *
 * @param opaque Unused transport state pointer.
 * @return Monotonic time in milliseconds.
 */
static uint64_t minizm_sync_transport_now_ms(void *opaque) {
  (void)opaque;

  /* The sample transport does not need state to read the monotonic clock. */
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0u;
  }
  return ((uint64_t)ts.tv_sec * 1000u) + (uint64_t)(ts.tv_nsec / 1000000L);
}

/**
 * @brief Waits for a transport descriptor to become readable or writable.
 *
 * @param fd File descriptor to monitor.
 * @param events `poll(2)` event mask that represents the desired readiness.
 * @param timeout_ms Maximum wait duration in milliseconds.
 * @return Transport status derived from the poll result.
 */
static xyzm_status_t minizm_sync_transport_wait_fd_ready(
    int fd,
    short events,
    uint32_t timeout_ms) {
  /* Build a one-element poll set because the sample uses one fd per direction. */
  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = events;

  /* Retry transparently when the wait is interrupted by a signal. */
  int rc;
  do {
    rc = poll(&pfd, 1, (int)timeout_ms);
  } while (rc < 0 && errno == EINTR);

  /* Convert the raw poll result into the library's transport status model. */
  if (rc == 0) {
    return XYZM_STATUS_TIMEOUT;
  }
  if (rc < 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  if ((pfd.revents & POLLNVAL) != 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  if ((pfd.revents & (POLLERR | POLLHUP)) != 0 && (pfd.revents & events) == 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Writes raw bytes through the custom synchronous transport.
 *
 * @param opaque Transport state supplied by the caller.
 * @param buf Bytes that should be written.
 * @param len Requested byte count.
 * @param timeout_ms Maximum time to wait for writability.
 * @param written_len Output byte count written during this call.
 * @return Transport status for the write attempt.
 */
static xyzm_status_t minizm_sync_transport_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  /* Recover the transport state and reset the caller-visible write count. */
  minizm_sync_transport_t *transport = opaque;
  *written_len = 0u;

  /* Wait for the output descriptor before attempting a single raw write. */
  xyzm_status_t status = minizm_sync_transport_wait_fd_ready(
      transport->write_fd,
      POLLOUT,
      timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  /* Retry only on EINTR so partial progress semantics stay library-driven. */
  ssize_t rc;
  do {
    rc = write(transport->write_fd, buf, len);
  } while (rc < 0 && errno == EINTR);

  /* Report the write in the status/length form expected by `xyzm_link_ops_t`. */
  if (rc <= 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  *written_len = (size_t)rc;
  return XYZM_STATUS_OK;
}

/**
 * @brief Reads raw bytes through the custom synchronous transport.
 *
 * @param opaque Transport state supplied by the caller.
 * @param buf Destination buffer for received bytes.
 * @param capacity Maximum number of bytes that can be stored in @p buf.
 * @param timeout_ms Maximum time to wait for readability.
 * @param read_len Output byte count read during this call.
 * @return Transport status for the read attempt.
 */
static xyzm_status_t minizm_sync_transport_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  /* Recover the transport state and reset the caller-visible read count. */
  minizm_sync_transport_t *transport = opaque;
  *read_len = 0u;

  /* Wait for the input descriptor before attempting a single raw read. */
  xyzm_status_t status = minizm_sync_transport_wait_fd_ready(
      transport->read_fd,
      POLLIN,
      timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  /* Retry only on EINTR so partial progress semantics stay library-driven. */
  ssize_t rc;
  do {
    rc = read(transport->read_fd, buf, capacity);
  } while (rc < 0 && errno == EINTR);

  /* Report the read in the status/length form expected by `xyzm_link_ops_t`. */
  if (rc <= 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  *read_len = (size_t)rc;
  return XYZM_STATUS_OK;
}

/**
 * @brief Binds the custom synchronous transport to `stdin` and `stdout`.
 *
 * @param link Output transport callback table.
 * @param transport Output transport state owned by the caller.
 */
void minizm_sync_transport_init_stdio_link(
    xyzm_link_ops_t *link,
    minizm_sync_transport_t *transport) {
  /* Fix the transport onto stdio so the sample stays comparable to basic/sync. */
  transport->read_fd = STDIN_FILENO;
  transport->write_fd = STDOUT_FILENO;

  /* Publish the state pointer and the three required transport callbacks. */
  link->opaque = transport;
  link->send = minizm_sync_transport_send;
  link->recv = minizm_sync_transport_recv;
  link->now_ms = minizm_sync_transport_now_ms;
}
