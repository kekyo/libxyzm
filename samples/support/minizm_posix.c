/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "minizm_posix.h"

#include <errno.h>
#include <fcntl.h>
#if defined(__linux__)
#include <liburing.h>
#endif
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Returns a monotonic timestamp in milliseconds for sample helpers.
 *
 * @return Monotonic time in milliseconds.
 */
static uint64_t minizm_posix_monotonic_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0u;
  }
  return ((uint64_t)ts.tv_sec * 1000u) + (uint64_t)(ts.tv_nsec / 1000000L);
}

/**
 * @brief Waits for a file descriptor to become readable or writable.
 *
 * @param fd File descriptor to monitor.
 * @param events Requested `poll(2)` event mask.
 * @param timeout_ms Maximum time to wait for readiness.
 * @return Status describing the wait result.
 */
static xyzm_status_t minizm_posix_wait_fd_ready(int fd, short events, uint32_t timeout_ms) {
  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = events;

  int rc;
  do {
    rc = poll(&pfd, 1, (int)timeout_ms);
  } while (rc < 0 && errno == EINTR);

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

#if defined(__linux__)
enum {
  MINIZM_POSIX_IO_URING_REQUEST_USER_DATA = 1u,
  MINIZM_POSIX_IO_URING_TIMEOUT_USER_DATA = 2u,
};

/**
 * @brief Returns the helper-owned `io_uring` instance for Linux transports.
 *
 * @param link POSIX file-descriptor transport state.
 * @return Active `io_uring` instance, or `NULL` when the helper should fall
 *         back to the portable poll path.
 *
 * @remarks Some kernels or sandboxes reject `io_uring_setup(2)`. The sample
 *          keeps working by marking the backend unavailable and reusing the
 *          existing `poll(2)` implementation in that case.
 */
static struct io_uring *minizm_posix_fd_link_get_io_uring(minizm_posix_fd_link_t *link) {
  if (link->io_uring_initialized) {
    return link->io_uring_enabled ? link->io_uring_handle : NULL;
  }

  link->io_uring_initialized = true;
  link->io_uring_enabled = false;
  link->io_uring_handle = NULL;

  struct io_uring *ring = malloc(sizeof(*ring));
  if (ring == NULL) {
    return NULL;
  }

  int rc = io_uring_queue_init(2u, ring, 0u);
  if (rc < 0) {
    free(ring);
    return NULL;
  }

  link->io_uring_handle = ring;
  link->io_uring_enabled = true;
  return ring;
}

/**
 * @brief Disables and destroys the Linux `io_uring` backend for one link.
 *
 * @param link POSIX file-descriptor transport state.
 */
static void minizm_posix_fd_link_disable_io_uring(minizm_posix_fd_link_t *link) {
  struct io_uring *ring = link->io_uring_handle;
  if (ring != NULL) {
    io_uring_queue_exit(ring);
    free(ring);
  }
  link->io_uring_handle = NULL;
  link->io_uring_enabled = false;
}

/**
 * @brief Executes one read or write through Linux `io_uring`.
 *
 * @param link POSIX file-descriptor transport state.
 * @param fd File descriptor used by the request.
 * @param send_mode `true` for writes, `false` for reads.
 * @param buf Transfer buffer.
 * @param len Requested byte count.
 * @param timeout_ms Maximum wait duration in milliseconds.
 * @param transferred_len Output byte count transferred during this call.
 * @return Transport status for the request, or @ref XYZM_STATUS_UNSUPPORTED
 *         when the helper should fall back to the portable path.
 */
static xyzm_status_t minizm_posix_fd_io_uring(
    minizm_posix_fd_link_t *link,
    int fd,
    bool send_mode,
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *transferred_len) {
  *transferred_len = 0u;
  struct io_uring *ring = minizm_posix_fd_link_get_io_uring(link);
  if (ring == NULL) {
    return XYZM_STATUS_UNSUPPORTED;
  }
  if (len > (size_t)UINT_MAX) {
    return XYZM_STATUS_IO_ERROR;
  }

  struct io_uring_sqe *request_sqe = io_uring_get_sqe(ring);
  struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(ring);
  if (request_sqe == NULL || timeout_sqe == NULL) {
    minizm_posix_fd_link_disable_io_uring(link);
    return XYZM_STATUS_UNSUPPORTED;
  }

  memset(request_sqe, 0, sizeof(*request_sqe));
  memset(timeout_sqe, 0, sizeof(*timeout_sqe));
  if (send_mode) {
    io_uring_prep_write(request_sqe, fd, buf, (unsigned)len, 0u);
  } else {
    io_uring_prep_read(request_sqe, fd, buf, (unsigned)len, 0u);
  }
  request_sqe->flags |= IOSQE_IO_LINK;
  request_sqe->user_data = MINIZM_POSIX_IO_URING_REQUEST_USER_DATA;

  struct __kernel_timespec timeout;
  timeout.tv_sec = (__kernel_time64_t)(timeout_ms / 1000u);
  timeout.tv_nsec = (long long)(timeout_ms % 1000u) * 1000000LL;
  io_uring_prep_link_timeout(timeout_sqe, &timeout, 0u);
  timeout_sqe->user_data = MINIZM_POSIX_IO_URING_TIMEOUT_USER_DATA;

  int rc = io_uring_submit_and_wait(ring, 2u);
  if (rc < 0) {
    minizm_posix_fd_link_disable_io_uring(link);
    return XYZM_STATUS_UNSUPPORTED;
  }

  int request_result = -ECANCELED;
  int timeout_result = -ECANCELED;
  unsigned completions_seen = 0u;
  while (completions_seen < 2u) {
    struct io_uring_cqe *cqe;
    rc = io_uring_wait_cqe(ring, &cqe);
    if (rc < 0) {
      minizm_posix_fd_link_disable_io_uring(link);
      return XYZM_STATUS_UNSUPPORTED;
    }

    if (cqe->user_data == MINIZM_POSIX_IO_URING_REQUEST_USER_DATA) {
      request_result = cqe->res;
    } else if (cqe->user_data == MINIZM_POSIX_IO_URING_TIMEOUT_USER_DATA) {
      timeout_result = cqe->res;
    }
    io_uring_cqe_seen(ring, cqe);
    ++completions_seen;
  }

  if (timeout_result == -ETIME) {
    return XYZM_STATUS_TIMEOUT;
  }
  if (request_result <= 0) {
    return XYZM_STATUS_IO_ERROR;
  }

  *transferred_len = (size_t)request_result;
  return XYZM_STATUS_OK;
}
#endif

/**
 * @brief Writes bytes to the configured POSIX transport descriptors.
 *
 * @param link POSIX file-descriptor transport state.
 * @param buf Bytes to write.
 * @param len Requested byte count.
 * @param timeout_ms Maximum time to wait for progress.
 * @param written_len Output byte count written during this call.
 * @return Transport status for the write.
 */
static xyzm_status_t minizm_posix_fd_send(
    minizm_posix_fd_link_t *link,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  *written_len = 0u;

  xyzm_status_t status;
#if defined(__linux__)
  status = minizm_posix_fd_io_uring(
      link,
      link->write_fd,
      true,
      (uint8_t *)buf,
      len,
      timeout_ms,
      written_len);
  if (status != XYZM_STATUS_UNSUPPORTED) {
    return status;
  }
#endif
  status = minizm_posix_wait_fd_ready(link->write_fd, POLLOUT, timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  ssize_t rc;
  do {
    rc = write(link->write_fd, buf, len);
  } while (rc < 0 && errno == EINTR);

  if (rc <= 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  *written_len = (size_t)rc;
  return XYZM_STATUS_OK;
}

/**
 * @brief Reads bytes from the configured POSIX transport descriptors.
 *
 * @param link POSIX file-descriptor transport state.
 * @param buf Destination buffer.
 * @param capacity Maximum number of bytes to read.
 * @param timeout_ms Maximum time to wait for progress.
 * @param read_len Output byte count read during this call.
 * @return Transport status for the read.
 */
static xyzm_status_t minizm_posix_fd_recv(
    minizm_posix_fd_link_t *link,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  *read_len = 0u;

  xyzm_status_t status;
#if defined(__linux__)
  status = minizm_posix_fd_io_uring(
      link,
      link->read_fd,
      false,
      buf,
      capacity,
      timeout_ms,
      read_len);
  if (status != XYZM_STATUS_UNSUPPORTED) {
    return status;
  }
#endif
  status = minizm_posix_wait_fd_ready(link->read_fd, POLLIN, timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  ssize_t rc;
  do {
    rc = read(link->read_fd, buf, capacity);
  } while (rc < 0 && errno == EINTR);

  if (rc < 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  if (rc == 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  *read_len = (size_t)rc;
  return XYZM_STATUS_OK;
}

/**
 * @brief Returns the last path component from a POSIX path string.
 *
 * @param path Path string to inspect.
 * @return Pointer to the last path component within @p path.
 */
static const char *minizm_posix_path_leaf(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

/**
 * @brief Chooses the receive-side file name from protocol metadata.
 *
 * @param info Protocol metadata for the incoming file.
 * @param fallback_name Name used when the metadata omits one.
 * @return Chosen file name.
 */
static const char *minizm_posix_received_name_from_info(
    const xyzm_file_info_t *info,
    const char *fallback_name) {
  const char *name = (fallback_name != NULL && fallback_name[0] != '\0') ?
      fallback_name :
      "received.bin";
  if ((info->valid_mask & XYZM_FILE_INFO_NAME) == 0u ||
      info->name == NULL ||
      info->name[0] == '\0') {
    return name;
  }

  name = minizm_posix_path_leaf(info->name);
  return name[0] != '\0' ? name : "received.bin";
}

/**
 * @brief Enumerates the single local file served by the POSIX source helper.
 *
 * @param state POSIX source state.
 * @param info Output metadata for the file.
 * @param file_opaque Output per-file handle returned to later callbacks.
 * @return Source status for the enumeration step.
 */
static xyzm_status_t minizm_posix_file_source_next(
    minizm_posix_file_source_t *state,
    xyzm_file_info_t *info,
    void **file_opaque) {
  if (state->used) {
    return XYZM_STATUS_END;
  }

  int fd = open(state->path, O_RDONLY);
  if (fd < 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return XYZM_STATUS_IO_ERROR;
  }

  state->fd = fd;
  state->info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  state->info.name = minizm_posix_path_leaf(state->path);
  state->info.size_bytes = (uint64_t)st.st_size;
  state->info.mtime_unix_seconds = 0u;
  state->info.mode = 0u;

  *info = state->info;
  *file_opaque = state;
  state->used = true;
  return XYZM_STATUS_OK;
}

/**
 * @brief Reads payload bytes from the active POSIX source file.
 *
 * @param state POSIX source state.
 * @param buf Destination buffer.
 * @param capacity Maximum number of bytes to read.
 * @param read_len Output byte count read during this call.
 * @return Source status for the read.
 */
static xyzm_status_t minizm_posix_file_source_read(
    minizm_posix_file_source_t *state,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  *read_len = 0u;

  ssize_t rc;
  do {
    rc = read(state->fd, buf, capacity);
  } while (rc < 0 && errno == EINTR);

  if (rc < 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  if (rc == 0) {
    return XYZM_STATUS_END;
  }
  *read_len = (size_t)rc;
  return XYZM_STATUS_OK;
}

static xyzm_status_t minizm_posix_file_source_seek(
    minizm_posix_file_source_t *state,
    uint64_t offset) {
  if (offset > (uint64_t)LONG_MAX) {
    return XYZM_STATUS_IO_ERROR;
  }
  return lseek(state->fd, (off_t)(long)offset, SEEK_SET) < 0 ?
      XYZM_STATUS_IO_ERROR :
      XYZM_STATUS_OK;
}

/**
 * @brief Finalizes the active POSIX source file.
 *
 * @param state POSIX source state.
 * @return Always returns @ref XYZM_STATUS_OK.
 */
static xyzm_status_t minizm_posix_file_source_end(minizm_posix_file_source_t *state) {
  if (state->fd >= 0) {
    close(state->fd);
    state->fd = -1;
  }
  return XYZM_STATUS_OK;
}

static int minizm_posix_write_all(int fd, const uint8_t *buf, size_t len) {
  size_t offset = 0u;
  while (offset < len) {
    ssize_t rc;
    do {
      rc = write(fd, buf + offset, len - offset);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
      return -1;
    }
    offset += (size_t)rc;
  }
  return 0;
}

/**
 * @brief Opens the destination file announced by the protocol metadata.
 *
 * @param state POSIX sink state.
 * @param info Metadata announced for the file.
 * @param file_opaque Output per-file handle returned to later callbacks.
 * @return Sink status for the begin step.
 */
static xyzm_status_t minizm_posix_file_sink_begin(
    minizm_posix_file_sink_t *state,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque) {
  state->fd = -1;
  if (resume_offset != NULL) {
    *resume_offset = 0u;
  }
  memset(state->path, 0, sizeof(state->path));
  int rc = snprintf(
      state->path,
      sizeof(state->path),
      "%s/%s",
      state->output_dir,
      minizm_posix_received_name_from_info(info, state->fallback_name));
  if (rc < 0 || (size_t)rc >= sizeof(state->path)) {
    return XYZM_STATUS_UNSUPPORTED;
  }

  state->fd = open(state->path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (state->fd < 0) {
    return XYZM_STATUS_IO_ERROR;
  }

  *file_opaque = state;
  return XYZM_STATUS_OK;
}

/**
 * @brief Writes payload bytes into the active POSIX sink file.
 *
 * @param state POSIX sink state.
 * @param buf Payload bytes to write.
 * @param len Number of payload bytes in @p buf.
 * @return Sink status for the write step.
 */
static xyzm_status_t minizm_posix_file_sink_write(
    minizm_posix_file_sink_t *state,
    const uint8_t *buf,
    size_t len) {
  return minizm_posix_write_all(state->fd, buf, len) == 0 ?
      XYZM_STATUS_OK :
      XYZM_STATUS_IO_ERROR;
}

/**
 * @brief Finalizes the active POSIX sink file and optionally removes partial output.
 *
 * @param state POSIX sink state.
 * @param result Final per-file transfer result.
 * @return Always returns @ref XYZM_STATUS_OK.
 */
static xyzm_status_t minizm_posix_file_sink_end(
    minizm_posix_file_sink_t *state,
    xyzm_status_t result) {
  if (state->fd >= 0) {
    close(state->fd);
    state->fd = -1;
  }
  if (state->remove_partial_on_error && result != XYZM_STATUS_OK) {
    (void)unlink(state->path);
  }
  return XYZM_STATUS_OK;
}

static xyzm_status_t minizm_posix_link_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  return minizm_posix_fd_send(opaque, buf, len, timeout_ms, written_len);
}

static xyzm_status_t minizm_posix_link_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  return minizm_posix_fd_recv(opaque, buf, capacity, timeout_ms, read_len);
}

static uint64_t minizm_posix_link_now_ms(void *opaque) {
  (void)opaque;
  return minizm_posix_monotonic_now_ms();
}

static xyzm_status_t minizm_posix_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  return minizm_posix_file_source_next(opaque, info, file_opaque);
}

static xyzm_status_t minizm_posix_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  (void)opaque;
  return minizm_posix_file_source_read(file_opaque, buf, capacity, read_len);
}

static xyzm_status_t minizm_posix_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset) {
  (void)opaque;
  return minizm_posix_file_source_seek(file_opaque, offset);
}

static xyzm_status_t minizm_posix_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  (void)opaque;
  (void)info;
  (void)result;
  return minizm_posix_file_source_end(file_opaque);
}

static xyzm_status_t minizm_posix_sink_begin(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque) {
  return minizm_posix_file_sink_begin(opaque, info, resume_offset, file_opaque);
}

static xyzm_status_t minizm_posix_sink_write(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len) {
  (void)opaque;
  return minizm_posix_file_sink_write(file_opaque, buf, len);
}

static xyzm_status_t minizm_posix_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  (void)opaque;
  (void)info;
  return minizm_posix_file_sink_end(file_opaque, result);
}

int minizm_posix_tty_guard_enable_raw(
    minizm_posix_tty_guard_t *guard,
    int fd) {
  memset(guard, 0, sizeof(*guard));
  if (!isatty(fd)) {
    return 0;
  }
  if (tcgetattr(fd, &guard->saved) != 0) {
    return -1;
  }

  struct termios raw = guard->saved;
  raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= (tcflag_t)~(OPOST);
  raw.c_cflag |= (tcflag_t)CS8;
  raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &raw) != 0) {
    return -1;
  }
  guard->active = true;
  guard->fd = fd;
  return 0;
}

void minizm_posix_tty_guard_restore(minizm_posix_tty_guard_t *guard) {
  if (!guard->active) {
    return;
  }
  (void)tcsetattr(guard->fd, TCSANOW, &guard->saved);
  guard->active = false;
}

void minizm_posix_init_fd_link(
    xyzm_link_ops_t *link,
    minizm_posix_fd_link_t *state,
    int read_fd,
    int write_fd) {
  memset(state, 0, sizeof(*state));
  state->read_fd = read_fd;
  state->write_fd = write_fd;
  link->opaque = state;
  link->send = minizm_posix_link_send;
  link->recv = minizm_posix_link_recv;
  link->now_ms = minizm_posix_link_now_ms;
}

void minizm_posix_init_stdio_link(
    xyzm_link_ops_t *link,
    minizm_posix_fd_link_t *state) {
  minizm_posix_init_fd_link(link, state, STDIN_FILENO, STDOUT_FILENO);
}

void minizm_posix_cleanup_fd_link(minizm_posix_fd_link_t *state) {
#if defined(__linux__)
  minizm_posix_fd_link_disable_io_uring(state);
#else
  (void)state;
#endif
}

void minizm_posix_init_file_source(
    xyzm_source_ops_t *source,
    minizm_posix_file_source_t *state,
    const char *path) {
  memset(state, 0, sizeof(*state));
  state->path = path;
  state->fd = -1;
  source->opaque = state;
  source->next = minizm_posix_source_next;
  source->read = minizm_posix_source_read;
  source->seek = minizm_posix_source_seek;
  source->end = minizm_posix_source_end;
}

void minizm_posix_init_file_sink(
    xyzm_sink_ops_t *sink,
    minizm_posix_file_sink_t *state,
    const char *output_dir,
    const char *fallback_name,
    bool remove_partial_on_error) {
  memset(state, 0, sizeof(*state));
  state->output_dir = output_dir;
  state->fallback_name = fallback_name;
  state->remove_partial_on_error = remove_partial_on_error;
  state->fd = -1;
  sink->opaque = state;
  sink->begin = minizm_posix_sink_begin;
  sink->write = minizm_posix_sink_write;
  sink->end = minizm_posix_sink_end;
}
