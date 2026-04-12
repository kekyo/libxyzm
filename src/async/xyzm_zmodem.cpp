/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_async_internal.h"

#include <array>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Keeps the private wire constants local to the ZMODEM implementation.
 */
enum {
  XYZM_ZMODEM_ASYNC_WIRE_BATCH_CAPACITY = 16384,
  XYZM_ZMODEM_ASYNC_SEND_PACKET_LEN = 8192,
  XYZM_ZMODEM_ASYNC_RECV_PACKET_LEN = 1024,
  XYZM_ZPAD = '*',
  XYZM_ZDLE = 0x18,
  XYZM_ZBIN = 'A',
  XYZM_ZBIN32 = 'C',
  XYZM_ZHEX = 'B',
  XYZM_ZRQINIT = 0,
  XYZM_ZRINIT = 1,
  XYZM_ZACK = 3,
  XYZM_ZFILE = 4,
  XYZM_ZSKIP = 5,
  XYZM_ZFIN = 8,
  XYZM_ZRPOS = 9,
  XYZM_ZDATA = 10,
  XYZM_ZEOF = 11,
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
struct xyzm_zmodem_send_state_t {
  uint8_t last_wire_byte;
  bool has_last_wire_byte;
};

struct xyzm_zmodem_recv_cursor_t {
  xyzm_async_runtime_t *runtime;
  uint32_t timeout_ms;
};

struct xyzm_zmodem_wire_batch_t {
  uint8_t bytes[XYZM_ZMODEM_ASYNC_WIRE_BATCH_CAPACITY];
  size_t len;
};

struct xyzm_zmodem_pending_header_t {
  uint8_t type;
  uint8_t hdr[4];
  bool available;
};

struct xyzm_zmodem_control_probe_t {
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
};

using xyzm_zcrc32_tables_t = std::array<std::array<uint32_t, 256>, 8>;

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

static inline const xyzm_zcrc32_tables_t &xyzm_zcrc32_tables(void) {
  static constexpr auto tables = []() {
    xyzm_zcrc32_tables_t values {};
    for (uint32_t index = 0u; index < values[0].size(); ++index) {
      uint32_t entry = index;
      for (unsigned bit = 0u; bit < 8u; ++bit) {
        if ((entry & 1u) != 0u) {
          entry = (entry >> 1) ^ 0xedb88320u;
        } else {
          entry >>= 1;
        }
      }
      values[0][index] = entry;
    }
    for (std::size_t table_index = 1u; table_index < values.size(); ++table_index) {
      for (std::size_t index = 0u; index < values[table_index].size(); ++index) {
        const uint32_t previous = values[table_index - 1u][index];
        values[table_index][index] =
            (previous >> 8) ^ values[0][previous & 0xffu];
      }
    }
    return values;
  }();
  return tables;
}

static inline uint32_t xyzm_zcrc32_update(uint32_t crc, uint8_t value) {
  const auto &tables = xyzm_zcrc32_tables();
  return (crc >> 8) ^ tables[0][(crc ^ value) & 0xffu];
}

static inline uint32_t xyzm_zcrc32_update_buffer(
    uint32_t crc,
    const uint8_t *buf,
    size_t len) {
  const auto &tables = xyzm_zcrc32_tables();
  size_t offset = 0u;
  while (len - offset >= 8u) {
    uint32_t value =
        crc ^
        (uint32_t)buf[offset] ^
        ((uint32_t)buf[offset + 1u] << 8) ^
        ((uint32_t)buf[offset + 2u] << 16) ^
        ((uint32_t)buf[offset + 3u] << 24);
    crc =
        tables[7][value & 0xffu] ^
        tables[6][(value >> 8) & 0xffu] ^
        tables[5][(value >> 16) & 0xffu] ^
        tables[4][(value >> 24) & 0xffu] ^
        tables[3][buf[offset + 4u]] ^
        tables[2][buf[offset + 5u]] ^
        tables[1][buf[offset + 6u]] ^
        tables[0][buf[offset + 7u]];
    offset += 8u;
  }
  while (offset < len) {
    crc = (crc >> 8) ^ tables[0][(crc ^ buf[offset]) & 0xffu];
    offset += 1u;
  }
  return crc;
}

static inline uint16_t xyzm_zmodem_crc16_entry(uint8_t index) {
  static constexpr auto table = []() {
    std::array<uint16_t, 256> values {};
    for (uint16_t index = 0u; index < values.size(); ++index) {
      uint16_t crc = (uint16_t)(index << 8);
      for (unsigned bit = 0u; bit < 8u; ++bit) {
        if ((crc & 0x8000u) != 0u) {
          crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
          crc <<= 1;
        }
      }
      values[index] = crc;
    }
    return values;
  }();
  return table[index];
}

static inline void xyzm_zmodem_stohdr(uint32_t value, uint8_t hdr[4]) {
  hdr[0] = (uint8_t)(value & 0xffu);
  hdr[1] = (uint8_t)((value >> 8) & 0xffu);
  hdr[2] = (uint8_t)((value >> 16) & 0xffu);
  hdr[3] = (uint8_t)((value >> 24) & 0xffu);
}

static inline uint32_t xyzm_zmodem_rclhdr(const uint8_t hdr[4]) {
  return ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[2] << 16) |
      ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[0];
}

static inline uint8_t xyzm_hex_digit(unsigned value) {
  return (uint8_t)(value < 10u ? ('0' + value) : ('a' + (value - 10u)));
}

static inline uint16_t xyzm_zmodem_crc16_update(uint16_t crc, uint8_t value) {
  return (uint16_t)(
      xyzm_zmodem_crc16_entry((uint8_t)(crc >> 8)) ^
      (uint16_t)(crc << 8) ^
      value);
}

static inline uint16_t xyzm_zmodem_crc16_update_buffer(
    uint16_t crc,
    const uint8_t *buf,
    size_t len) {
  for (size_t index = 0u; index < len; ++index) {
    crc = xyzm_zmodem_crc16_update(crc, buf[index]);
  }
  return crc;
}

static inline uint16_t xyzm_zmodem_crc16_header(uint8_t type, const uint8_t hdr[4]) {
  uint16_t crc = xyzm_zmodem_crc16_update(0u, (uint8_t)(type & 0x7fu));
  for (size_t index = 0u; index < 4u; ++index) {
    crc = xyzm_zmodem_crc16_update(crc, hdr[index]);
  }
  crc = xyzm_zmodem_crc16_update(crc, 0u);
  crc = xyzm_zmodem_crc16_update(crc, 0u);
  return crc;
}

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

static inline cardio::promise<void> xyzm_zmodem_control_probe_feed_async(
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
      co_return;

    case XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD2:
      if (raw == XYZM_ZPAD) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_ZDLE;
        co_return;
      }
      if (raw == XYZM_ZDLE) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND;
        co_return;
      }
      probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
      co_return;

    case XYZM_ZMODEM_PARSE_HEADER_SEEK_ZDLE:
      if (raw == XYZM_ZDLE) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND;
      } else {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
      }
      co_return;

    case XYZM_ZMODEM_PARSE_HEADER_FRAME_KIND:
      probe->frame_kind = raw;
      probe->parse_index = 0u;
      if (raw == XYZM_ZBIN) {
        probe->crc16 = 0u;
        probe->decode_state = XYZM_ZMODEM_DECODE_FIRST;
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_BINARY16;
        co_return;
      }
      if (raw == XYZM_ZBIN32) {
        probe->crc32 = 0xffffffffu;
        probe->decode_state = XYZM_ZMODEM_DECODE_FIRST;
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_BINARY32;
        co_return;
      }
      if (raw == XYZM_ZHEX) {
        probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_HEX_DIGIT;
        co_return;
      }
      probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_SEEK_PAD1;
      co_return;

    case XYZM_ZMODEM_PARSE_HEADER_BINARY16: {
      int decoded;
      if (!xyzm_zmodem_control_decode_zdle(probe, raw, &decoded)) {
        co_return;
      }
      if ((decoded & 0x400) != 0) {
        xyzm_throw_protocol_async();
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
        co_return;
      }
      if (probe->crc16 != 0u) {
        xyzm_throw_protocol_async();
      }
      *header_ready = true;
      xyzm_zmodem_control_header_ready(probe, type, hdr);
      co_return;
    }

    case XYZM_ZMODEM_PARSE_HEADER_BINARY32: {
      int decoded;
      if (!xyzm_zmodem_control_decode_zdle(probe, raw, &decoded)) {
        co_return;
      }
      if ((decoded & 0x400) != 0) {
        xyzm_throw_protocol_async();
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
        co_return;
      }
      if (probe->crc32 != 0xdebb20e3u) {
        xyzm_throw_protocol_async();
      }
      *header_ready = true;
      xyzm_zmodem_control_header_ready(probe, type, hdr);
      co_return;
    }

    case XYZM_ZMODEM_PARSE_HEADER_HEX_DIGIT:
      if (xyzm_from_hex(raw) < 0) {
        xyzm_throw_protocol_async();
      }
      probe->digits[probe->parse_index++] = raw;
      if (probe->parse_index < 14u) {
        co_return;
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
        xyzm_throw_protocol_async();
      }
      probe->parse_state = XYZM_ZMODEM_PARSE_HEADER_HEX_TRAILER;
      probe->trailer_len = 0u;
      probe->expected_trailer_len =
          ((probe->type != XYZM_ZFIN) && (probe->type != XYZM_ZACK)) ? 3u : 2u;
      co_return;

    case XYZM_ZMODEM_PARSE_HEADER_HEX_TRAILER:
      if (probe->trailer_len == 0u) {
        if ((raw != '\r') && (raw != '\n')) {
          xyzm_throw_protocol_async();
        }
      } else if (probe->trailer_len == 1u) {
        if ((raw != '\n') && (raw != 0x8au)) {
          xyzm_throw_protocol_async();
        }
      } else if ((raw != 0x11u) && (raw != 0x91u)) {
        xyzm_throw_protocol_async();
      }
      probe->trailer_len += 1u;
      if (probe->trailer_len < probe->expected_trailer_len) {
        co_return;
      }
      *header_ready = true;
      xyzm_zmodem_control_header_ready(probe, type, hdr);
      co_return;

    default:
      xyzm_throw_io_async();
  }
}

static inline cardio::promise<void> xyzm_zmodem_send_bytes_async(
    xyzm_async_runtime_t &runtime,
    const uint8_t *buf,
    size_t len,
    xyzm_zmodem_send_state_t *state) {
  co_await xyzm_send_exact_async(
      runtime,
      std::span<const uint8_t>(buf, len),
      runtime.options->block_timeout_ms);
  if ((len > 0u) && (state != NULL)) {
    state->last_wire_byte = buf[len - 1u];
    state->has_last_wire_byte = true;
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_control_pump_async(
    xyzm_async_runtime_t &runtime,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  *resume_requested = false;
  if ((pending != NULL) && pending->available) {
    co_return;
  }

  for (;;) {
    uint8_t raw;
    size_t read_len = 0u;

    try {
      co_await xyzm_link_recv_async(
          runtime,
          std::span<uint8_t>(&raw, 1u),
          0u,
          read_len);
    } catch (const xyzm_async_timeout_error &) {
      co_return;
    }
    if (read_len == 0u) {
      xyzm_throw_io_async();
    }

    bool header_ready;
    uint8_t type;
    uint8_t hdr[4];
    co_await xyzm_zmodem_control_probe_feed_async(
        probe,
        raw,
        &header_ready,
        &type,
        hdr);
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
      co_return;
    }
    if (type == XYZM_ZRINIT) {
      if (pending != NULL) {
        pending->type = type;
        memcpy(pending->hdr, hdr, 4u);
        pending->available = true;
      }
      co_return;
    }
    if ((type == XYZM_ZFIN) || (type == XYZM_ZSKIP)) {
      xyzm_throw_remote_cancelled_async();
    }
  }
}

static inline cardio::promise<void> xyzm_zmodem_send_bytes_with_control_async(
    xyzm_async_runtime_t &runtime,
    const uint8_t *buf,
    size_t len,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  uint64_t deadline_ms =
      runtime.link->now_ms() +
      (uint64_t)runtime.options->block_timeout_ms;
  size_t sent_len = 0u;

  while (sent_len < len) {
    uint64_t now_ms = runtime.link->now_ms();
    uint32_t remaining_ms;
    if (now_ms >= deadline_ms) {
      xyzm_throw_timeout_async();
    }
    remaining_ms = ((deadline_ms - now_ms) > 0xffffffffu) ?
        0xffffffffu :
        (uint32_t)(deadline_ms - now_ms);
    if (remaining_ms == 0u) {
      xyzm_throw_timeout_async();
    }
    size_t written_len = 0u;
    co_await xyzm_link_send_async(
        runtime,
        std::span<const uint8_t>(buf + sent_len, len - sent_len),
        remaining_ms,
        written_len);
    if (written_len == 0u) {
      xyzm_throw_io_async();
    }
    if (state != NULL) {
      state->last_wire_byte = buf[sent_len + written_len - 1u];
      state->has_last_wire_byte = true;
    }
    sent_len += written_len;

    co_await xyzm_zmodem_send_control_pump_async(
        runtime,
        probe,
        pending,
        resume_requested);
    if (*resume_requested) {
      co_return;
    }
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_wire_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t value,
    xyzm_zmodem_send_state_t *state) {
  co_await xyzm_zmodem_send_bytes_async(runtime, &value, 1u, state);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_wire_byte_with_control_async(
    xyzm_async_runtime_t &runtime,
    uint8_t value,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  co_await xyzm_zmodem_send_bytes_with_control_async(
      runtime,
      &value,
      1u,
      state,
      probe,
      pending,
      resume_requested);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_raw_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &value,
    uint32_t timeout_ms) {
  co_await xyzm_recv_byte_async(runtime, value, timeout_ms);
  co_return;
}

static inline bool xyzm_zmodem_recv_cursor_take_raw(
    xyzm_zmodem_recv_cursor_t *cursor,
    uint8_t &value) {
  return xyzm_consume_recv_cache_async(
      *cursor->runtime,
      std::span<uint8_t>(&value, 1u)) == 1u;
}

static inline bool xyzm_zmodem_is_data_escape_or_flow(uint8_t value) {
  return value == XYZM_ZDLE || xyzm_zmodem_is_flow_control(value);
}

static inline void xyzm_zmodem_discard_recv_cache_bytes(
    xyzm_async_runtime_t &runtime,
    size_t len) {
  runtime.recv_cache_offset += len;
  if (runtime.recv_cache_offset == runtime.recv_cache_size) {
    runtime.recv_cache_offset = 0u;
    runtime.recv_cache_size = 0u;
  }
}

static inline cardio::promise<void> xyzm_zmodem_recv_cursor_refill_async(
    xyzm_zmodem_recv_cursor_t *cursor) {
  co_await xyzm_refill_recv_cache_async(*cursor->runtime, cursor->timeout_ms);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_cursor_raw_async(
    xyzm_zmodem_recv_cursor_t *cursor,
    uint8_t &value) {
  while (!xyzm_zmodem_recv_cursor_take_raw(cursor, value)) {
    co_await xyzm_zmodem_recv_cursor_refill_async(cursor);
  }
  co_return;
}

static inline bool xyzm_zmodem_decode_zdle_value(
    uint8_t raw,
    int &value) {
  if (raw == XYZM_ZRUB0) {
    value = 0x7f;
  } else if (raw == XYZM_ZRUB1) {
    value = 0xff;
  } else if (raw == XYZM_ZCRCE) {
    value = XYZM_GOTCRCE;
  } else if (raw == XYZM_ZCRCG) {
    value = XYZM_GOTCRCG;
  } else if (raw == XYZM_ZCRCQ) {
    value = XYZM_GOTCRCQ;
  } else if (raw == XYZM_ZCRCW) {
    value = XYZM_GOTCRCW;
  } else {
    value = raw ^ 0x40u;
  }
  return true;
}

static inline bool xyzm_zmodem_recv_cursor_try_zdle(
    xyzm_zmodem_recv_cursor_t *cursor,
    int &value,
    bool &after_zdle) {
  uint8_t raw;
  while (xyzm_zmodem_recv_cursor_take_raw(cursor, raw)) {
    if (xyzm_zmodem_is_flow_control(raw)) {
      continue;
    }
    if (!after_zdle) {
      if (raw != XYZM_ZDLE) {
        value = raw;
        return true;
      }
      after_zdle = true;
      continue;
    }
    after_zdle = false;
    return xyzm_zmodem_decode_zdle_value(raw, value);
  }
  return false;
}

static inline cardio::promise<void> xyzm_zmodem_recv_cursor_zdle_async(
    xyzm_zmodem_recv_cursor_t *cursor,
    int &value) {
  bool after_zdle = false;
  while (!xyzm_zmodem_recv_cursor_try_zdle(cursor, value, after_zdle)) {
    co_await xyzm_zmodem_recv_cursor_refill_async(cursor);
  }
  co_return;
}

static inline uint16_t xyzm_zmodem_packet_len(
    const xyzm_async_runtime_t &runtime,
    uint16_t default_len,
    uint16_t max_len) {
  uint16_t packet_len = runtime.options->zmodem_packet_len;
  if (packet_len == 0u) {
    return default_len;
  }
  if (packet_len < 32u) {
    return 32u;
  }
  if (packet_len > max_len) {
    return max_len;
  }
  return packet_len;
}

static inline uint16_t xyzm_zmodem_send_packet_len(const xyzm_async_runtime_t &runtime) {
  return xyzm_zmodem_packet_len(
      runtime,
      XYZM_ZMODEM_ASYNC_SEND_PACKET_LEN,
      XYZM_ZMODEM_ASYNC_SEND_PACKET_LEN);
}

static inline uint16_t xyzm_zmodem_recv_packet_len(const xyzm_async_runtime_t &runtime) {
  return xyzm_zmodem_packet_len(
      runtime,
      XYZM_ZMODEM_ASYNC_RECV_PACKET_LEN,
      XYZM_ZMODEM_ASYNC_RECV_PACKET_LEN);
}

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

static inline cardio::promise<void> xyzm_zmodem_send_escaped_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t value,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  if (!xyzm_zmodem_needs_escape(value, escape_ctrl, state)) {
    co_await xyzm_zmodem_send_wire_byte_async(runtime, value, state);
    co_return;
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
  co_await xyzm_zmodem_send_bytes_async(runtime, pair, sizeof(pair), state);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_escaped_byte_with_control_async(
    xyzm_async_runtime_t &runtime,
    uint8_t value,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  if (!xyzm_zmodem_needs_escape(value, escape_ctrl, state)) {
    co_await xyzm_zmodem_send_wire_byte_with_control_async(
        runtime,
        value,
        state,
        probe,
        pending,
        resume_requested);
    co_return;
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
  co_await xyzm_zmodem_send_bytes_with_control_async(
      runtime,
      pair,
      sizeof(pair),
      state,
      probe,
      pending,
      resume_requested);
  co_return;
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

static inline cardio::promise<void> xyzm_zmodem_flush_wire_batch_with_control_async(
    xyzm_async_runtime_t &runtime,
    xyzm_zmodem_wire_batch_t *batch,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  if (batch->len == 0u) {
    co_return;
  }
  co_await xyzm_zmodem_send_bytes_with_control_async(
      runtime,
      batch->bytes,
      batch->len,
      NULL,
      probe,
      pending,
      resume_requested);
  batch->len = 0u;
  co_return;
}

static inline bool xyzm_zmodem_try_append_wire_byte_to_batch(
    xyzm_zmodem_wire_batch_t *batch,
    uint8_t value,
    xyzm_zmodem_send_state_t *state) {
  if (batch->len == sizeof(batch->bytes)) {
    return false;
  }
  batch->bytes[batch->len++] = value;
  xyzm_zmodem_record_wire_byte(state, value);
  return true;
}

static inline size_t xyzm_zmodem_encode_escaped_byte(
    uint8_t value,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state,
    uint8_t wire_bytes[2]) {
  if (!xyzm_zmodem_needs_escape(value, escape_ctrl, state)) {
    wire_bytes[0] = value;
    return 1u;
  }

  wire_bytes[0] = XYZM_ZDLE;
  if (value == 0x7fu) {
    wire_bytes[1] = XYZM_ZRUB0;
  } else if (value == 0xffu) {
    wire_bytes[1] = XYZM_ZRUB1;
  } else {
    wire_bytes[1] = (uint8_t)(value ^ 0x40u);
  }
  return 2u;
}

static inline cardio::promise<void> xyzm_zmodem_send_hex_header_async(
    xyzm_async_runtime_t &runtime,
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
  co_await xyzm_zmodem_send_bytes_async(runtime, frame, index, state);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_binary32_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t type,
    const uint8_t hdr[4],
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZPAD, state);
  co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZDLE, state);
  co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZBIN32, state);

  uint32_t crc = xyzm_zcrc32_update(0xffffffffu, type);
  co_await xyzm_zmodem_send_escaped_byte_async(runtime, type, escape_ctrl, state);
  for (size_t index = 0u; index < 4u; ++index) {
    crc = xyzm_zcrc32_update(crc, hdr[index]);
    co_await xyzm_zmodem_send_escaped_byte_async(runtime, hdr[index], escape_ctrl, state);
  }

  crc = ~crc;
  for (size_t index = 0u; index < 4u; ++index) {
    uint8_t value = (uint8_t)((crc >> (index * 8u)) & 0xffu);
    co_await xyzm_zmodem_send_escaped_byte_async(runtime, value, escape_ctrl, state);
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_binary16_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t type,
    const uint8_t hdr[4],
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZPAD, state);
  co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZDLE, state);
  co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZBIN, state);

  uint16_t crc = xyzm_zmodem_crc16_update(0u, type);
  co_await xyzm_zmodem_send_escaped_byte_async(runtime, type, escape_ctrl, state);
  for (size_t index = 0u; index < 4u; ++index) {
    crc = xyzm_zmodem_crc16_update(crc, hdr[index]);
    co_await xyzm_zmodem_send_escaped_byte_async(runtime, hdr[index], escape_ctrl, state);
  }

  crc = xyzm_zmodem_crc16_update(crc, 0u);
  crc = xyzm_zmodem_crc16_update(crc, 0u);
  co_await xyzm_zmodem_send_escaped_byte_async(runtime, (uint8_t)(crc >> 8), escape_ctrl, state);
  co_await xyzm_zmodem_send_escaped_byte_async(runtime, (uint8_t)crc, escape_ctrl, state);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_data_subpacket_async(
    xyzm_async_runtime_t &runtime,
    const uint8_t *buf,
    size_t len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state) {
  if (use_crc32) {
    uint32_t crc;

    crc = 0xffffffffu;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zcrc32_update(crc, buf[index]);
      co_await xyzm_zmodem_send_escaped_byte_async(runtime, buf[index], escape_ctrl, state);
    }

    crc = xyzm_zcrc32_update(crc, frame_end);
    co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZDLE, state);
    co_await xyzm_zmodem_send_wire_byte_async(runtime, frame_end, state);

    crc = ~crc;
    for (size_t index = 0u; index < 4u; ++index) {
      uint8_t value = (uint8_t)((crc >> (index * 8u)) & 0xffu);
      co_await xyzm_zmodem_send_escaped_byte_async(runtime, value, escape_ctrl, state);
    }
  } else {
    uint16_t crc = 0u;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zmodem_crc16_update(crc, buf[index]);
      co_await xyzm_zmodem_send_escaped_byte_async(runtime, buf[index], escape_ctrl, state);
    }

    crc = xyzm_zmodem_crc16_update(crc, frame_end);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_ZDLE, state);
    co_await xyzm_zmodem_send_wire_byte_async(runtime, frame_end, state);
    co_await xyzm_zmodem_send_escaped_byte_async(runtime, (uint8_t)(crc >> 8), escape_ctrl, state);
    co_await xyzm_zmodem_send_escaped_byte_async(runtime, (uint8_t)crc, escape_ctrl, state);
  }
  if (frame_end == XYZM_ZCRCW) {
    co_await xyzm_zmodem_send_wire_byte_async(runtime, XYZM_XON, state);
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_data_subpacket_with_control_async(
    xyzm_async_runtime_t &runtime,
    const uint8_t *buf,
    size_t len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl,
    xyzm_zmodem_send_state_t *state,
    xyzm_zmodem_control_probe_t *probe,
    xyzm_zmodem_pending_header_t *pending,
    bool *resume_requested) {
  *resume_requested = false;
  xyzm_zmodem_wire_batch_t batch = {};

#define XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE(value_expr) \
  do { \
    const uint8_t xyzm_buffered_wire_value = (uint8_t)(value_expr); \
    if (!xyzm_zmodem_try_append_wire_byte_to_batch( \
            &batch, \
            xyzm_buffered_wire_value, \
            state)) { \
      co_await xyzm_zmodem_flush_wire_batch_with_control_async( \
          runtime, \
          &batch, \
          probe, \
          pending, \
          resume_requested); \
      if (*resume_requested) { \
        co_return; \
      } \
      if (!xyzm_zmodem_try_append_wire_byte_to_batch( \
              &batch, \
              xyzm_buffered_wire_value, \
              state)) { \
        xyzm_throw_io_async(); \
      } \
    } \
  } while (false)

#define XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE(value_expr) \
  do { \
    uint8_t xyzm_buffered_wire_bytes[2]; \
    const size_t xyzm_buffered_wire_len = xyzm_zmodem_encode_escaped_byte( \
        (uint8_t)(value_expr), \
        escape_ctrl, \
        state, \
        xyzm_buffered_wire_bytes); \
    for (size_t xyzm_buffered_wire_index = 0u; \
         xyzm_buffered_wire_index < xyzm_buffered_wire_len; \
         ++xyzm_buffered_wire_index) { \
      XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE( \
          xyzm_buffered_wire_bytes[xyzm_buffered_wire_index]); \
    } \
  } while (false)

  if (use_crc32) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zcrc32_update(crc, buf[index]);
      XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE(buf[index]);
    }

    crc = xyzm_zcrc32_update(crc, frame_end);
    XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE(XYZM_ZDLE);
    XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE(frame_end);

    crc = ~crc;
    for (size_t index = 0u; index < 4u; ++index) {
      uint8_t value = (uint8_t)((crc >> (index * 8u)) & 0xffu);
      XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE(value);
    }
  } else {
    uint16_t crc = 0u;
    for (size_t index = 0u; index < len; ++index) {
      crc = xyzm_zmodem_crc16_update(crc, buf[index]);
      XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE(buf[index]);
    }

    crc = xyzm_zmodem_crc16_update(crc, frame_end);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    crc = xyzm_zmodem_crc16_update(crc, 0u);
    XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE(XYZM_ZDLE);
    XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE(frame_end);
    XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE((uint8_t)(crc >> 8));
    XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE((uint8_t)crc);
  }
  if (frame_end == XYZM_ZCRCW) {
    XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE(XYZM_XON);
  }
#undef XYZM_ZMODEM_APPEND_BUFFERED_ESCAPED_BYTE
#undef XYZM_ZMODEM_APPEND_BUFFERED_WIRE_BYTE

  co_await xyzm_zmodem_flush_wire_batch_with_control_async(
      runtime,
      &batch,
      probe,
      pending,
      resume_requested);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_zdle_async(
    xyzm_async_runtime_t &runtime,
    int &value,
    uint32_t timeout_ms) {
  uint8_t raw;
  for (;;) {
    co_await xyzm_zmodem_recv_raw_async(runtime, raw, timeout_ms);
    if (!xyzm_zmodem_is_flow_control(raw)) {
      break;
    }
  }
  if (raw != XYZM_ZDLE) {
    value = raw;
    co_return;
  }

  for (;;) {
    co_await xyzm_zmodem_recv_raw_async(runtime, raw, timeout_ms);
    if (!xyzm_zmodem_is_flow_control(raw)) {
      break;
    }
  }
  if (raw == XYZM_ZRUB0) {
    value = 0x7f;
  } else if (raw == XYZM_ZRUB1) {
    value = 0xff;
  } else if (raw == XYZM_ZCRCE) {
    value = XYZM_GOTCRCE;
  } else if (raw == XYZM_ZCRCG) {
    value = XYZM_GOTCRCG;
  } else if (raw == XYZM_ZCRCQ) {
    value = XYZM_GOTCRCQ;
  } else if (raw == XYZM_ZCRCW) {
    value = XYZM_GOTCRCW;
  } else {
    value = raw ^ 0x40u;
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_binary32_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &type,
    uint8_t hdr[4]) {
  int value;
  co_await xyzm_zmodem_recv_zdle_async(runtime, value, runtime.options->block_timeout_ms);
  type = (uint8_t)value;
  uint32_t crc = xyzm_zcrc32_update(0xffffffffu, type);

  for (size_t index = 0u; index < 4u; ++index) {
    co_await xyzm_zmodem_recv_zdle_async(runtime, value, runtime.options->block_timeout_ms);
    hdr[index] = (uint8_t)value;
    crc = xyzm_zcrc32_update(crc, hdr[index]);
  }
  for (size_t index = 0u; index < 4u; ++index) {
    co_await xyzm_zmodem_recv_zdle_async(runtime, value, runtime.options->block_timeout_ms);
    crc = xyzm_zcrc32_update(crc, (uint8_t)value);
  }
  if (crc != 0xdebb20e3u) {
    xyzm_throw_protocol_async();
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_binary16_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &type,
    uint8_t hdr[4]) {
  int value;
  co_await xyzm_zmodem_recv_zdle_async(runtime, value, runtime.options->block_timeout_ms);
  type = (uint8_t)value;
  uint16_t crc = xyzm_zmodem_crc16_update(0u, type);

  for (size_t index = 0u; index < 4u; ++index) {
    co_await xyzm_zmodem_recv_zdle_async(runtime, value, runtime.options->block_timeout_ms);
    hdr[index] = (uint8_t)value;
    crc = xyzm_zmodem_crc16_update(crc, hdr[index]);
  }
  for (size_t index = 0u; index < 2u; ++index) {
    co_await xyzm_zmodem_recv_zdle_async(runtime, value, runtime.options->block_timeout_ms);
    crc = xyzm_zmodem_crc16_update(crc, (uint8_t)value);
  }
  if (crc != 0u) {
    xyzm_throw_protocol_async();
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_hex_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &type,
    uint8_t hdr[4]) {
  uint8_t digits[14];
  for (size_t index = 0u; index < sizeof(digits); ++index) {
    co_await xyzm_zmodem_recv_raw_async(runtime, digits[index], runtime.options->block_timeout_ms);
    if (xyzm_from_hex(digits[index]) < 0) {
      xyzm_throw_protocol_async();
    }
  }

  type = (uint8_t)((xyzm_from_hex(digits[0]) << 4) | xyzm_from_hex(digits[1]));
  for (size_t index = 0u; index < 4u; ++index) {
    hdr[index] = (uint8_t)((xyzm_from_hex(digits[2u + index * 2u]) << 4) |
        xyzm_from_hex(digits[3u + index * 2u]));
  }
  uint16_t actual_crc = (uint16_t)((xyzm_from_hex(digits[10]) << 12) |
      (xyzm_from_hex(digits[11]) << 8) |
      (xyzm_from_hex(digits[12]) << 4) |
      xyzm_from_hex(digits[13]));
  uint16_t expected_crc = xyzm_zmodem_crc16_header(type, hdr);
  if (actual_crc != expected_crc) {
    xyzm_throw_protocol_async();
  }

  uint8_t trailer;
  co_await xyzm_zmodem_recv_raw_async(runtime, trailer, 10u);
  if ((trailer != '\r') && (trailer != '\n')) {
    xyzm_throw_protocol_async();
  }
  co_await xyzm_zmodem_recv_raw_async(runtime, trailer, 10u);
  if ((trailer != '\n') && (trailer != 0x8au)) {
    xyzm_throw_protocol_async();
  }
  if ((type != XYZM_ZFIN) && (type != XYZM_ZACK)) {
    co_await xyzm_zmodem_recv_raw_async(runtime, trailer, 10u);
    if ((trailer != 0x11u) && (trailer != 0x91u)) {
      xyzm_throw_protocol_async();
    }
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_recv_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &type,
    uint8_t hdr[4],
    uint8_t *frame_kind,
    uint32_t timeout_ms) {
  uint64_t deadline_ms = runtime.link->now_ms() + timeout_ms;
  for (;;) {
    uint32_t remaining_ms = (uint32_t)(deadline_ms - runtime.link->now_ms());
    if ((int32_t)remaining_ms <= 0) {
      xyzm_throw_timeout_async();
    }

    uint8_t byte0;
    co_await xyzm_zmodem_recv_raw_async(runtime, byte0, remaining_ms);
    if (byte0 != XYZM_ZPAD) {
      continue;
    }

    co_await xyzm_zmodem_recv_raw_async(runtime, byte0, remaining_ms);
    if (byte0 == XYZM_ZPAD) {
      co_await xyzm_zmodem_recv_raw_async(runtime, byte0, remaining_ms);
    }
    if (byte0 != XYZM_ZDLE) {
      continue;
    }

    co_await xyzm_zmodem_recv_raw_async(runtime, byte0, remaining_ms);
    if (frame_kind != NULL) {
      *frame_kind = byte0;
    }
    if (byte0 == XYZM_ZBIN) {
      co_await xyzm_zmodem_recv_binary16_header_async(runtime, type, hdr);
      co_return;
    }
    if (byte0 == XYZM_ZHEX) {
      co_await xyzm_zmodem_recv_hex_header_async(runtime, type, hdr);
      co_return;
    }
    if (byte0 == XYZM_ZBIN32) {
      co_await xyzm_zmodem_recv_binary32_header_async(runtime, type, hdr);
      co_return;
    }
  }
}

static inline cardio::promise<void> xyzm_zmodem_recv_data_async(
    xyzm_async_runtime_t &runtime,
    uint8_t *buf,
    size_t capacity,
    size_t &received_len,
    uint8_t frame_kind,
    int &frame_end) {
  const bool use_crc32 = frame_kind == XYZM_ZBIN32;
  uint32_t crc32 = 0xffffffffu;
  uint16_t crc16 = 0u;
  size_t used_len = 0u;
  xyzm_zmodem_recv_cursor_t cursor = {
      &runtime,
      runtime.options->block_timeout_ms};
  bool after_zdle = false;
  for (;;) {
    int value;

    if (!after_zdle &&
        runtime.recv_cache_offset < runtime.recv_cache_size &&
        used_len < capacity) {
      const uint8_t *cached =
          runtime.recv_cache.data() + runtime.recv_cache_offset;
      const size_t available_len =
          runtime.recv_cache_size - runtime.recv_cache_offset;
      const size_t capacity_len = capacity - used_len;
      size_t plain_len = 0u;
      while (plain_len < available_len &&
             plain_len < capacity_len &&
             !xyzm_zmodem_is_data_escape_or_flow(cached[plain_len])) {
        plain_len += 1u;
      }
      if (plain_len != 0u) {
        memcpy(buf + used_len, cached, plain_len);
        if (use_crc32) {
          crc32 = xyzm_zcrc32_update_buffer(crc32, cached, plain_len);
        } else {
          crc16 = xyzm_zmodem_crc16_update_buffer(crc16, cached, plain_len);
        }
        used_len += plain_len;
        xyzm_zmodem_discard_recv_cache_bytes(runtime, plain_len);
        continue;
      }
    }

    if (!xyzm_zmodem_recv_cursor_try_zdle(&cursor, value, after_zdle)) {
      co_await xyzm_zmodem_recv_cursor_refill_async(&cursor);
      continue;
    }

    if ((value & 0x400) == 0) {
      if (used_len >= capacity) {
        xyzm_throw_protocol_async();
      }
      buf[used_len++] = (uint8_t)value;
      if (use_crc32) {
        crc32 = xyzm_zcrc32_update(crc32, (uint8_t)value);
      } else {
        crc16 = xyzm_zmodem_crc16_update(crc16, (uint8_t)value);
      }
      continue;
    }

    frame_end = value;
    if (use_crc32) {
      crc32 = xyzm_zcrc32_update(crc32, (uint8_t)(value & 0xff));
    } else {
      crc16 = xyzm_zmodem_crc16_update(crc16, (uint8_t)(value & 0xff));
    }
    for (size_t index = 0u; index < (use_crc32 ? 4u : 2u); ++index) {
      bool crc_after_zdle = false;
      while (!xyzm_zmodem_recv_cursor_try_zdle(&cursor, value, crc_after_zdle)) {
        co_await xyzm_zmodem_recv_cursor_refill_async(&cursor);
      }
      if ((value & 0x400) != 0) {
        xyzm_throw_protocol_async();
      }
      if (use_crc32) {
        crc32 = xyzm_zcrc32_update(crc32, (uint8_t)value);
      } else {
        crc16 = xyzm_zmodem_crc16_update(crc16, (uint8_t)value);
      }
    }
    if (use_crc32 && (crc32 != 0xdebb20e3u)) {
      xyzm_throw_protocol_async();
    }
    if (!use_crc32 && (crc16 != 0u)) {
      xyzm_throw_protocol_async();
    }
    received_len = used_len;
    co_return;
  }
}

static inline void xyzm_zmodem_build_file_info(
    xyzm_async_receive_file_t *file,
    const uint8_t *metadata,
    size_t metadata_len) {
  *file = {};
  size_t name_len = 0u;
  while ((name_len < metadata_len) && (metadata[name_len] != '\0')) {
    name_len += 1u;
  }
  if (name_len > 0u) {
    file->info.name = std::string((const char *)metadata, name_len);
  }
  if ((name_len + 1u) < metadata_len && metadata[name_len + 1u] != '\0') {
    unsigned long long size_value = strtoull((const char *)(metadata + name_len + 1u), NULL, 10);
    file->info.size_bytes = (uint64_t)size_value;
  }
}

static inline size_t xyzm_zmodem_build_metadata(
    const xyzm_async_send_file_t *file,
    uint8_t *buf,
    size_t capacity) {
  const char *name = file->info.name.has_value() && !file->info.name->empty() ?
      file->info.name->c_str() :
      "file.bin";
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
      (unsigned long long)file->info.size_bytes.value_or(0u));
  if (written_len < 0) {
    return 0u;
  }
  return name_len + 1u + (size_t)written_len + 1u;
}

static inline cardio::promise<void> xyzm_zmodem_wait_for_type_async(
    xyzm_async_runtime_t &runtime,
    uint8_t expected_type,
    uint8_t hdr[4]) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t type;
    try {
      co_await xyzm_zmodem_recv_header_async(
          runtime,
          type,
          hdr,
          NULL,
          runtime.options->handshake_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (type == expected_type) {
      co_return;
    }
    if ((type == XYZM_ZFIN) || (type == XYZM_ZSKIP)) {
      /*
       * Forsberg's ZMODEM specification defines ZSKIP as the receiver response
       * to ZFILE that tells the sender to skip the current file. This
       * single-file API has no "next file" to continue with, so surface the peer
       * skip as a remote-cancelled terminal failure for now.
       */
      xyzm_throw_remote_cancelled_async();
    }
  }
  xyzm_throw_timeout_async();
}

static inline cardio::promise<void> xyzm_zmodem_wait_for_type_with_pending_async(
    xyzm_async_runtime_t &runtime,
    uint8_t expected_type,
    uint8_t hdr[4],
    xyzm_zmodem_pending_header_t *pending) {
  if ((pending != NULL) && pending->available) {
    const uint8_t type = pending->type;
    memcpy(hdr, pending->hdr, 4u);
    pending->available = false;
    if (type == expected_type) {
      co_return;
    }
    if ((type == XYZM_ZFIN) || (type == XYZM_ZSKIP)) {
      xyzm_throw_remote_cancelled_async();
    }
    xyzm_throw_protocol_async();
  }
  co_await xyzm_zmodem_wait_for_type_async(runtime, expected_type, hdr);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_begin_session_async(
    xyzm_async_runtime_t &runtime,
    xyzm_zmodem_send_state_t *send_state,
    bool *use_crc32,
    bool *escape_ctrl,
    size_t *packet_len) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZRQINIT, hdr, send_state);
  co_await xyzm_zmodem_wait_for_type_async(runtime, XYZM_ZRINIT, hdr);
  *use_crc32 = (runtime.options->zmodem_use_crc32 != 0u) &&
      ((hdr[3] & XYZM_CANFC32) != 0u);
  *escape_ctrl = (runtime.options->zmodem_escape_ctrl != 0u) ||
      ((hdr[3] & XYZM_ESCCTL) != 0u);
  *packet_len = xyzm_zmodem_send_packet_len(runtime);
  uint16_t peer_packet_len = (uint16_t)((uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8));
  if ((peer_packet_len >= 32u) && (peer_packet_len < *packet_len)) {
    *packet_len = peer_packet_len;
  }
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_apply_zrpos_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    const uint8_t hdr[4],
    uint64_t *payload_bytes) {
  uint64_t offset = xyzm_zmodem_rclhdr(hdr);
  if (file->info.size_bytes.has_value() && (offset > *file->info.size_bytes)) {
    xyzm_throw_protocol_async();
  }
  if (offset != 0u) {
    if (!source->seek) {
      xyzm_throw_unsupported_async();
    }
    co_await xyzm_source_seek_async(runtime, source, offset);
  }
  *payload_bytes = offset;
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_one_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    xyzm_zmodem_send_state_t *send_state,
    bool use_crc32,
    bool escape_ctrl,
    size_t packet_len) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  hdr[3] = XYZM_ZCBIN;
  uint8_t metadata[512];
  size_t metadata_len = xyzm_zmodem_build_metadata(file, metadata, sizeof(metadata));
  if (metadata_len == 0u) {
    xyzm_throw_argument_async();
  }
  if (use_crc32) {
    co_await xyzm_zmodem_send_binary32_header_async(
        runtime,
        XYZM_ZFILE,
        hdr,
        escape_ctrl,
        send_state);
  } else {
    co_await xyzm_zmodem_send_binary16_header_async(
        runtime,
        XYZM_ZFILE,
        hdr,
        escape_ctrl,
        send_state);
  }
  co_await xyzm_zmodem_send_data_subpacket_async(
      runtime,
      metadata,
      metadata_len,
      XYZM_ZCRCW,
      use_crc32,
      escape_ctrl,
      send_state);
  co_await xyzm_zmodem_wait_for_type_async(runtime, XYZM_ZRPOS, hdr);
  uint64_t payload_bytes = 0u;
  co_await xyzm_zmodem_send_apply_zrpos_async(
      runtime,
      source,
      file,
      hdr,
      &payload_bytes);
  xyzm_zmodem_control_probe_t control_probe;
  xyzm_zmodem_pending_header_t pending_header;
  xyzm_zmodem_control_probe_init(&control_probe);
  memset(&pending_header, 0, sizeof(pending_header));

  for (;;) {
    bool resume_requested = false;

    xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
    if (use_crc32) {
      co_await xyzm_zmodem_send_binary32_header_async(
          runtime,
          XYZM_ZDATA,
          hdr,
          escape_ctrl,
          send_state);
    } else {
      co_await xyzm_zmodem_send_binary16_header_async(
          runtime,
          XYZM_ZDATA,
          hdr,
          escape_ctrl,
          send_state);
    }

    for (;;) {
      uint8_t payload[XYZM_ZMODEM_ASYNC_SEND_PACKET_LEN];
      size_t total_len = 0u;
      uint8_t frame_end;
      for (;;) {
        size_t read_len = 0u;
        xyzm_async_status_t source_status = co_await xyzm_source_read_async(
            runtime,
            source,
            std::span<uint8_t>(payload + total_len, packet_len - total_len),
            read_len);
        if (source_status == xyzm_async_status_t::end) {
          break;
        }
        if (read_len == 0u) {
          xyzm_throw_io_async();
        }
        total_len += read_len;
        if (total_len == packet_len) {
          break;
        }
      }

      if (total_len == 0u) {
        co_await xyzm_zmodem_send_data_subpacket_with_control_async(
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
      co_await xyzm_zmodem_send_data_subpacket_with_control_async(
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
      if (resume_requested) {
        if (!pending_header.available || (pending_header.type != XYZM_ZRPOS)) {
          xyzm_throw_protocol_async();
        }
        co_await xyzm_zmodem_send_apply_zrpos_async(
            runtime,
            source,
            file,
            pending_header.hdr,
            &payload_bytes);
        pending_header.available = false;
        xyzm_zmodem_control_probe_init(&control_probe);
        break;
      }
      xyzm_report_payload_async(runtime, total_len, file->info);
      payload_bytes += total_len;
      if (frame_end == XYZM_ZCRCE) {
        break;
      }
    }

    if (resume_requested && pending_header.available) {
      if (pending_header.type != XYZM_ZRPOS) {
        xyzm_throw_protocol_async();
      }
      co_await xyzm_zmodem_send_apply_zrpos_async(
          runtime,
          source,
          file,
          pending_header.hdr,
          &payload_bytes);
      pending_header.available = false;
      xyzm_zmodem_control_probe_init(&control_probe);
    }
    if (!resume_requested) {
      break;
    }
  }

  xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
  if (use_crc32) {
    co_await xyzm_zmodem_send_binary32_header_async(
        runtime,
        XYZM_ZEOF,
        hdr,
        escape_ctrl,
        send_state);
  } else {
    co_await xyzm_zmodem_send_binary16_header_async(
        runtime,
        XYZM_ZEOF,
        hdr,
        escape_ctrl,
        send_state);
  }
  co_await xyzm_zmodem_wait_for_type_with_pending_async(
      runtime,
      XYZM_ZRINIT,
      hdr,
      &pending_header);
  co_return;
}

static inline cardio::promise<void> xyzm_zmodem_send_finish_session_async(
    xyzm_async_runtime_t &runtime,
    xyzm_zmodem_send_state_t *send_state) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZFIN, hdr, send_state);
  co_await xyzm_zmodem_wait_for_type_async(runtime, XYZM_ZFIN, hdr);
  co_await xyzm_zmodem_send_bytes_async(runtime, (const uint8_t *)"OO", 2u, send_state);
  co_return;
}

cardio::promise<void> xyzm_zmodem_send_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file) {
  xyzm_zmodem_send_state_t send_state;
  bool use_crc32;
  bool escape_ctrl;
  size_t packet_len;

  memset(&send_state, 0, sizeof(send_state));
  co_await xyzm_zmodem_send_begin_session_async(
      runtime,
      &send_state,
      &use_crc32,
      &escape_ctrl,
      &packet_len);
  co_await xyzm_zmodem_send_one_file_async(
      runtime,
      source,
      file,
      &send_state,
      use_crc32,
      escape_ctrl,
      packet_len);
  co_await xyzm_zmodem_send_finish_session_async(runtime, &send_state);
  xyzm_report_completed_file_async(runtime);
  co_return;
}

cardio::promise<void> xyzm_zmodem_send_batch_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file) {
  xyzm_zmodem_send_state_t send_state;
  bool use_crc32;
  bool escape_ctrl;
  size_t packet_len;

  memset(&send_state, 0, sizeof(send_state));
  co_await xyzm_zmodem_send_begin_session_async(
      runtime,
      &send_state,
      &use_crc32,
      &escape_ctrl,
      &packet_len);
  for (;;) {
    co_await xyzm_zmodem_send_one_file_async(
        runtime,
        source,
        file,
        &send_state,
        use_crc32,
        escape_ctrl,
        packet_len);
    xyzm_report_completed_file_async(runtime);
    co_await xyzm_finish_send_file_async(runtime, source, file, nullptr);
    if ((co_await xyzm_send_single_file_async(runtime, source, file)) ==
        xyzm_async_status_t::end) {
      co_await xyzm_zmodem_send_finish_session_async(runtime, &send_state);
      co_return;
    }
  }
}

static inline cardio::promise<void> xyzm_send_cancel_preserving_zmodem_async(
    xyzm_async_runtime_t &runtime,
    std::exception_ptr error) {
  try {
    co_await xyzm_send_cancel_async(runtime);
  } catch (...) {
  }
  std::rethrow_exception(error);
}

cardio::promise<void> xyzm_zmodem_receive_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink) {
  uint8_t hdr[4];
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = (uint8_t)(xyzm_zmodem_recv_packet_len(runtime) & 0xffu);
  hdr[1] = (uint8_t)(xyzm_zmodem_recv_packet_len(runtime) >> 8);
  hdr[3] = XYZM_CANFDX | XYZM_CANOVIO;
  if (runtime.options->zmodem_use_crc32 != 0u) {
    hdr[3] |= XYZM_CANFC32;
  }
  if (runtime.options->zmodem_escape_ctrl != 0u) {
    hdr[3] |= XYZM_ESCCTL;
  }

  uint8_t type = 0u;
  uint8_t frame_kind = 0u;
  unsigned attempt = 0u;
  bool have_final_zfin = false;
  bool accept_final_zfin = false;
  size_t used_len;
  int frame_end;
  uint8_t buffer[XYZM_ASYNC_RECV_CACHE_SIZE];

  for (;;) {
    co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZRINIT, hdr, NULL);
    try {
      co_await xyzm_zmodem_recv_header_async(
          runtime,
          type,
          hdr,
          &frame_kind,
          runtime.options->handshake_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      if (attempt >= runtime.options->retry_limit) {
        throw;
      }
      attempt += 1u;
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (type == XYZM_ZRQINIT) {
      if (attempt >= runtime.options->retry_limit) {
        xyzm_throw_timeout_async();
      }
      attempt += 1u;
      continue;
    }
    if (type == XYZM_ZFILE) {
      break;
    }
    if (accept_final_zfin && (type == XYZM_ZFIN)) {
      have_final_zfin = true;
      break;
    }
    if (attempt >= runtime.options->retry_limit) {
      xyzm_throw_protocol_async();
    }
    attempt += 1u;
    xyzm_report_retry_async(runtime);
  }

  while (!have_final_zfin) {
    xyzm_async_receive_file_t file{};
    uint64_t resume_offset;
    uint64_t remaining_bytes;
    uint64_t payload_bytes;

    co_await xyzm_zmodem_recv_data_async(runtime, buffer, sizeof(buffer), used_len, frame_kind, frame_end);
    xyzm_zmodem_build_file_info(&file, buffer, used_len);
    resume_offset = 0u;
    std::exception_ptr begin_error;
    try {
      co_await xyzm_sink_begin_async(runtime, sink, file.info, resume_offset);
    } catch (...) {
      begin_error = std::current_exception();
    }
    if (begin_error) {
      co_await xyzm_send_cancel_preserving_zmodem_async(runtime, begin_error);
    }
    file.active = true;
    std::exception_ptr receive_error;
    try {
      if (file.info.size_bytes.has_value() && (resume_offset > *file.info.size_bytes)) {
        try {
          co_await xyzm_send_cancel_async(runtime);
        } catch (...) {
        }
        xyzm_throw_argument_async();
      }
      xyzm_notify_file_started_async(runtime, file.info);

      remaining_bytes = UINT64_MAX;
      if (file.info.size_bytes.has_value()) {
        remaining_bytes = *file.info.size_bytes - resume_offset;
      }

      xyzm_zmodem_stohdr((uint32_t)resume_offset, hdr);
      co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZRPOS, hdr, NULL);

      payload_bytes = resume_offset;
      for (;;) {
        co_await xyzm_zmodem_recv_header_async(
            runtime,
            type,
            hdr,
            &frame_kind,
            runtime.options->handshake_timeout_ms);

        if (type == XYZM_ZDATA) {
          uint64_t offset;

          offset = xyzm_zmodem_rclhdr(hdr);
          if (offset != payload_bytes) {
            xyzm_throw_unsupported_async();
          }
          for (;;) {
            co_await xyzm_zmodem_recv_data_async(
                runtime,
                buffer,
                sizeof(buffer),
                used_len,
                frame_kind,
                frame_end);
            if (used_len > 0u) {
              size_t write_len;

              write_len = used_len;
              if (file.info.size_bytes.has_value()) {
                if (remaining_bytes < write_len) {
                  xyzm_throw_protocol_async();
                }
              }
              if (write_len > 0u) {
                std::exception_ptr write_error;
                try {
                  co_await xyzm_sink_write_async(
                      runtime,
                      sink,
                      std::span<const uint8_t>(buffer, write_len));
                } catch (...) {
                  write_error = std::current_exception();
                }
                if (write_error) {
                  co_await xyzm_send_cancel_preserving_zmodem_async(runtime, write_error);
                }
                xyzm_report_payload_async(runtime, write_len, file.info);
                payload_bytes += write_len;
                if (remaining_bytes != UINT64_MAX) {
                  remaining_bytes -= write_len;
                }
              }
            }

            if (frame_end == XYZM_GOTCRCQ) {
              xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
              co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZACK, hdr, NULL);
              continue;
            }
            if (frame_end == XYZM_GOTCRCW) {
              xyzm_zmodem_stohdr((uint32_t)payload_bytes, hdr);
              co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZACK, hdr, NULL);
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
          if (file.info.size_bytes.has_value()) {
            if (remaining_bytes != 0u) {
              xyzm_throw_protocol_async();
            }
          }
          if (xyzm_zmodem_rclhdr(hdr) != payload_bytes) {
            xyzm_throw_protocol_async();
          }
          co_await xyzm_finish_receive_file_async(runtime, sink, &file, nullptr);
          xyzm_report_completed_file_async(runtime);
          memset(hdr, 0, sizeof(hdr));
          if (runtime.options->batch_mode) {
            co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZRINIT, hdr, NULL);
            attempt = 0u;
            accept_final_zfin = true;
            for (;;) {
              try {
                co_await xyzm_zmodem_recv_header_async(
                    runtime,
                    type,
                    hdr,
                    &frame_kind,
                    runtime.options->handshake_timeout_ms);
              } catch (const xyzm_async_timeout_error &) {
                if (attempt >= runtime.options->retry_limit) {
                  throw;
                }
                attempt += 1u;
                xyzm_report_retry_async(runtime);
                continue;
              }
              if (type == XYZM_ZFILE) {
                break;
              }
              if (accept_final_zfin && (type == XYZM_ZFIN)) {
                have_final_zfin = true;
                break;
              }
              if (attempt >= runtime.options->retry_limit) {
                xyzm_throw_protocol_async();
              }
              attempt += 1u;
              xyzm_report_retry_async(runtime);
            }
            break;
          }
          co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZRINIT, hdr, NULL);
          have_final_zfin = false;
          break;
        }
      }
    } catch (...) {
      receive_error = std::current_exception();
    }
    if (receive_error) {
      co_await xyzm_finish_receive_file_and_rethrow_async(runtime, sink, &file, receive_error);
    }

    if (!runtime.options->batch_mode || have_final_zfin) {
      break;
    }
  }

  if (!have_final_zfin) {
    co_await xyzm_zmodem_recv_header_async(
        runtime,
        type,
        hdr,
        &frame_kind,
        runtime.options->handshake_timeout_ms);
    if (type != XYZM_ZFIN) {
      xyzm_throw_protocol_async();
    }
  }
  memset(hdr, 0, sizeof(hdr));
  co_await xyzm_zmodem_send_hex_header_async(runtime, XYZM_ZFIN, hdr, NULL);
  {
    uint8_t trailer[2];
    try {
      co_await xyzm_recv_exact_async(
          runtime,
          std::span<uint8_t>(trailer, sizeof(trailer)),
          runtime.options->block_timeout_ms);
    } catch (...) {
    }
  }
  co_return;
}
