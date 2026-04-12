/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#include "libxyzm/xyzm.h"

/**
 * @brief Synchronous stdio transport state used by the custom transport sample.
 *
 * @remarks The sample keeps only the read and write file descriptors here so
 *          the relationship between `xyzm_link_ops_t` and the backing state is
 *          easy to compare with the default helper-based sample.
 */
typedef struct minizm_sync_transport {
  int read_fd;   /**< File descriptor used for reads. */
  int write_fd;  /**< File descriptor used for writes. */
} minizm_sync_transport_t;

/**
 * @brief Initializes custom synchronous transport callbacks for `stdin` and `stdout`.
 *
 * @param link Output transport callback table.
 * @param transport Output transport state owned by the caller.
 *
 * @remarks The callback table receives a custom `send`, `recv`, and `now_ms`
 *          implementation, while the sample still relies on default file
 *          source/sink helpers for local storage.
 */
void minizm_sync_transport_init_stdio_link(
    xyzm_link_ops_t *link,
    minizm_sync_transport_t *transport);
