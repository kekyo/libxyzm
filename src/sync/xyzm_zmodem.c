/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_sync_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Keeps the private wire constants local to the ZMODEM implementation.
 */
enum {
  XYZM_ZMODEM_WIRE_BATCH_CAPACITY = 1024,
  XYZM_ZPAD = '*',
  XYZM_ZDLE = 0x18,
  XYZM_ZBIN = 'A',
  XYZM_ZBIN32 = 'C',
  XYZM_ZHEX = 'B',
  XYZM_ZRQINIT = 0,
  XYZM_ZRINIT = 1,
  XYZM_ZACK = 3,
  XYZM_ZFILE = 4,
  XYZM_ZFIN = 8,
  XYZM_ZRPOS = 9,
  XYZM_ZDATA = 10,
  XYZM_ZEOF = 11,
  XYZM_ZSKIP = 5,
  XYZM_ZCRCE = 'h',
  XYZM_ZCRCG = 'i',
  XYZM_ZCRCQ = 'j',
  XYZM_ZCRCW = 'k',
  XYZM_ZRUB0 = 'l',
  XYZM_ZRUB1 = 'm',
  XYZM_GOTCRCE = 0x400 | XYZM_ZCRCE,
  XYZM_GOTCRCG = 0x400 | XYZM_ZCRCG,
  XYZM_GOTCRCQ = 0x400 | XYZM_ZCRCQ,
  XYZM_GOTCRCW = 0x400 | XYZM_ZCRCW,
  XYZM_CANFDX = 0x01,
  XYZM_CANOVIO = 0x02,
  XYZM_CANFC32 = 0x20,
  XYZM_ESCCTL = 0x40,
  XYZM_ZCBIN = 1,
  XYZM_XON = 0x11,
  XYZM_XOFF = 0x13,
};

/**
 * @brief Tracks the last emitted wire byte so contextual escape rules stay correct.
 */
typedef struct xyzm_zmodem_send_state {
  uint8_t last_wire_byte;
  bool has_last_wire_byte;
} xyzm_zmodem_send_state_t;

typedef struct xyzm_zmodem_wire_batch {
  uint8_t bytes[XYZM_ZMODEM_WIRE_BATCH_CAPACITY];
  size_t len;
} xyzm_zmodem_wire_batch_t;

typedef struct xyzm_zmodem_pending_header {
  uint8_t type;
  uint8_t hdr[4];
  bool available;
} xyzm_zmodem_pending_header_t;

typedef struct xyzm_zmodem_control_probe {
  uint16_t parse_state;
  uint16_t decode_state;
  size_t parse_index;
  size_t trailer_len;
  size_t expected_trailer_len;
  uint32_t crc32;
  uint16_t crc16;
  uint8_t type;
  uint8_t frame_kind;
  uint8_t hdr[4];
  uint8_t digits[14];
} xyzm_zmodem_control_probe_t;

enum {
  XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1 = 1,
  XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD2 = 2,
  XYZM_ZMODEM_PARSE_HEADER_SEEK_ZDLE = 3,
  XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND = 4,
  XYZM_ZMODEM_PARSE_HEADER_BINARY16 = 5,
  XYZM_ZMODEM_PARSE_HEADER_BINARY32 = 6,
  XYZM_ZMODEM_PARSE_HEADER_HEX_DIGIT = 7,
  XYZM_ZMODEM_PARSE_HEADER_HEX_TRAILER = 8
};

enum {
  XYZM_ZMODEM_DECODE_FIRST = 1,
  XYZM_ZMODEM_DECODE_SECOND = 2
};

static inline uint32_t xyzm_zcrc32_update(uint32_t crc, uint8_t value) {
  return xyzm_crc32_zmodem_update(crc, value);
}

/**
 * @brief Stores a 32-bit header value in ZMODEM's little-endian wire layout.
 *
 * @param value Header value to serialize.
 * @param hdr Output 4-byte header buffer.
 */
static inline void xyzm_zmodem_stohdr(uint32_t value, uint8_t hdr[4]) {
  hdr[0] = (uint8_t)(value & 0xffu);
  hdr[1] = (uint8_t)((value >> 8) & 0xffu);
  hdr[2] = (uint8_t)((value >> 16) & 0xffu);
  hdr[3] = (uint8_t)((value >> 24) & 0xffu);
}

/**
 * @brief Reconstructs a 32-bit header value from little-endian wire bytes.
 *
 * @param hdr Serialized 4-byte header.
 * @return Decoded 32-bit header value.
 */
static inline uint32_t xyzm_zmodem_rclhdr(const uint8_t hdr[4]) {
  return ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[2] << 16) |
      ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[0];
}

/**
 * @brief Converts a nibble into the lowercase hexadecimal digit used by ZHEX frames.
 *
 * @param value Nibble value in the range 0..15.
 * @return Lowercase hexadecimal digit for @p value.
 */
static inline uint8_t xyzm_hex_digit(unsigned value) {
  return (uint8_t)(value < 10u ? ('0' + value) : ('a' + (value - 10u)));
}

/**
 * @brief Computes one CRC16 lookup entry on demand instead of carrying a static table.
 *
 * @param index Lookup index to expand.
 * @return CRC16 table entry for @p index.
 */
static inline uint16_t xyzm_zmodem_crc16_entry(uint8_t index) {
  uint16_t crc = (uint16_t)index << 8;
  for (unsigned bit = 0u; bit < 8u; ++bit) {
    if ((crc & 0x8000u) != 0u) {
      crc = (uint16_t)((crc << 1) ^ 0x1021u);
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

/**
 * @brief Updates the CRC16 variant used by ZMODEM binary16 framing.
 *
 * @param crc Previous CRC16 state.
 * @param value Next byte to fold into the CRC.
 * @return Updated CRC16 state.
 */
static inline uint16_t xyzm_zmodem_crc16_update(uint16_t crc, uint8_t value) {
  return (uint16_t)(
      xyzm_zmodem_crc16_entry((uint8_t)(crc >> 8)) ^
      (uint16_t)(crc << 8) ^
      value);
}

/**
 * @brief Computes the CRC16 trailer for a ZMODEM header, including the required zeros.
 *
 * @param type Header type byte.
 * @param hdr 4-byte header payload.
 * @return CRC16 value that should follow the header.
 */
static inline uint16_t xyzm_zmodem_crc16_header(uint8_t type, const uint8_t hdr[4]) {
  uint16_t crc = xyzm_zmodem_crc16_update(0u, (uint8_t)(type & 0x7fu));
  for (size_t index = 0u; index < 4u; ++index) {
    crc = xyzm_zmodem_crc16_update(crc, hdr[index]);
  }
  crc = xyzm_zmodem_crc16_update(crc, 0u);
  crc = xyzm_zmodem_crc16_update(crc, 0u);
  return crc;
}

/**
 * @brief Decodes one hexadecimal digit or reports failure with -1.
 *
 * @param value ASCII byte to decode.
 * @return Decoded nibble value in the range 0..15.
 * @return `-1` when @p value is not a hexadecimal digit.
 */
static inline int xyzm_from_hex(uint8_t value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return 10 + (value - 'a');
  }
  if (value >= 'A' && value <= 'F') {
    return 10 + (value - 'A');
  }
  return -1;
}

static inline bool xyzm_zmodem_is_flow_control(uint8_t value) {
  return (value == XYZM_XON) ||
      (value == XYZM_XOFF) ||
      (value == (uint8_t)(XYZM_XON | 0x80u)) ||
      (value == (uint8_t)(XYZM_XOFF | 0x80u));
}

static inline void xyzm_zmodem_control_probe_init(
    xyzm_zmodem_control_probe_t *probe) {
  memset(probe, 0, sizeof(*probe));
  probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
  probe->decode_state = XYZM_ZMODEM_DECODE_FIRST;
}

static inline bool xyzm_zmodem_control_decode_zdle(
    xyzm_zmodem_control_probe_t *probe,
    uint8_t raw,
    int *value) {
  if (xyzm_zmodem_is_flow_control(raw)) {
    return false;
  }
  if (probe->decode_state == XYZM_ZMODEM_DECODE_FIRST) {
    if (raw != XYZM_ZDLE) {
      *value = raw;
      return true;
    }
    probe->decode_state = XYZM_ZMODEM_DECODE_SECOND;
    return false;
  }

  probe->decode_state = XYZM_ZMODEM_DECODE_FIRST;
  if (raw == XYZM_ZRUB0) {
    *value = 0x7f;
  } else if (raw == XYZM_ZRUB1) {
    *value = 0xff;
  } else if (raw == XYZM_ZCRCE) {
    *value = XYZM_GOTCRCE;
  } else if (raw == XYZM_ZCRCG) {
    *value = XYZM_GOTCRCG;
  } else if (raw == XYZM_ZCRCQ) {
    *value = XYZM_GOTCRCQ;
  } else if (raw == XYZM_ZCRCW) {
    *value = XYZM_GOTCRCW;
  } else {
    *value = raw ^ 0x40u;
  }
  return true;
}

static inline void xyzm_zmodem_control_header_ready(
    xyzm_zmodem_control_probe_t *probe,
    uint8_t *type,
    uint8_t hdr[4]) {
  *type = probe->type;
  memcpy(hdr, probe->hdr, 4u);
  xyzm_zmodem_control_probe_init(probe);
}

static inline xyzm_status_t xyzm_zmodem_control_probe_feed(
    xyzm_zmodem_control_probe_t *probe,
    uint8_t raw,
    bool *header_ready,
    uint8_t *type,
    uint8_t hdr[4]) {
  *header_ready = false;

  switch (probe->parse_state) {
    case XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1:
      if (raw == XYZM_ZPAD) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD2;
      }
      return XYZM_STATUS_OK;

    case XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD2:
      if (raw == XYZM_ZPAD) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_ZDLE;
        return XYZM_STATUS_OK;
      }
      if (raw == XYZM_ZDLE) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND;
        return XYZM_STATUS_OK;
      }
      probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
      return XYZM_STATUS_OK;

    case XYZM_ZMODEM_PARSE_HEADER_SEEK_ZDLE:
      if (raw == XYZM_ZDLE) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND;
      } else {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
      }
      return XYZM_STATUS_OK;

    case XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND:
      probe->frame_kind = raw;
      probe->parse_index = 0u;
      if (raw == XYZM_ZBIN) {
        probe->crc16 = 0u;
        probe->decode_state = XYZM_ZMODEM_DECODE_FIRST;
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_BINARY16;
        return XYZM_STATUS_OK;
      }
      if (raw == XYZM_ZBIN32) {
        probe->crc32 = 0xffffffffu;
        probe->decode_state = XYZM_ZMODEM_DECODE_FIRST;
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_BINARY32;
        return XYZM_STATUS_OK;
      }
      if (raw == XYZM_ZHEX) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_HEX_DIGIT;
        return XYZM_STATUS_OK;
      }
      probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
      return XYZM_STATUS_OK;

    case XYZM_ZMODEM_PARSE_HEADER_BINARY16: {
      int decoded;
      if (!xyzm_zmodem_control_decode_zdle(probe, raw, &decoded)) {
        return XYZM_STATUS_OK;
      }
      if ((decoded & 0x400) != 0) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      uint8_t value = (uint8_t)decoded;
      if (probe->parse_index == 0u) {
        probe->type = value;
      } else if (probe->parse_index <= 4u) {
        probe->hdr[probe->parse_index - 1u] = value;
      }
      probe->crc16 = xyzm_zmodem_crc16_update(probe->crc16, value);
      probe->parse_index += 1u;
      if (probe->parse_index < 7u) {
        return XYZM_STATUS_OK;
      }
      if (probe->crc16 != 0u) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      *header_ready = true;
      xyzm_zmodem_control_header_ready(probe, type, hdr);
      return XYZM_STATUS_OK;
    }

    case XYZM_ZMODEM_PARSE_HEADER_BINARY32: {
      int decoded;
      if (!xyzm_zmodem_control_decode_zdle(probe, raw, &decoded)) {
        return XYZM_STATUS_OK;
      }
      if ((decoded & 0x400) != 0) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      uint8_t value = (uint8_t)decoded;
      if (probe->parse_index == 0u) {
        probe->type = value;
      } else if (probe->parse_index <= 4u) {
        probe->hdr[probe->parse_index - 1u] = value;
      }
      probe->crc32 = xyzm_zcrc32_update(probe->crc32, value);
      probe->parse_index += 1u;
      if (probe->parse_index < 9u) {
        return XYZM_STATUS_OK;
      }
      if (probe->crc32 != 0xdebb20e3u) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      *header_ready = true;
      xyzm_zmodem_control_header_ready(probe, type, hdr);
      return XYZM_STATUS_OK;
    }

    case XYZM_ZMODEM_PARSE_HEADER_HEX_DIGIT:
      if (xyzm_from_hex(raw) < 0) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      probe->digits[probe->parse_index++] = raw;
      if (probe->parse_index < 14u) {
        return XYZM_STATUS_OK;
      }
      probe->type = (uint8_t)(
          (xyzm_from_hex(probe->digits[0]) << 4) |
          xyzm_from_hex(probe->digits[1]));
      for (probe->parse_index = 0u; probe->parse_index < 4u; ++probe->parse_index) {
        probe->hdr[probe->parse_index] = (uint8_t)(
            (xyzm_from_hex(probe->digits[2u + probe->parse_index * 2u]) << 4) |
            xyzm_from_hex(probe->digits[3u + probe->parse_index * 2u]));
      }
      probe->crc16 = (uint16_t)(
          (xyzm_from_hex(probe->digits[10]) << 12) |
          (xyzm_from_hex(probe->digits[11]) << 8) |
          (xyzm_from_hex(probe->digits[12]) << 4) |
          xyzm_from_hex(probe->digits[13]));
      if (probe->crc16 != xyzm_zmodem_crc16_header(probe->type, probe->hdr)) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_HEX_TRAILER;
      probe->trailer_len = 0u;
      probe->expected_trailer_len =
          ((probe->type != XYZM_ZFIN) && (probe->type != XYZM_ZACK)) ? 3u : 2u;
      return XYZM_STATUS_OK;

    case XYZM_ZMODEM_PARSE_HEADER_HEX_TRAILER:
      if (probe->trailer_len == 0u) {
        if ((raw != '\r') && (raw != '\n')) {
          return XYZM_STATUS_PROTOCOL_ERROR;
        }
      } else if (probe->trailer_len == 1u) {
        if ((raw != '\n') && (raw != 0x8au)) {
          return XYZM_STATUS_PROTOCOL_ERROR;
        }
      } else if ((raw != 0x11u) && (raw != 0x91u)) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      probe->trailer_len += 1u;
      if (probe->trailer_len < probe->expected_trailer_len) {
        return XYZM_STATUS_OK;
      }
      *header_ready = true;
      xyzm_zmodem_control_header_ready(probe, type, hdr);
      return XYZM_STATUS_OK;

    default:
      return XYZM_STATUS_IO_ERROR;
  }
}

/**
 * @brief Sends raw wire bytes and remembers the trailing byte for escape heuristics.
 *
 * @param runtime Active transfer state.
 * @param buf Raw wire bytes to send.
 * @param len Number of bytes to send from @p buf.
 * @param state Optional send-side wire state updated with the last emitted byte.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_zmodem_send_bytes(
    xyzm_runtime_t *runtime,
    const uint8_t *buf,
    size_t len,
    xyzm_zmodem_send_state_t *state) {
  xyzm_status_t status = xyzm_send_exact(runtime, buf, len, runtime->options->block_timeout_ms);
  if ((status == XYZM_STATUS_OK) && (len > 0u) && (state != NULL)) {
    state->last_wire_byte = buf[len - 1u];
    state->has_last_wire_byte = true;
  }
  return status;
}

static inline xyzm_status_t xyzm_zmodem_send_control_pump(
    xyzm_runtime_t *runtime,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  *resume_requested = false;
  if ((pending != NULL) && pending->available) {
    return XYZM_STATUS_OK;
  }

  for (;;) {
    uint8_t raw;
    size_t read_len = 0u;
    xyzm_status_t status = runtime->link->recv(
        runtime->link->opaque,
        &raw,
        1u,
        0u,
        &read_len);
    if (status == XYZM_STATUS_TIMEOUT) {
      return XYZM_STATUS_OK;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (read_len == 0u) {
      return XYZM_STATUS_IO_ERROR;
    }

    bool header_ready;
    uint8_t type;
    uint8_t hdr[4];
    status = xyzm_zmodem_control_probe_feed(
        probe,
        raw,
        &header_ready,
        &type,
        hdr);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (!header_ready) {
      continue;
    }
    if (type == XYZM_ZRPOS) {
      if (pending != NULL) {
        pending->type = type;
        memcpy(pending->hdr, hdr, 4u);
        pending->available = true;
      }
      *resume_requested = true;
      return XYZM_STATUS_OK;
    }
    if (type == XYZM_ZRINIT) {
      if (pending != NULL) {
        pending->type = type;
        memcpy(pending->hdr, hdr, 4u);
        pending->available = true;
      }
      return XYZM_STATUS_OK;
    }
    if ((type == XYZM_ZFIN) || (type == XYZM_ZSKIP)) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
  }
}

static inline xyzm_status_t xyzm_zmodem_send_bytes_with_control(
    xyzm_runtime_t *runtime,
    const uint8_t *buf,
    size_t len,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  uint64_t deadline_ms =
      runtime->link->now_ms(runtime->link->opaque) +
      (uint64_t)runtime->options->block_timeout_ms;
  size_t sent_len = 0u;

  while (sent_len < len) {
    uint64_t now_ms = runtime->link->now_ms(runtime->link->opaque);
    uint32_t remaining_ms;
    if (now_ms >= deadline_ms) {
      return XYZM_STATUS_TIMEOUT;
    }
    remaining_ms = ((deadline_ms - now_ms) > 0xffffffffu) ?
        0xffffffffu :
        (uint32_t)(deadline_ms - now_ms);
    if (remaining_ms == 0u) {
      return XYZM_STATUS_TIMEOUT;
    }
    size_t written_len = 0u;
    xyzm_status_t status = runtime->link->send(
        runtime->link->opaque,
        buf + sent_len,
        len - sent_len,
        remaining_ms,
        &written_len);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (written_len == 0u) {
      return XYZM_STATUS_IO_ERROR;
    }
    if (state != NULL) {
      state->last_wire_byte = buf[sent_len + written_len - 1u];
      state->has_last_wire_byte = true;
    }
    sent_len += written_len;

    status = xyzm_zmodem_send_control_pump(
        runtime,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Convenience wrapper for emitting a single raw wire byte.
 *
 * @param runtime Active transfer state.
 * @param value Raw wire byte to send.
 * @param state Optional send-side wire state updated with the emitted byte.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_zmodem_send_wire_byte(
    xyzm_runtime_t *runtime,
    uint8_t value,
    xyzm_zmodem_send_state_t *state) {
  return xyzm_zmodem_send_bytes(runtime, &value, 1u, state);
}

static inline xyzm_status_t xyzm_zmodem_send_wire_byte_with_control(
    xyzm_runtime_t *runtime,
    uint8_t value,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  return xyzm_zmodem_send_bytes_with_control(
      runtime,
      &value,
      1u,
      state,
      probe,
      pending,
      resume_requested);
}

/**
 * @brief Receives one uninterpreted wire byte using the caller's timeout budget.
 *
 * @param runtime Active transfer state.
 * @param value Output raw wire byte.
 * @param timeout_ms Maximum time to wait for the byte.
 * @return Status returned by the underlying transport read path.
 */
static inline xyzm_status_t xyzm_zmodem_recv_raw(
    xyzm_runtime_t *runtime,
    uint8_t *value,
    uint32_t timeout_ms) {
  return xyzm_recv_byte(runtime, value, timeout_ms);
}

/**
 * @brief Clamps the configured packet length to the range this implementation supports.
 *
 * @param runtime Active transfer state that owns the configured packet length.
 * @return Effective packet length in bytes.
 */
static inline uint16_t xyzm_zmodem_packet_len(const xyzm_runtime_t *runtime) {
  uint16_t packet_len = runtime->options->zmodem_packet_len;
  if (packet_len == 0u) {
    return 1024u;
  }
  if (packet_len < 32u) {
    return 32u;
  }
  if (packet_len > 1024u) {
    return 1024u;
  }
  return packet_len;
}

/**
 * @brief Applies ZMODEM's control-byte and attention-sequence escaping rules.
 *
 * @param value Candidate byte to emit.
 * @param escape_ctrl Whether control bytes should be escaped aggressively.
 * @param state Optional send-side wire state used for contextual CR escaping.
 * @return `true` when @p value must be escaped.
 * @return `false` when @p value may be emitted verbatim.
 */
static inline bool xyzm_zmodem_needs_escape(
    uint8_t value,
    bool escape_ctrl,
    const xyzm_zmodem_send_state_t *state) {
  if (value == XYZM_ZDLE || value == 0x7fu || value == 0xffu) {
    return true;
  }
  if (value == XYZM_XON || value == XYZM_XOFF ||
      value == (uint8_t)(XYZM_XON | 0x80u) ||
      value == (uint8_t)(XYZM_XOFF | 0x80u)) {
    return true;
  }
  if (((value == '\r') || (value == (uint8_t)('\r' | 0x80u))) &&
      (state != NULL) && state->has_last_wire_byte &&
      ((state->last_wire_byte & 0x7fu) == '@')) {
    return true;
  }
  if (escape_ctrl && value < 0x20u) {
    return true;
  }
  if (escape_ctrl &&
      (value == 0x80u || value == 0x90u || value == 0x91u || value == 0x93u)) {
    return true;
  }
  return false;
}

/**
 * @brief Emits one payload byte, escaping it with ZDLE when the wire rules require it.
 *
 * @param runtime Active transfer state.
 * @param value Payload byte to emit.
 * @param escape_ctrl Whether control bytes should be escaped aggressively.
 * @param state Send-side wire state updated with the emitted bytes.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_zmodem_send_escaped_byte(
    xyzm_runtime_t *runtime,
    uint8_t value,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  if (!xyzm_zmodem_needs_escape(value, escape_ctrl, state)) {
    return xyzm_zmodem_send_wire_byte(runtime, value, state);
  }

  uint8_t pair[2];
  pair[0] = XYZM_ZDLE;
  if (value == 0x7fu) {
    pair[1] = XYZM_ZRUB0;
  } else if (value == 0xffu) {
    pair[1] = XYZM_ZRUB1;
  } else {
    pair[1] = (uint8_t)(value ^ 0x40u);
  }
  return xyzm_zmodem_send_bytes(runtime, pair, sizeof(pair), state);
}

static inline xyzm_status_t xyzm_zmodem_send_escaped_byte_with_control(
    xyzm_runtime_t *runtime,
    uint8_t value,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  if (!xyzm_zmodem_needs_escape(value, escape_ctrl, state)) {
    return xyzm_zmodem_send_wire_byte_with_control(
        runtime,
        value,
        state,
        probe,
        pending,
        resume_requested);
  }

  uint8_t pair[2];
  pair[0] = XYZM_ZDLE;
  if (value == 0x7fu) {
    pair[1] = XYZM_ZRUB0;
  } else if (value == 0xffu) {
    pair[1] = XYZM_ZRUB1;
  } else {
    pair[1] = (uint8_t)(value ^ 0x40u);
  }
  return xyzm_zmodem_send_bytes_with_control(
      runtime,
      pair,
      sizeof(pair),
      state,
      probe,
      pending,
      resume_requested);
}

static inline void xyzm_zmodem_record_wire_byte(
    xyzm_zmodem_send_state_t *state,
    uint8_t value) {
  if (state == NULL) {
    return;
  }
  state->last_wire_byte = value;
  state->has_last_wire_byte = true;
}

static inline xyzm_status_t xyzm_zmodem_flush_wire_batch_with_control(
    xyzm_runtime_t *runtime,
    xyzm_zmodem_wire_batch_t *batch,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  xyzm_status_t status;
  if (batch->len == 0u) {
    return XYZM_STATUS_OK;
  }
  status = xyzm_zmodem_send_bytes_with_control(
      runtime,
      batch->bytes,
      batch->len,
      NULL,
      probe,
      pending,
      resume_requested);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  batch->len = 0u;
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_zmodem_append_wire_byte_with_control(
    xyzm_runtime_t *runtime,
    xyzm_zmodem_wire_batch_t *batch,
    uint8_t value,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  xyzm_status_t status;
  if (batch->len == sizeof(batch->bytes)) {
    status = xyzm_zmodem_flush_wire_batch_with_control(
        runtime,
        batch,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
  }
  batch->bytes[batch->len++] = value;
  xyzm_zmodem_record_wire_byte(state, value);
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_zmodem_append_escaped_byte_with_control(
    xyzm_runtime_t *runtime,
    xyzm_zmodem_wire_batch_t *batch,
    uint8_t value,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  xyzm_status_t status;
  uint8_t escaped;

  if (!xyzm_zmodem_needs_escape(value, escape_ctrl, state)) {
    return xyzm_zmodem_append_wire_byte_with_control(
        runtime,
        batch,
        value,
        state,
        probe,
        pending,
        resume_requested);
  }

  if (value == 0x7fu) {
    escaped = XYZM_ZRUB0;
  } else if (value == 0xffu) {
    escaped = XYZM_ZRUB1;
  } else {
    escaped = (uint8_t)(value ^ 0x40u);
  }
  status = xyzm_zmodem_append_wire_byte_with_control(
      runtime,
      batch,
      XYZM_ZDLE,
      state,
      probe,
      pending,
      resume_requested);
  if ((status != XYZM_STATUS_OK) || *resume_requested) {
    return status;
  }
  return xyzm_zmodem_append_wire_byte_with_control(
      runtime,
      batch,
      escaped,
      state,
      probe,
      pending,
      resume_requested);
}

/**
 * @brief Formats and sends a human-readable ZHEX header with its trailer bytes.
 *
 * @param runtime Active transfer state.
 * @param type Header type byte.
 * @param hdr 4-byte header payload.
 * @param state Send-side wire state updated with the emitted bytes.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_zmodem_send_hex_header(
    xyzm_runtime_t *runtime,
    uint8_t type,
    const uint8_t hdr[4],
    xyzm_zmodem_send_state_t *state) {
  uint16_t crc = xyzm_zmodem_crc16_header(type, hdr);
  size_t index = 0u;
  uint8_t frame[32];
  frame[index++] = XYZM_ZPAD;
  frame[index++] = XYZM_ZPAD;
  frame[index++] = XYZM_ZDLE;
  frame[index++] = XYZM_ZHEX;
  frame[index++] = xyzm_hex_digit((type >> 4) & 0x0fu);
  frame[index++] = xyzm_hex_digit(type & 0x0fu);
  for (size_t i = 0u; i < 4u; ++i) {
    frame[index++] = xyzm_hex_digit((hdr[i] >> 4) & 0x0fu);
    frame[index++] = xyzm_hex_digit(hdr[i] & 0x0fu);
  }
  frame[index++] = xyzm_hex_digit((crc >> 12) & 0x0fu);
  frame[index++] = xyzm_hex_digit((crc >> 8) & 0x0fu);
  frame[index++] = xyzm_hex_digit((crc >> 4) & 0x0fu);
  frame[index++] = xyzm_hex_digit(crc & 0x0fu);
  frame[index++] = '\r';
  frame[index++] = 0x8au;
  if ((type != XYZM_ZFIN) && (type != XYZM_ZACK)) {
    frame[index++] = 0x11u;
  }
  return xyzm_zmodem_send_bytes(runtime, frame, index, state);
}

/**
 * @brief Sends a binary32 header and streams its CRC32 with the normal escape rules.
 *
 * @param runtime Active transfer state.
 * @param type Header type byte.
 * @param hdr 4-byte header payload.
 * @param escape_ctrl Whether control bytes should be escaped aggressively.
 * @param state Send-side wire state updated with the emitted bytes.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_zmodem_send_binary32_header(
    xyzm_runtime_t *runtime,
    uint8_t type,
    const uint8_t hdr[4],
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  xyzm_status_t status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZPAD, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZDLE, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZBIN32, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  uint32_t crc = xyzm_zcrc32_update(0xffffffffu, type);
  status = xyzm_zmodem_send_escaped_byte(runtime, type, escape_ctrl, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  for (size_t index = 0u; index < 4u; ++index) {
    crc = xyzm_zcrc32_update(crc, hdr[index]);
    status = xyzm_zmodem_send_escaped_byte(runtime, hdr[index], escape_ctrl, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }

  crc = ~crc;
  for (size_t index = 0u; index < 4u; ++index) {
    uint8_t value = (uint8_t)((crc >> (index * 8u)) & 0xffu);
    status = xyzm_zmodem_send_escaped_byte(runtime, value, escape_ctrl, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Sends a binary16 header and appends the CRC16 trailer expected by ZBIN.
 *
 * @param runtime Active transfer state.
 * @param type Header type byte.
 * @param hdr 4-byte header payload.
 * @param escape_ctrl Whether control bytes should be escaped aggressively.
 * @param state Send-side wire state updated with the emitted bytes.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_zmodem_send_binary16_header(
    xyzm_runtime_t *runtime,
    uint8_t type,
    const uint8_t hdr[4],
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  xyzm_status_t status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZPAD, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZDLE, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZBIN, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  uint16_t crc = xyzm_zmodem_crc16_update(0u, type);
  status = xyzm_zmodem_send_escaped_byte(runtime, type, escape_ctrl, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  for (size_t index = 0u; index < 4u; ++index) {
    crc = xyzm_zmodem_crc16_update(crc, hdr[index]);
    status = xyzm_zmodem_send_escaped_byte(runtime, hdr[index], escape_ctrl, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }

  crc = xyzm_zmodem_crc16_update(crc, 0u);
  crc = xyzm_zmodem_crc16_update(crc, 0u);
  status = xyzm_zmodem_send_escaped_byte(runtime, (uint8_t)(crc >> 8), escape_ctrl, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_escaped_byte(runtime, (uint8_t)crc, escape_ctrl, state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Sends one data subpacket, its frame-end marker, and the matching integrity trailer.
 *
 * @param runtime Active transfer state.
 * @param buf Payload bytes to send.
 * @param len Number of payload bytes in @p buf.
 * @param frame_end ZMODEM frame-end marker to append.
 * @param use_crc32 Whether the subpacket should use CRC32 instead of CRC16.
 * @param escape_ctrl Whether control bytes should be escaped aggressively.
 * @param state Send-side wire state updated with the emitted bytes.
 * @return Final status for the subpacket write.
 */
static inline xyzm_status_t xyzm_zmodem_send_data_subpacket(
    xyzm_runtime_t *runtime,
    const uint8_t *buf,
    size_t len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  xyzm_status_t status;
  if (use_crc32) {
    uint32_t crc;

    crc = 0xffffffffu;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zcrc32_update(crc, buf[index]);
      status = xyzm_zmodem_send_escaped_byte(runtime, buf[index], escape_ctrl, state);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
    }

    crc = xyzm_zcrc32_update(crc, frame_end);
    status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZDLE, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_zmodem_send_wire_byte(runtime, frame_end, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }

    crc = ~crc;
    for (size_t index = 0u; index < 4u; ++index) {
      uint8_t value = (uint8_t)((crc >> (index * 8u)) & 0xffu);
      status = xyzm_zmodem_send_escaped_byte(runtime, value, escape_ctrl, state);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
    }
  } else {
    uint16_t crc = 0u;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zmodem_crc16_update(crc, buf[index]);
      status = xyzm_zmodem_send_escaped_byte(runtime, buf[index], escape_ctrl, state);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
    }

    crc = xyzm_zmodem_crc16_update(crc, frame_end);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    status = xyzm_zmodem_send_wire_byte(runtime, XYZM_ZDLE, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_zmodem_send_wire_byte(runtime, frame_end, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_zmodem_send_escaped_byte(runtime, (uint8_t)(crc >> 8), escape_ctrl, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_zmodem_send_escaped_byte(runtime, (uint8_t)crc, escape_ctrl, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }
  if (frame_end == XYZM_ZCRCW) {
    status = xyzm_zmodem_send_wire_byte(runtime, XYZM_XON, state);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_zmodem_send_data_subpacket_with_control(
    xyzm_runtime_t *runtime,
    const uint8_t *buf,
    size_t len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  xyzm_status_t status;
  xyzm_zmodem_wire_batch_t batch;
  *resume_requested = false;
  memset(&batch, 0, sizeof(batch));

  if (use_crc32) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zcrc32_update(crc, buf[index]);
      status = xyzm_zmodem_append_escaped_byte_with_control(
          runtime,
          &batch,
          buf[index],
          escape_ctrl,
          state,
          probe,
          pending,
          resume_requested);
      if ((status != XYZM_STATUS_OK) || *resume_requested) {
        return status;
      }
    }

    crc = xyzm_zcrc32_update(crc, frame_end);
    status = xyzm_zmodem_append_wire_byte_with_control(
        runtime,
        &batch,
        XYZM_ZDLE,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
    status = xyzm_zmodem_append_wire_byte_with_control(
        runtime,
        &batch,
        frame_end,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }

    crc = ~crc;
    for (size_t index = 0u; index < 4u; ++index) {
      uint8_t value = (uint8_t)((crc >> (index * 8u)) & 0xffu);
      status = xyzm_zmodem_append_escaped_byte_with_control(
          runtime,
          &batch,
          value,
          escape_ctrl,
          state,
          probe,
          pending,
          resume_requested);
      if ((status != XYZM_STATUS_OK) || *resume_requested) {
        return status;
      }
    }
  } else {
    uint16_t crc = 0u;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zmodem_crc16_update(crc, buf[index]);
      status = xyzm_zmodem_append_escaped_byte_with_control(
          runtime,
          &batch,
          buf[index],
          escape_ctrl,
          state,
          probe,
          pending,
          resume_requested);
      if ((status != XYZM_STATUS_OK) || *resume_requested) {
        return status;
      }
    }

    crc = xyzm_zmodem_crc16_update(crc, frame_end);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    status = xyzm_zmodem_append_wire_byte_with_control(
        runtime,
        &batch,
        XYZM_ZDLE,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
    status = xyzm_zmodem_append_wire_byte_with_control(
        runtime,
        &batch,
        frame_end,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
    status = xyzm_zmodem_append_escaped_byte_with_control(
        runtime,
        &batch,
        (uint8_t)(crc >> 8),
        escape_ctrl,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
    status = xyzm_zmodem_append_escaped_byte_with_control(
        runtime,
        &batch,
        (uint8_t)crc,
        escape_ctrl,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
  }
  if (frame_end == XYZM_ZCRCW) {
    status = xyzm_zmodem_append_wire_byte_with_control(
        runtime,
        &batch,
        XYZM_XON,
        state,
        probe,
        pending,
        resume_requested);
    if ((status != XYZM_STATUS_OK) || *resume_requested) {
      return status;
    }
  }
  return xyzm_zmodem_flush_wire_batch_with_control(
      runtime,
      &batch,
      probe,
      pending,
      resume_requested);
}

/**
 * @brief Reads the next data byte or frame sentinel, undoing ZDLE escaping on the fly.
 *
 * @param runtime Active transfer state.
 * @param value Output decoded data byte or internal frame sentinel.
 * @param timeout_ms Maximum time to wait for additional wire data.
 * @return Final status for the decode step.
 * @retval XYZM_STATUS_OK A decoded byte or sentinel was produced.
 * @retval XYZM_STATUS_TIMEOUT No more wire data arrived before the timeout expired.
 */
static inline xyzm_status_t xyzm_zmodem_recv_zdle(
    xyzm_runtime_t *runtime,
    int *value,
    uint32_t timeout_ms) {
  uint8_t raw;
  for (;;) {
    xyzm_status_t status = xyzm_zmodem_recv_raw(runtime, &raw, timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if ((raw != XYZM_XON) && (raw != XYZM_XOFF) &&
        (raw != (uint8_t)(XYZM_XON | 0x80u)) &&
        (raw != (uint8_t)(XYZM_XOFF | 0x80u))) {
      break;
    }
  }
  if (raw != XYZM_ZDLE) {
    *value = raw;
    return XYZM_STATUS_OK;
  }

  for (;;) {
    xyzm_status_t status = xyzm_zmodem_recv_raw(runtime, &raw, timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if ((raw != XYZM_XON) && (raw != XYZM_XOFF) &&
        (raw != (uint8_t)(XYZM_XON | 0x80u)) &&
        (raw != (uint8_t)(XYZM_XOFF | 0x80u))) {
      break;
    }
  }
  if (raw == XYZM_ZRUB0) {
    *value = 0x7f;
  } else if (raw == XYZM_ZRUB1) {
    *value = 0xff;
  } else if (raw == XYZM_ZCRCE) {
    *value = XYZM_GOTCRCE;
  } else if (raw == XYZM_ZCRCG) {
    *value = XYZM_GOTCRCG;
  } else if (raw == XYZM_ZCRCQ) {
    *value = XYZM_GOTCRCQ;
  } else if (raw == XYZM_ZCRCW) {
    *value = XYZM_GOTCRCW;
  } else {
    *value = raw ^ 0x40u;
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Parses a ZBIN32 header and validates its CRC32 residue.
 *
 * @param runtime Active transfer state.
 * @param type Output header type byte.
 * @param hdr Output 4-byte header payload.
 * @return Status for the received header.
 * @retval XYZM_STATUS_OK The header and CRC32 residue were valid.
 * @retval XYZM_STATUS_PROTOCOL_ERROR The header encoding or CRC32 residue was invalid.
 */
static inline xyzm_status_t xyzm_zmodem_recv_binary32_header(
    xyzm_runtime_t *runtime,
    uint8_t *type,
    uint8_t hdr[4]) {
  int value;
  xyzm_status_t status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  *type = (uint8_t)value;
  uint32_t crc = xyzm_zcrc32_update(0xffffffffu, *type);

  for (size_t index = 0u; index < 4u; ++index) {
    status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    hdr[index] = (uint8_t)value;
    crc = xyzm_zcrc32_update(crc, hdr[index]);
  }
  for (size_t index = 0u; index < 4u; ++index) {
    status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    crc = xyzm_zcrc32_update(crc, (uint8_t)value);
  }
  if (crc != 0xdebb20e3u) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Parses a ZBIN header and validates its CRC16 residue.
 *
 * @param runtime Active transfer state.
 * @param type Output header type byte.
 * @param hdr Output 4-byte header payload.
 * @return Status for the received header.
 * @retval XYZM_STATUS_OK The header and CRC16 residue were valid.
 * @retval XYZM_STATUS_PROTOCOL_ERROR The header encoding or CRC16 residue was invalid.
 */
static inline xyzm_status_t xyzm_zmodem_recv_binary16_header(
    xyzm_runtime_t *runtime,
    uint8_t *type,
    uint8_t hdr[4]) {
  int value;
  xyzm_status_t status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  *type = (uint8_t)value;
  uint16_t crc = xyzm_zmodem_crc16_update(0u, *type);

  for (size_t index = 0u; index < 4u; ++index) {
    status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    hdr[index] = (uint8_t)value;
    crc = xyzm_zmodem_crc16_update(crc, hdr[index]);
  }
  for (size_t index = 0u; index < 2u; ++index) {
    status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    crc = xyzm_zmodem_crc16_update(crc, (uint8_t)value);
  }
  if (crc != 0u) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Parses a ZHEX header, including the CR/LF style trailer that follows it.
 *
 * @param runtime Active transfer state.
 * @param type Output header type byte.
 * @param hdr Output 4-byte header payload.
 * @return Status for the received header.
 * @retval XYZM_STATUS_OK The header and trailer bytes were valid.
 * @retval XYZM_STATUS_PROTOCOL_ERROR The hexadecimal payload or trailer was invalid.
 */
static inline xyzm_status_t xyzm_zmodem_recv_hex_header(
    xyzm_runtime_t *runtime,
    uint8_t *type,
    uint8_t hdr[4]) {
  uint8_t digits[14];
  for (size_t index = 0u; index < sizeof(digits); ++index) {
    xyzm_status_t status = xyzm_zmodem_recv_raw(runtime, &digits[index], runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (xyzm_from_hex(digits[index]) < 0) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
  }

  *type = (uint8_t)((xyzm_from_hex(digits[0]) << 4) | xyzm_from_hex(digits[1]));
  for (size_t index = 0u; index < 4u; ++index) {
    hdr[index] = (uint8_t)((xyzm_from_hex(digits[2u + index * 2u]) << 4) |
        xyzm_from_hex(digits[3u + index * 2u]));
  }
  uint16_t actual_crc = (uint16_t)((xyzm_from_hex(digits[10]) << 12) |
      (xyzm_from_hex(digits[11]) << 8) |
      (xyzm_from_hex(digits[12]) << 4) |
      xyzm_from_hex(digits[13]));
  uint16_t expected_crc = xyzm_zmodem_crc16_header(*type, hdr);
  if (actual_crc != expected_crc) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  {
    uint8_t trailer;
    xyzm_status_t status = xyzm_zmodem_recv_raw(runtime, &trailer, 10u);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if ((trailer != '\r') && (trailer != '\n')) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
    status = xyzm_zmodem_recv_raw(runtime, &trailer, 10u);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if ((trailer != '\n') && (trailer != 0x8au)) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
    if ((*type != XYZM_ZFIN) && (*type != XYZM_ZACK)) {
      status = xyzm_zmodem_recv_raw(runtime, &trailer, 10u);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
      if ((trailer != 0x11u) && (trailer != 0x91u)) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
    }
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Scans the stream for the next header preamble and dispatches by frame encoding.
 *
 * @param runtime Active transfer state.
 * @param type Output header type byte.
 * @param hdr Output 4-byte header payload.
 * @param frame_kind Optional output frame encoding byte such as @ref XYZM_ZHEX.
 * @param timeout_ms Maximum total time to spend searching for a header preamble.
 * @return Status for the received header.
 * @retval XYZM_STATUS_OK A complete header was parsed successfully.
 * @retval XYZM_STATUS_TIMEOUT No header preamble was completed before the timeout expired.
 */
static inline xyzm_status_t xyzm_zmodem_recv_header(
    xyzm_runtime_t *runtime,
    uint8_t *type,
    uint8_t hdr[4],
    uint8_t *frame_kind,
    uint32_t timeout_ms) {
  uint64_t deadline_ms = runtime->link->now_ms(runtime->link->opaque) + timeout_ms;
  for (;;) {
    uint32_t remaining_ms = (uint32_t)(deadline_ms - runtime->link->now_ms(runtime->link->opaque));
    if ((int32_t)remaining_ms <= 0) {
      return XYZM_STATUS_TIMEOUT;
    }

    uint8_t byte0;
    xyzm_status_t status = xyzm_zmodem_recv_raw(runtime, &byte0, remaining_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (byte0 != XYZM_ZPAD) {
      continue;
    }

    status = xyzm_zmodem_recv_raw(runtime, &byte0, remaining_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (byte0 == XYZM_ZPAD) {
      status = xyzm_zmodem_recv_raw(runtime, &byte0, remaining_ms);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
    }
    if (byte0 != XYZM_ZDLE) {
      continue;
    }

    status = xyzm_zmodem_recv_raw(runtime, &byte0, remaining_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (frame_kind != NULL) {
      *frame_kind = byte0;
    }
    if (byte0 == XYZM_ZBIN) {
      return xyzm_zmodem_recv_binary16_header(runtime, type, hdr);
    }
    if (byte0 == XYZM_ZHEX) {
      return xyzm_zmodem_recv_hex_header(runtime, type, hdr);
    }
    if (byte0 == XYZM_ZBIN32) {
      return xyzm_zmodem_recv_binary32_header(runtime, type, hdr);
    }
  }
}

/**
 * @brief Receives one data subpacket until its frame-end marker and validates the trailer.
 *
 * @param runtime Active transfer state.
 * @param buf Destination payload buffer.
 * @param capacity Capacity of @p buf in bytes.
 * @param received_len Output byte count stored in @p buf.
 * @param frame_kind Frame encoding byte that determines the trailer format.
 * @param frame_end Output frame-end sentinel such as @ref XYZM_GOTCRCW.
 * @return Status for the received subpacket.
 * @retval XYZM_STATUS_OK The subpacket and integrity trailer were valid.
 * @retval XYZM_STATUS_PROTOCOL_ERROR The subpacket exceeded capacity or failed integrity checks.
 */
static inline xyzm_status_t xyzm_zmodem_recv_data(
    xyzm_runtime_t *runtime,
    uint8_t *buf,
    size_t capacity,
    size_t *received_len,
    uint8_t frame_kind,
    int *frame_end) {
  const bool use_crc32 = frame_kind == XYZM_ZBIN32;
  uint32_t crc32 = 0xffffffffu;
  uint16_t crc16 = 0u;
  size_t used_len = 0u;
  for (;;) {
    int value;
    xyzm_status_t status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if ((value & 0x400) != 0) {
      *frame_end = value;
      if (use_crc32) {
        crc32 = xyzm_zcrc32_update(crc32, (uint8_t)(value & 0xff));
      } else {
        crc16 = xyzm_zmodem_crc16_update(crc16, (uint8_t)(value & 0xff));
      }
      for (size_t index = 0u; index < (use_crc32 ? 4u : 2u); ++index) {
        status = xyzm_zmodem_recv_zdle(runtime, &value, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return status;
        }
        if (use_crc32) {
          crc32 = xyzm_zcrc32_update(crc32, (uint8_t)value);
        } else {
          crc16 = xyzm_zmodem_crc16_update(crc16, (uint8_t)value);
        }
      }
      if (use_crc32 && (crc32 != 0xdebb20e3u)) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      if (!use_crc32 && (crc16 != 0u)) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      *received_len = used_len;
      return XYZM_STATUS_OK;
    }
    if (used_len >= capacity) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
    buf[used_len++] = (uint8_t)value;
    if (use_crc32) {
      crc32 = xyzm_zcrc32_update(crc32, (uint8_t)value);
    } else {
      crc16 = xyzm_zmodem_crc16_update(crc16, (uint8_t)value);
    }
  }
}

/**
 * @brief Builds sink file metadata from the ZFILE payload carried by the peer.
 *
 * @param file Output receive-side file state.
 * @param metadata Raw metadata payload from the peer.
 * @param metadata_len Number of metadata bytes available in @p metadata.
 */
static inline void xyzm_zmodem_build_file_info(
    xyzm_receive_file_t *file,
    const uint8_t *metadata,
    size_t metadata_len) {
  memset(file, 0, sizeof(*file));
  size_t name_len = 0u;
  while ((name_len < metadata_len) && (metadata[name_len] != '\0') &&
         (name_len + 1u < sizeof(file->name_storage))) {
    file->name_storage[name_len] = (char)metadata[name_len];
    name_len += 1u;
  }
  file->name_storage[name_len] = '\0';
  if (name_len > 0u) {
    file->info.valid_mask |= XYZM_FILE_INFO_NAME;
    file->info.name = file->name_storage;
  }
  if ((name_len + 1u) < metadata_len && metadata[name_len + 1u] != '\0') {
    unsigned long long size_value = strtoull((const char *)(metadata + name_len + 1u), NULL, 10);
    file->info.valid_mask |= XYZM_FILE_INFO_SIZE;
    file->info.size_bytes = (uint64_t)size_value;
  }
}

/**
 * @brief Serializes the local file metadata into the payload used by ZFILE.
 *
 * @param file Send-side file metadata to serialize.
 * @param buf Destination metadata buffer.
 * @param capacity Capacity of @p buf in bytes.
 * @return Serialized metadata size in bytes.
 * @return `0` when the metadata cannot fit in @p buf.
 */
static inline size_t xyzm_zmodem_build_metadata(
    const xyzm_send_file_t *file,
    uint8_t *buf,
    size_t capacity) {
  const char *name = ((file->info.valid_mask & XYZM_FILE_INFO_NAME) != 0u) &&
          (file->info.name != NULL) ? file->info.name : "file.bin";
  const size_t name_len = strlen(name);
  if (name_len + 2u >= capacity) {
    return 0u;
  }
  memcpy(buf, name, name_len);
  buf[name_len] = '\0';
  const int written_len = snprintf(
      (char *)(buf + name_len + 1u),
      capacity - name_len - 1u,
      "%llu 0 0 0 1 1",
      (unsigned long long)(((file->info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) ?
          file->info.size_bytes : 0u));
  if (written_len < 0) {
    return 0u;
  }
  return name_len + 1u + (size_t)written_len + 1u;
}

/**
 * @brief Waits for a specific header type while treating timeouts as retryable handshakes.
 *
 * @param runtime Active transfer state.
 * @param expected_type Header type expected from the peer.
 * @param hdr Output 4-byte header payload for the matched header.
 * @return Final handshake status.
 * @retval XYZM_STATUS_OK The expected header type was received.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted without the expected header.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer ended the session with `ZFIN` or skipped the file with `ZSKIP`.
 */
static inline xyzm_status_t xyzm_zmodem_wait_for_type(
    xyzm_runtime_t *runtime,
    uint8_t expected_type,
    uint8_t hdr[4]) {
  for (unsigned attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t type;
    xyzm_status_t status = xyzm_zmodem_recv_header(
        runtime,
        &type,
        hdr,
        NULL,
        runtime->options->handshake_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (type == expected_type) {
      return XYZM_STATUS_OK;
    }
    if ((type == XYZM_ZFIN) || (type == XYZM_ZSKIP)) {
      /*
       * Forsberg's ZMODEM specification defines ZSKIP as the receiver response
       * to ZFILE that tells the sender to skip the current file:
       * https://www.tuhs.org/Usenet/comp.sources.unix/1986-May/004372.html
       *
       * GNU lrzsz 0.12.21's src/lrz.c follows that sequence: procheader()
       * returns ERROR when an existing destination is protected, rzfile()
       * sets tryzhdrtype = ZSKIP, and the next tryz() sends that header.
       * This single-file API has no "next file" to continue with, so surface
       * the peer skip as a remote-cancelled terminal status for now. A future
       * public skip status/event can distinguish the reason without changing
       * the wire-state handling here.
       */
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
  }
  return XYZM_STATUS_TIMEOUT;
}

static inline xyzm_status_t xyzm_zmodem_wait_for_type_with_pending(
    xyzm_runtime_t *runtime,
    uint8_t expected_type,
    uint8_t hdr[4],
    xyzm_zmodem_pending_header_t *pending) {
  if ((pending != NULL) && pending->available) {
    const uint8_t type = pending->type;
    memcpy(hdr, pending->hdr, 4u);
    pending->available = false;
    if (type == expected_type) {
      return XYZM_STATUS_OK;
    }
    if ((type == XYZM_ZFIN) || (type == XYZM_ZSKIP)) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
    return XYZM_STATUS_PROTOCOL_ERROR;
  }
  return xyzm_zmodem_wait_for_type(runtime, expected_type, hdr);
}

static inline xyzm_status_t xyzm_zmodem_send_begin_session(
    xyzm_runtime_t *runtime,
    xyzm_zmodem_send_state_t *send_state,
    bool *use_crc32,
    bool *escape_ctrl,
    size_t *packet_len) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  xyzm_status_t status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZRQINIT, hdr, send_state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_wait_for_type(runtime, XYZM_ZRINIT, hdr);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  *use_crc32 = (runtime->options->zmodem_use_crc32 != 0u) &&
      ((hdr[3] & XYZM_CANFC32) != 0u);
  *escape_ctrl = (runtime->options->zmodem_escape_ctrl != 0u) ||
      ((hdr[3] & XYZM_ESCCTL) != 0u);
  *packet_len = xyzm_zmodem_packet_len(runtime);
  uint16_t peer_packet_len = (uint16_t)((uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8));
  if ((peer_packet_len >= 32u) && (peer_packet_len < *packet_len)) {
    *packet_len = peer_packet_len;
  }
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_zmodem_send_apply_zrpos(
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    const uint8_t hdr[4],
    uint64_t *payload_bytes) {
  uint64_t offset = xyzm_zmodem_rclhdr(hdr);
  if (((file->info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) &&
      (offset > file->info.size_bytes)) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }
  if (offset != 0u) {
    if (source->seek == NULL) {
      return XYZM_STATUS_UNSUPPORTED;
    }
    xyzm_status_t status = source->seek(source->opaque, file->file_opaque, offset);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }
  *payload_bytes = offset;
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_zmodem_send_one_file(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    xyzm_zmodem_send_state_t *send_state,
    bool use_crc32,
    bool escape_ctrl,
    size_t packet_len) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  hdr[3] = XYZM_ZCBIN;
  uint8_t metadata[512];
  size_t metadata_len = xyzm_zmodem_build_metadata(file, metadata, sizeof(metadata));
  xyzm_status_t status;
  if (metadata_len == 0u) {
    return XYZM_STATUS_ARGUMENT_ERROR;
  }
  if (use_crc32) {
    status = xyzm_zmodem_send_binary32_header(
        runtime,
        XYZM_ZFILE,
        hdr,
        escape_ctrl,
        send_state);
  } else {
    status = xyzm_zmodem_send_binary16_header(
        runtime,
        XYZM_ZFILE,
        hdr,
        escape_ctrl,
        send_state);
  }
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_data_subpacket(
      runtime,
      metadata,
      metadata_len,
      XYZM_ZCRCW,
      use_crc32,
      escape_ctrl,
      send_state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_wait_for_type(runtime, XYZM_ZRPOS, hdr);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  uint64_t payload_bytes = 0u;
  status = xyzm_zmodem_send_apply_zrpos(source, file, hdr, &payload_bytes);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  xyzm_zmodem_control_probe_t control_probe;
  xyzm_zmodem_pending_header_t pending_header;
  xyzm_zmodem_control_probe_init(&control_probe);
  memset(&pending_header, 0, sizeof(pending_header));

  for (;;) {
    bool resume_requested = false;

    xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
    if (use_crc32) {
      status = xyzm_zmodem_send_binary32_header(
          runtime,
          XYZM_ZDATA,
          hdr,
          escape_ctrl,
          send_state);
    } else {
      status = xyzm_zmodem_send_binary16_header(
          runtime,
          XYZM_ZDATA,
          hdr,
          escape_ctrl,
          send_state);
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }

    for (;;) {
      uint8_t payload[1024];
      size_t total_len = 0u;
      uint8_t frame_end;
      for (;;) {
        size_t read_len;
        status = source->read(
            source->opaque,
            file->file_opaque,
            payload + total_len,
            packet_len - total_len,
            &read_len);
        if (status == XYZM_STATUS_END) {
          break;
        }
        if (status != XYZM_STATUS_OK) {
          return status;
        }
        if (read_len == 0u) {
          return XYZM_STATUS_IO_ERROR;
        }
        total_len += read_len;
        if (total_len == packet_len) {
          break;
        }
      }

      if (total_len == 0u) {
        status = xyzm_zmodem_send_data_subpacket_with_control(
            runtime,
            payload,
            0u,
            XYZM_ZCRCE,
            use_crc32,
            escape_ctrl,
            send_state,
            &control_probe,
            &pending_header,
            &resume_requested);
        break;
      }
      frame_end = (total_len < packet_len) ? XYZM_ZCRCE : XYZM_ZCRCG;
      status = xyzm_zmodem_send_data_subpacket_with_control(
          runtime,
          payload,
          total_len,
          frame_end,
          use_crc32,
          escape_ctrl,
          send_state,
          &control_probe,
          &pending_header,
          &resume_requested);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
      if (resume_requested) {
        if (!pending_header.available || (pending_header.type != XYZM_ZRPOS)) {
          return XYZM_STATUS_PROTOCOL_ERROR;
        }
        status = xyzm_zmodem_send_apply_zrpos(
            source,
            file,
            pending_header.hdr,
            &payload_bytes);
        if (status != XYZM_STATUS_OK) {
          return status;
        }
        pending_header.available = false;
        xyzm_zmodem_control_probe_init(&control_probe);
        break;
      }
      xyzm_report_payload(runtime, total_len, &file->info);
      payload_bytes += total_len;
      if (frame_end == XYZM_ZCRCE) {
        break;
      }
    }

    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (resume_requested && pending_header.available) {
      if (pending_header.type != XYZM_ZRPOS) {
        return XYZM_STATUS_PROTOCOL_ERROR;
      }
      status = xyzm_zmodem_send_apply_zrpos(
          source,
          file,
          pending_header.hdr,
          &payload_bytes);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
      pending_header.available = false;
      xyzm_zmodem_control_probe_init(&control_probe);
    }
    if (!resume_requested) {
      break;
    }
  }

  xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
  if (use_crc32) {
    status = xyzm_zmodem_send_binary32_header(
        runtime,
        XYZM_ZEOF,
        hdr,
        escape_ctrl,
        send_state);
  } else {
    status = xyzm_zmodem_send_binary16_header(
        runtime,
        XYZM_ZEOF,
        hdr,
        escape_ctrl,
        send_state);
  }
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_wait_for_type_with_pending(
      runtime,
      XYZM_ZRINIT,
      hdr,
      &pending_header);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_zmodem_send_finish_session(
    xyzm_runtime_t *runtime,
    xyzm_zmodem_send_state_t *send_state) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  xyzm_status_t status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZFIN, hdr, send_state);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_wait_for_type(runtime, XYZM_ZFIN, hdr);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  return xyzm_zmodem_send_bytes(runtime, (const uint8_t *)"OO", 2u, send_state);
}

xyzm_status_t xyzm_zmodem_send_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file) {
  xyzm_zmodem_send_state_t send_state;
  bool use_crc32;
  bool escape_ctrl;
  size_t packet_len;
  xyzm_status_t status;

  memset(&send_state, 0, sizeof(send_state));
  status = xyzm_zmodem_send_begin_session(
      runtime,
      &send_state,
      &use_crc32,
      &escape_ctrl,
      &packet_len);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_zmodem_send_one_file(
      runtime,
      source,
      file,
      &send_state,
      use_crc32,
      escape_ctrl,
      packet_len);
  if (status == XYZM_STATUS_OK) {
    status = xyzm_zmodem_send_finish_session(runtime, &send_state);
  }
  if (status == XYZM_STATUS_OK) {
    xyzm_report_completed_file(runtime);
  }
  return status;
}

xyzm_status_t xyzm_zmodem_send_batch_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file) {
  xyzm_zmodem_send_state_t send_state;
  bool use_crc32;
  bool escape_ctrl;
  size_t packet_len;
  xyzm_status_t status;

  memset(&send_state, 0, sizeof(send_state));
  status = xyzm_zmodem_send_begin_session(
      runtime,
      &send_state,
      &use_crc32,
      &escape_ctrl,
      &packet_len);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  for (;;) {
    status = xyzm_zmodem_send_one_file(
        runtime,
        source,
        file,
        &send_state,
        use_crc32,
        escape_ctrl,
        packet_len);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    xyzm_report_completed_file(runtime);
    status = xyzm_finish_send_file(runtime, source, file, XYZM_STATUS_OK);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_send_single_file(runtime, source, file);
    if (status == XYZM_STATUS_END) {
      return xyzm_zmodem_send_finish_session(runtime, &send_state);
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }
}

xyzm_status_t xyzm_zmodem_receive_impl(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = (uint8_t)(xyzm_zmodem_packet_len(runtime) & 0xffu);
  hdr[1] = (uint8_t)(xyzm_zmodem_packet_len(runtime) >> 8);
  hdr[3] = XYZM_CANFDX | XYZM_CANOVIO;
  if (runtime->options->zmodem_use_crc32 != 0u) {
    hdr[3] |= XYZM_CANFC32;
  }
  if (runtime->options->zmodem_escape_ctrl != 0u) {
    hdr[3] |= XYZM_ESCCTL;
  }

  uint8_t type;
  uint8_t frame_kind;
  xyzm_status_t status;
  unsigned attempt = 0u;
  bool have_final_zfin = false;
  bool accept_final_zfin = false;
  for (;;) {
    status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZRINIT, hdr, NULL);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
wait_zfile_or_zfin:
    status = xyzm_zmodem_recv_header(runtime, &type, hdr, &frame_kind, runtime->options->handshake_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      if (attempt >= runtime->options->retry_limit) {
        return XYZM_STATUS_TIMEOUT;
      }
      attempt += 1u;
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (type == XYZM_ZRQINIT) {
      if (attempt >= runtime->options->retry_limit) {
        return XYZM_STATUS_TIMEOUT;
      }
      attempt += 1u;
      continue;
    }
    if (type == XYZM_ZFILE) {
      break;
    }
    if (accept_final_zfin && (type == XYZM_ZFIN)) {
      have_final_zfin = true;
      goto finish_zfin;
    }
    if (attempt >= runtime->options->retry_limit) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
    attempt += 1u;
    xyzm_report_retry(runtime);
  }

  size_t used_len;
  int frame_end;
  uint8_t buffer[4096];
  status = xyzm_zmodem_recv_data(runtime, buffer, sizeof(buffer), &used_len, frame_kind, &frame_end);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  xyzm_receive_file_t file;
  uint64_t resume_offset;
  memset(&file, 0, sizeof(file));
  xyzm_zmodem_build_file_info(&file, buffer, used_len);
  resume_offset = 0u;
  status = sink->begin(sink->opaque, &file.info, &resume_offset, &file.file_opaque);
  if (status != XYZM_STATUS_OK) {
    xyzm_send_cancel(runtime);
    return status;
  }
  if (((file.info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) &&
      (resume_offset > file.info.size_bytes)) {
    xyzm_send_cancel(runtime);
    return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_ARGUMENT_ERROR);
  }
  xyzm_notify_file_started(runtime, &file.info);

  uint64_t remaining_bytes = UINT64_MAX;
  if ((file.info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) {
    remaining_bytes = file.info.size_bytes - resume_offset;
  }

  xyzm_zmodem_stohdr((uint32_t)resume_offset, hdr);
  status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZRPOS, hdr, NULL);
  if (status != XYZM_STATUS_OK) {
    return xyzm_finish_receive_file(runtime, sink, &file, status);
  }

  uint64_t payload_bytes = resume_offset;
  for (;;) {
    status = xyzm_zmodem_recv_header(
        runtime,
        &type,
        hdr,
        &frame_kind,
        runtime->options->handshake_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }

    if (type == XYZM_ZDATA) {
      uint64_t offset;

      offset = xyzm_zmodem_rclhdr(hdr);
      if (offset != payload_bytes) {
        return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_UNSUPPORTED);
      }
      for (;;) {
        status = xyzm_zmodem_recv_data(
            runtime,
            buffer,
            sizeof(buffer),
            &used_len,
            frame_kind,
            &frame_end);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
        if (used_len > 0u) {
          size_t write_len;

          write_len = used_len;
          if ((file.info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) {
            if (remaining_bytes < write_len) {
              return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
            }
          }
          if (write_len > 0u) {
            status = sink->write(sink->opaque, file.file_opaque, buffer, write_len);
            if (status != XYZM_STATUS_OK) {
              xyzm_send_cancel(runtime);
              return xyzm_finish_receive_file(runtime, sink, &file, status);
            }
            xyzm_report_payload(runtime, write_len, &file.info);
            payload_bytes += write_len;
            if (remaining_bytes != UINT64_MAX) {
              remaining_bytes -= write_len;
            }
          }
        }

        if (frame_end == XYZM_GOTCRCQ) {
          xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
          status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZACK, hdr, NULL);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
          continue;
        }
        if (frame_end == XYZM_GOTCRCW) {
          xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
          status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZACK, hdr, NULL);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
          break;
        }
        if (frame_end == XYZM_GOTCRCG) {
          continue;
        }
        break;
      }
      continue;
    }
    if (type == XYZM_ZEOF) {
      if ((file.info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) {
        if (remaining_bytes != 0u) {
          return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
        }
      }
      if (xyzm_zmodem_rclhdr(hdr) != payload_bytes) {
        return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
      }
      status = xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_OK);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
      xyzm_report_completed_file(runtime);
      memset(hdr, 0, sizeof(hdr));
      status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZRINIT, hdr, NULL);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
      if (runtime->options->batch_mode) {
        memset(&file, 0, sizeof(file));
        attempt = 0u;
        accept_final_zfin = true;
        goto wait_zfile_or_zfin;
      }
      break;
    }
  }

  if (!have_final_zfin) {
    status = xyzm_zmodem_recv_header(
        runtime,
        &type,
        hdr,
        &frame_kind,
        runtime->options->handshake_timeout_ms);
    if ((status != XYZM_STATUS_OK) || (type != XYZM_ZFIN)) {
      return (status == XYZM_STATUS_OK) ? XYZM_STATUS_PROTOCOL_ERROR : status;
    }
  }
finish_zfin:
  memset(hdr, 0, sizeof(hdr));
  status = xyzm_zmodem_send_hex_header(runtime, XYZM_ZFIN, hdr, NULL);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  {
    uint8_t trailer[2];
    (void)xyzm_recv_exact(runtime, trailer, sizeof(trailer), runtime->options->block_timeout_ms);
  }
  return XYZM_STATUS_OK;
}
