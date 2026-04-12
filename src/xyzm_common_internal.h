/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef XYZM_COMMON_INTERNAL_H
#define XYZM_COMMON_INTERNAL_H

#include "libxyzm/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  XYZM_SOH = 0x01,
  XYZM_STX = 0x02,
  XYZM_EOT = 0x04,
  XYZM_ACK = 0x06,
  XYZM_NAK = 0x15,
  XYZM_CAN = 0x18,
  XYZM_CRC_REQ = 'C',
  XYZM_G_REQ = 'G',
};

typedef struct xyzm_runtime_options {
  uint32_t handshake_timeout_ms;
  uint32_t block_timeout_ms;
  uint32_t retry_limit;
  uint8_t pad_byte;
  xyzm_xmodem_checksum_mode_t xmodem_checksum_mode;
  xyzm_xmodem_packet_size_t xmodem_packet_size;
  xyzm_ymodem_variant_t ymodem_variant;
  uint16_t zmodem_packet_len;
  uint8_t zmodem_escape_ctrl;
  uint8_t zmodem_use_crc32;
  bool batch_mode;
} xyzm_runtime_options_t;

extern const uint16_t xyzm_crc16_xmodem_table[256];
extern const uint32_t xyzm_crc32_zmodem_table[256];

#ifdef __cplusplus
}
#endif

static inline uint8_t xyzm_checksum8_update(uint8_t checksum, uint8_t value) {
  return (uint8_t)(((uint32_t)checksum + value) & 0xffu);
}

static inline uint8_t xyzm_checksum8_update_buffer(
    uint8_t checksum,
    const uint8_t *buf,
    size_t len) {
  for (size_t index = 0u; index < len; ++index) {
    checksum = xyzm_checksum8_update(checksum, buf[index]);
  }
  return checksum;
}

static inline uint16_t xyzm_crc16_xmodem_update(uint16_t crc, uint8_t value) {
  return (uint16_t)(
      (uint16_t)(crc << 8) ^
      xyzm_crc16_xmodem_table[((crc >> 8) ^ value) & 0xffu]);
}

static inline uint16_t xyzm_crc16_xmodem_update_buffer(
    uint16_t crc,
    const uint8_t *buf,
    size_t len) {
  for (size_t index = 0u; index < len; ++index) {
    crc = xyzm_crc16_xmodem_update(crc, buf[index]);
  }
  return crc;
}

static inline uint32_t xyzm_crc32_zmodem_update(uint32_t crc, uint8_t value) {
  return (crc >> 8) ^ xyzm_crc32_zmodem_table[(crc ^ value) & 0xffu];
}

static inline uint32_t xyzm_crc32_zmodem_update_buffer(
    uint32_t crc,
    const uint8_t *buf,
    size_t len) {
  for (size_t index = 0u; index < len; ++index) {
    crc = xyzm_crc32_zmodem_update(crc, buf[index]);
  }
  return crc;
}

#endif
