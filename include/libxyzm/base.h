/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef XYZM_BASE_H
#define XYZM_BASE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief XMODEM checksum modes supported by the dedicated XMODEM entry points.
 */
typedef enum xyzm_xmodem_checksum_mode {
  XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM = 1, /**< Use the legacy 8-bit checksum trailer. */
  XYZM_XMODEM_CHECKSUM_MODE_CRC = 2,      /**< Use the 16-bit CRC trailer. */
  XYZM_XMODEM_CHECKSUM_MODE_AUTO = 3      /**< Let the send path choose checksum or CRC from the peer handshake byte. */
} xyzm_xmodem_checksum_mode_t;

/**
 * @brief XMODEM payload block sizes supported by the dedicated XMODEM entry points.
 */
typedef enum xyzm_xmodem_packet_size {
  XYZM_XMODEM_PACKET_SIZE_128 = 1, /**< Use the classic 128-byte SOH frame size. */
  XYZM_XMODEM_PACKET_SIZE_1K = 2   /**< Prefer 1024-byte STX frames and fall back to a trailing 128-byte block when needed. */
} xyzm_xmodem_packet_size_t;

/**
 * @brief YMODEM wire variants supported by the dedicated YMODEM entry points.
 */
typedef enum xyzm_ymodem_variant {
  XYZM_YMODEM_VARIANT_STANDARD = 1, /**< Classic YMODEM with ACK/NAK handshakes. */
  XYZM_YMODEM_VARIANT_G = 2,        /**< Streaming YMODEM-g without block retries. */
  XYZM_YMODEM_VARIANT_AUTO = 3      /**< Let the send path choose standard or YMODEM-g from the peer handshake byte. */
} xyzm_ymodem_variant_t;

/**
 * @brief XMODEM send settings shared by the dedicated XMODEM send entry points.
 */
typedef struct xyzm_xmodem_send_opts {
  uint32_t handshake_timeout_ms;  /**< Timeout used while waiting for start-of-transfer events. */
  uint32_t block_timeout_ms;      /**< Timeout used for per-block send and receive progress. */
  uint32_t retry_limit;           /**< Maximum retry count before reporting failure. */
  uint8_t pad_byte;               /**< Padding byte used when a block is only partially filled. */
  xyzm_xmodem_checksum_mode_t checksum_mode; /**< Checksum behavior selected for this transfer. */
  xyzm_xmodem_packet_size_t packet_size;     /**< Outbound payload block size preferred for this transfer. */
} xyzm_xmodem_send_opts_t;

/**
 * @brief XMODEM receive settings shared by the dedicated XMODEM receive entry points.
 */
typedef struct xyzm_xmodem_receive_opts {
  uint32_t handshake_timeout_ms;  /**< Timeout used while waiting for start-of-transfer events. */
  uint32_t block_timeout_ms;      /**< Timeout used for per-block send and receive progress. */
  uint32_t retry_limit;           /**< Maximum retry count before reporting failure. */
  uint8_t pad_byte;               /**< Padding byte used when a block is only partially filled. */
  xyzm_xmodem_checksum_mode_t checksum_mode; /**< Checksum behavior requested from the peer for this transfer. */
} xyzm_xmodem_receive_opts_t;

/**
 * @brief YMODEM transfer settings shared by the dedicated YMODEM entry points.
 *
 * @remarks The send entry points accept @ref XYZM_YMODEM_VARIANT_AUTO, while
 *          the receive entry points require an explicit non-auto variant.
 */
typedef struct xyzm_ymodem_opts {
  uint32_t handshake_timeout_ms;  /**< Timeout used while waiting for start-of-transfer events. */
  uint32_t block_timeout_ms;      /**< Timeout used for per-block send and receive progress. */
  uint32_t retry_limit;           /**< Maximum retry count before reporting failure. */
  uint8_t pad_byte;               /**< Padding byte used when a block is only partially filled. */
  xyzm_ymodem_variant_t variant;  /**< YMODEM wire behavior selected for this transfer. */
} xyzm_ymodem_opts_t;

/**
 * @brief ZMODEM transfer settings shared by the dedicated ZMODEM entry points.
 */
typedef struct xyzm_zmodem_opts {
  uint32_t handshake_timeout_ms;  /**< Timeout used while waiting for start-of-transfer events. */
  uint32_t block_timeout_ms;      /**< Timeout used for per-block send and receive progress. */
  uint32_t retry_limit;           /**< Maximum retry count before reporting failure. */
  uint8_t pad_byte;               /**< Padding byte used when a block is only partially filled. */
  uint16_t zmodem_packet_len;     /**< Preferred ZMODEM subpacket size in bytes. */
  uint8_t zmodem_escape_ctrl;     /**< Enables extra ZMODEM control-character escaping. */
  uint8_t zmodem_use_crc32;       /**< Prefers 32-bit ZMODEM FCS when non-zero. */
} xyzm_zmodem_opts_t;

/**
 * @brief Protocol family reported by transfer notification events.
 */
typedef enum xyzm_transfer_protocol {
  XYZM_TRANSFER_PROTOCOL_XMODEM = 1, /**< XMODEM-family transfer. */
  XYZM_TRANSFER_PROTOCOL_YMODEM = 2, /**< YMODEM-family transfer. */
  XYZM_TRANSFER_PROTOCOL_ZMODEM = 3  /**< ZMODEM-family transfer. */
} xyzm_transfer_protocol_t;

/**
 * @brief Transfer direction reported by transfer notification events.
 */
typedef enum xyzm_transfer_direction {
  XYZM_TRANSFER_DIRECTION_SEND = 1,    /**< Local-to-remote transfer. */
  XYZM_TRANSFER_DIRECTION_RECEIVE = 2  /**< Remote-to-local transfer. */
} xyzm_transfer_direction_t;

/**
 * @brief Transfer notification event kind.
 */
typedef enum xyzm_transfer_event_kind {
  XYZM_TRANSFER_EVENT_TRANSFER_STARTED = 1,  /**< The validated transfer attempt started. */
  XYZM_TRANSFER_EVENT_FILE_STARTED = 2,      /**< One file started. */
  XYZM_TRANSFER_EVENT_PROGRESS = 3,          /**< Payload bytes were accepted by the protocol. */
  XYZM_TRANSFER_EVENT_RETRY = 4,             /**< The protocol consumed one retry. */
  XYZM_TRANSFER_EVENT_FILE_FINISHED = 5,     /**< One file reached a terminal status. */
  XYZM_TRANSFER_EVENT_TRANSFER_FINISHED = 6  /**< The whole transfer reached a terminal status. */
} xyzm_transfer_event_kind_t;

#ifdef __cplusplus
}
#endif

#endif
