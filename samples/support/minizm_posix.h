/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef MINIZM_POSIX_H
#define MINIZM_POSIX_H

#include "libxyzm/sync.h"

#include <stdbool.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Saves and restores terminal raw-mode changes for sample byte streams.
 */
typedef struct minizm_posix_tty_guard {
  bool active;             /**< `true` when the guard changed terminal state. */
  int fd;                  /**< File descriptor whose terminal state was changed. */
  struct termios saved;    /**< Original terminal settings restored on cleanup. */
} minizm_posix_tty_guard_t;

/**
 * @brief File-descriptor-backed transport state for sample transfers.
 */
typedef struct minizm_posix_fd_link {
  int read_fd;   /**< File descriptor used for reads. */
  int write_fd;  /**< File descriptor used for writes. */
#if defined(__linux__)
  bool io_uring_initialized;   /**< Tracks whether the Linux io_uring backend was probed. */
  bool io_uring_enabled;       /**< `true` when the Linux io_uring backend is active. */
  void *io_uring_handle;       /**< Opaque `struct io_uring *` owned by the helper. */
#endif
} minizm_posix_fd_link_t;

/**
 * @brief Single-file send context backed by POSIX file descriptors.
 */
typedef struct minizm_posix_file_source {
  const char *path;        /**< Path opened when the source is enumerated. */
  bool used;               /**< Tracks whether the single file was already served. */
  int fd;                  /**< Open file descriptor for the active file. */
  xyzm_file_info_t info;   /**< Metadata announced for the active file. */
} minizm_posix_file_source_t;

/**
 * @brief Single-file receive context backed by POSIX file descriptors.
 */
typedef struct minizm_posix_file_sink {
  const char *output_dir;            /**< Destination directory for received files. */
  const char *fallback_name;         /**< Name used when metadata omits a filename. */
  bool remove_partial_on_error;      /**< Removes the destination file when the transfer fails. */
  int fd;                            /**< Open file descriptor for the active file. */
  char path[1024];                   /**< Full path of the active destination file. */
} minizm_posix_file_sink_t;

/**
 * @brief Enables raw mode on a terminal file descriptor when one is attached.
 *
 * @param guard Output guard state restored by @ref minizm_posix_tty_guard_restore.
 * @param fd Terminal file descriptor to configure.
 * @return Zero when the terminal is ready for raw byte transfers.
 * @retval -1 The terminal state could not be queried or changed.
 * @remarks Non-terminal file descriptors are accepted and treated as a no-op.
 */
int minizm_posix_tty_guard_enable_raw(
    minizm_posix_tty_guard_t *guard,
    int fd);

/**
 * @brief Restores terminal settings saved by @ref minizm_posix_tty_guard_enable_raw.
 *
 * @param guard Guard state previously initialized by
 *              @ref minizm_posix_tty_guard_enable_raw.
 */
void minizm_posix_tty_guard_restore(minizm_posix_tty_guard_t *guard);

/**
 * @brief Initializes synchronous link callbacks for arbitrary POSIX file descriptors.
 *
 * @param link Output transport callback table.
 * @param state Output file-descriptor state owned by the caller.
 * @param read_fd File descriptor used for reads.
 * @param write_fd File descriptor used for writes.
 */
void minizm_posix_init_fd_link(
    xyzm_link_ops_t *link,
    minizm_posix_fd_link_t *state,
    int read_fd,
    int write_fd);

/**
 * @brief Initializes synchronous link callbacks for `stdin` and `stdout`.
 *
 * @param link Output transport callback table.
 * @param state Output file-descriptor state owned by the caller.
 */
void minizm_posix_init_stdio_link(
    xyzm_link_ops_t *link,
    minizm_posix_fd_link_t *state);

/**
 * @brief Releases resources held by @ref minizm_posix_init_fd_link.
 *
 * @param state File-descriptor state previously initialized by
 *              @ref minizm_posix_init_fd_link.
 * @remarks On Linux this closes the helper-owned `io_uring` instance when one
 *          was created. Other platforms treat this as a no-op.
 */
void minizm_posix_cleanup_fd_link(minizm_posix_fd_link_t *state);

/**
 * @brief Initializes a single-file synchronous source backed by `open` and `read`.
 *
 * @param source Output source callback table.
 * @param state Output source state owned by the caller.
 * @param path Path of the file to send.
 */
void minizm_posix_init_file_source(
    xyzm_source_ops_t *source,
    minizm_posix_file_source_t *state,
    const char *path);

/**
 * @brief Initializes a single-file synchronous sink backed by `open` and `write`.
 *
 * @param sink Output sink callback table.
 * @param state Output sink state owned by the caller.
 * @param output_dir Directory that will receive the file.
 * @param fallback_name File name used when the protocol metadata omits one.
 * @param remove_partial_on_error Removes the partially written file on failure when non-zero.
 */
void minizm_posix_init_file_sink(
    xyzm_sink_ops_t *sink,
    minizm_posix_file_sink_t *state,
    const char *output_dir,
    const char *fallback_name,
    bool remove_partial_on_error);

#ifdef __cplusplus
}
#endif

#endif
