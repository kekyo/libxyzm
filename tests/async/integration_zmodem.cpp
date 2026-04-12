/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_async_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

static constexpr uint8_t TEST_ASYNC_ZPAD = '*';
static constexpr uint8_t TEST_ASYNC_ZDLE = 0x18u;
static constexpr uint8_t TEST_ASYNC_ZBIN = 'A';
static constexpr uint8_t TEST_ASYNC_ZBIN32 = 'C';
static constexpr uint8_t TEST_ASYNC_ZHEX = 'B';
static constexpr uint8_t TEST_ASYNC_ZRINIT = 1u;
static constexpr uint8_t TEST_ASYNC_ZFILE = 4u;
static constexpr uint8_t TEST_ASYNC_ZFIN = 8u;
static constexpr uint8_t TEST_ASYNC_ZRPOS = 9u;
static constexpr uint8_t TEST_ASYNC_ZDATA = 10u;
static constexpr uint8_t TEST_ASYNC_ZEOF = 11u;
static constexpr uint8_t TEST_ASYNC_ZCRCE = 'h';
static constexpr uint8_t TEST_ASYNC_ZCRCG = 'i';
static constexpr uint8_t TEST_ASYNC_CANFC32 = 0x20u;
static constexpr uint8_t TEST_ASYNC_ESCCTL = 0x40u;
static constexpr uint8_t TEST_ASYNC_XON = 0x11u;
static constexpr uint8_t TEST_ASYNC_XOFF = 0x13u;

static uint8_t test_async_zmodem_hex_digit(unsigned value) {
  return static_cast<uint8_t>(value < 10u ? ('0' + value) : ('a' + (value - 10u)));
}

static uint16_t test_async_zmodem_crc16_entry(uint8_t index) {
  uint16_t crc = static_cast<uint16_t>(index) << 8;
  for (unsigned bit = 0u; bit < 8u; ++bit) {
    if ((crc & 0x8000u) != 0u) {
      crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
    } else {
      crc = static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

static uint16_t test_async_zmodem_crc16_update(uint16_t crc, uint8_t value) {
  return static_cast<uint16_t>(
      test_async_zmodem_crc16_entry(static_cast<uint8_t>(crc >> 8)) ^
      static_cast<uint16_t>(crc << 8) ^
      value);
}

static uint32_t test_async_zmodem_crc32_update(uint32_t crc, uint8_t value) {
  crc ^= static_cast<uint32_t>(value);
  for (unsigned bit = 0u; bit < 8u; ++bit) {
    if ((crc & 1u) != 0u) {
      crc = (crc >> 1) ^ 0xedb88320u;
    } else {
      crc >>= 1;
    }
  }
  return crc;
}

static void test_async_zmodem_stohdr(uint32_t value, uint8_t hdr[4]) {
  hdr[0] = static_cast<uint8_t>(value & 0xffu);
  hdr[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  hdr[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  hdr[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

static void test_async_append_zmodem_hex_header(
    std::vector<uint8_t> *buffer,
    uint8_t type,
    const uint8_t hdr[4]) {
  uint16_t crc = 0u;
  crc = test_async_zmodem_crc16_update(crc, static_cast<uint8_t>(type & 0x7fu));
  for (std::size_t index = 0u; index < 4u; ++index) {
    crc = test_async_zmodem_crc16_update(crc, hdr[index]);
  }
  crc = test_async_zmodem_crc16_update(crc, 0u);
  crc = test_async_zmodem_crc16_update(crc, 0u);

  buffer->push_back(TEST_ASYNC_ZPAD);
  buffer->push_back(TEST_ASYNC_ZPAD);
  buffer->push_back(TEST_ASYNC_ZDLE);
  buffer->push_back(TEST_ASYNC_ZHEX);
  buffer->push_back(test_async_zmodem_hex_digit((type >> 4) & 0x0fu));
  buffer->push_back(test_async_zmodem_hex_digit(type & 0x0fu));
  for (std::size_t index = 0u; index < 4u; ++index) {
    buffer->push_back(test_async_zmodem_hex_digit((hdr[index] >> 4) & 0x0fu));
    buffer->push_back(test_async_zmodem_hex_digit(hdr[index] & 0x0fu));
  }
  for (std::size_t index = 0u; index < 4u; ++index) {
    const unsigned shift = static_cast<unsigned>(12u - index * 4u);
    buffer->push_back(test_async_zmodem_hex_digit((crc >> shift) & 0x0fu));
  }
  buffer->push_back('\r');
  buffer->push_back(static_cast<uint8_t>(0x8au));
  if (type != TEST_ASYNC_ZFIN) {
    buffer->push_back(TEST_ASYNC_XON);
  }
}

static void test_async_append_zmodem_escaped_byte(
    std::vector<uint8_t> *buffer,
    uint8_t value,
    bool escape_ctrl) {
  bool needs_escape =
      value == TEST_ASYNC_ZDLE ||
      value == 0x7fu ||
      value == 0xffu ||
      value == TEST_ASYNC_XON ||
      value == TEST_ASYNC_XOFF ||
      value == static_cast<uint8_t>(TEST_ASYNC_XON | 0x80u) ||
      value == static_cast<uint8_t>(TEST_ASYNC_XOFF | 0x80u);
  if (!needs_escape && escape_ctrl &&
      (value < 0x20u || value == 0x80u || value == 0x90u)) {
    needs_escape = true;
  }
  if (!needs_escape) {
    buffer->push_back(value);
    return;
  }

  buffer->push_back(TEST_ASYNC_ZDLE);
  if (value == 0x7fu) {
    buffer->push_back('l');
  } else if (value == 0xffu) {
    buffer->push_back('m');
  } else {
    buffer->push_back(static_cast<uint8_t>(value ^ 0x40u));
  }
}

static void test_async_append_zmodem_binary_header(
    std::vector<uint8_t> *buffer,
    uint8_t type,
    const uint8_t hdr[4],
    bool use_crc32,
    bool escape_ctrl) {
  buffer->push_back(TEST_ASYNC_ZPAD);
  buffer->push_back(TEST_ASYNC_ZDLE);
  buffer->push_back(use_crc32 ? TEST_ASYNC_ZBIN32 : TEST_ASYNC_ZBIN);

  if (use_crc32) {
    uint32_t crc = 0xffffffffu;
    crc = test_async_zmodem_crc32_update(crc, type);
    test_async_append_zmodem_escaped_byte(buffer, type, escape_ctrl);
    for (std::size_t index = 0u; index < 4u; ++index) {
      crc = test_async_zmodem_crc32_update(crc, hdr[index]);
      test_async_append_zmodem_escaped_byte(buffer, hdr[index], escape_ctrl);
    }
    crc = ~crc;
    for (std::size_t index = 0u; index < 4u; ++index) {
      test_async_append_zmodem_escaped_byte(
          buffer,
          static_cast<uint8_t>((crc >> (index * 8u)) & 0xffu),
          escape_ctrl);
    }
    return;
  }

  uint16_t crc = 0u;
  crc = test_async_zmodem_crc16_update(crc, type);
  test_async_append_zmodem_escaped_byte(buffer, type, escape_ctrl);
  for (std::size_t index = 0u; index < 4u; ++index) {
    crc = test_async_zmodem_crc16_update(crc, hdr[index]);
    test_async_append_zmodem_escaped_byte(buffer, hdr[index], escape_ctrl);
  }
  crc = test_async_zmodem_crc16_update(crc, 0u);
  crc = test_async_zmodem_crc16_update(crc, 0u);
  test_async_append_zmodem_escaped_byte(buffer, static_cast<uint8_t>(crc >> 8), escape_ctrl);
  test_async_append_zmodem_escaped_byte(buffer, static_cast<uint8_t>(crc), escape_ctrl);
}

static void test_async_append_zmodem_data_subpacket(
    std::vector<uint8_t> *buffer,
    const uint8_t *payload,
    std::size_t payload_len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl) {
  if (use_crc32) {
    uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0u; index < payload_len; ++index) {
      crc = test_async_zmodem_crc32_update(crc, payload[index]);
      test_async_append_zmodem_escaped_byte(buffer, payload[index], escape_ctrl);
    }
    crc = test_async_zmodem_crc32_update(crc, frame_end);
    buffer->push_back(TEST_ASYNC_ZDLE);
    buffer->push_back(frame_end);
    crc = ~crc;
    for (std::size_t index = 0u; index < 4u; ++index) {
      test_async_append_zmodem_escaped_byte(
          buffer,
          static_cast<uint8_t>((crc >> (index * 8u)) & 0xffu),
          escape_ctrl);
    }
    return;
  }

  uint16_t crc = 0u;
  for (std::size_t index = 0u; index < payload_len; ++index) {
    crc = test_async_zmodem_crc16_update(crc, payload[index]);
    test_async_append_zmodem_escaped_byte(buffer, payload[index], escape_ctrl);
  }
  crc = test_async_zmodem_crc16_update(crc, frame_end);
  crc = test_async_zmodem_crc16_update(crc, 0u);
  crc = test_async_zmodem_crc16_update(crc, 0u);
  buffer->push_back(TEST_ASYNC_ZDLE);
  buffer->push_back(frame_end);
  test_async_append_zmodem_escaped_byte(buffer, static_cast<uint8_t>(crc >> 8), escape_ctrl);
  test_async_append_zmodem_escaped_byte(buffer, static_cast<uint8_t>(crc), escape_ctrl);
}

static void test_async_append_zmodem_standard_send_replies(
    std::vector<uint8_t> *buffer) {
  uint8_t hdr[4] = {};
  hdr[3] = static_cast<uint8_t>(TEST_ASYNC_CANFC32 | TEST_ASYNC_ESCCTL);
  test_async_append_zmodem_hex_header(buffer, TEST_ASYNC_ZRINIT, hdr);

  hdr[0] = 0u;
  hdr[1] = 0u;
  hdr[2] = 0u;
  hdr[3] = 0u;
  test_async_append_zmodem_hex_header(buffer, TEST_ASYNC_ZRPOS, hdr);
  test_async_append_zmodem_hex_header(buffer, TEST_ASYNC_ZRINIT, hdr);
  test_async_append_zmodem_hex_header(buffer, TEST_ASYNC_ZFIN, hdr);
}

static bool test_async_zmodem_contains_sequence(
    const std::vector<uint8_t> &buffer,
    const uint8_t *pattern,
    std::size_t pattern_len) {
  if (pattern_len == 0u) {
    return true;
  }
  if (pattern_len > buffer.size()) {
    return false;
  }
  for (std::size_t offset = 0u; offset <= (buffer.size() - pattern_len); ++offset) {
    bool matched = true;
    for (std::size_t index = 0u; index < pattern_len; ++index) {
      if (buffer[offset + index] != pattern[index]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      return true;
    }
  }
  return false;
}

static std::vector<uint8_t> test_async_make_zmodem_large_payload(std::size_t len) {
  std::vector<uint8_t> payload(len);
  for (std::size_t index = 0u; index < len; ++index) {
    if ((index % 4096u) == 37u && (index + 1u) < len) {
      payload[index] = '@';
      payload[index + 1u] = '\r';
      ++index;
      continue;
    }
    switch (index % 29u) {
      case 0u:
        payload[index] = TEST_ASYNC_ZDLE;
        break;
      case 1u:
        payload[index] = TEST_ASYNC_XON;
        break;
      case 2u:
        payload[index] = TEST_ASYNC_XOFF;
        break;
      case 3u:
        payload[index] = 0x7fu;
        break;
      case 4u:
        payload[index] = 0xffu;
        break;
      default:
        payload[index] = static_cast<uint8_t>((index * 31u) ^ (index >> 5u));
        break;
    }
  }
  return payload;
}

static std::vector<uint8_t> test_async_make_zmodem_receive_wire(
    const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> wire;
  uint8_t hdr[4] = {};
  const bool use_crc32 = true;
  const bool escape_ctrl = true;
  std::string metadata = "zmodem-receive.bin";
  metadata.push_back('\0');
  metadata += std::to_string(payload.size());
  metadata += " 0 0 0 1 1";
  metadata.push_back('\0');

  test_async_append_zmodem_binary_header(
      &wire,
      TEST_ASYNC_ZFILE,
      hdr,
      use_crc32,
      escape_ctrl);
  test_async_append_zmodem_data_subpacket(
      &wire,
      reinterpret_cast<const uint8_t *>(metadata.data()),
      metadata.size(),
      TEST_ASYNC_ZCRCE,
      use_crc32,
      escape_ctrl);

  test_async_zmodem_stohdr(0u, hdr);
  test_async_append_zmodem_binary_header(
      &wire,
      TEST_ASYNC_ZDATA,
      hdr,
      use_crc32,
      escape_ctrl);
  for (std::size_t offset = 0u; offset < payload.size();) {
    const std::size_t chunk_len = std::min<std::size_t>(4096u, payload.size() - offset);
    const uint8_t frame_end =
        offset + chunk_len == payload.size() ? TEST_ASYNC_ZCRCE : TEST_ASYNC_ZCRCG;
    test_async_append_zmodem_data_subpacket(
        &wire,
        payload.data() + offset,
        chunk_len,
        frame_end,
        use_crc32,
        escape_ctrl);
    offset += chunk_len;
  }

  test_async_zmodem_stohdr(static_cast<uint32_t>(payload.size()), hdr);
  test_async_append_zmodem_binary_header(
      &wire,
      TEST_ASYNC_ZEOF,
      hdr,
      use_crc32,
      escape_ctrl);
  memset(hdr, 0, sizeof(hdr));
  test_async_append_zmodem_hex_header(&wire, TEST_ASYNC_ZFIN, hdr);
  return wire;
}

static void test_async_zmodem_large_send_direct(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_source_t source{};
  xyzm_async_source_ops_t source_ops{};
  auto options = test_async_zmodem_options();
  xyzm_zmodem_send_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  xyzm_async_transfer_observer_t observer{};
  uint64_t payload_delta_sum = 0u;

  test_async_append_zmodem_standard_send_replies(&link.incoming);
  test_async_build_fake_link_ops(&link, &link_ops);

  source.info.name = "large-zmodem.bin";
  source.data = test_async_make_zmodem_large_payload((2u * 1024u * 1024u) + 37u);
  source.info.size_bytes = source.data.size();
  test_async_build_memory_source_ops(&source, &source_ops);

  observer.notify = [&payload_delta_sum](const xyzm_async_transfer_event_t &event) {
    if (event.protocol == XYZM_TRANSFER_PROTOCOL_ZMODEM &&
        event.direction == XYZM_TRANSFER_DIRECTION_SEND) {
      payload_delta_sum += event.payload_delta;
    }
  };

  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;
  request.observer = &observer;

  test_async_run([&]() {
    return xyzm_zmodem_send_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_TRUE(report.payload_bytes == source.data.size());
  ASYNC_ASSERT_TRUE(report.completed_files == 1u);
  ASYNC_ASSERT_TRUE(payload_delta_sum == source.data.size());
  ASYNC_ASSERT_TRUE(!source.end_error);
  ASYNC_ASSERT_TRUE(link.incoming_pos == link.incoming.size());
  ASYNC_ASSERT_TRUE(link.outgoing.size() > source.data.size());
  ASYNC_ASSERT_TRUE(link.outgoing.size() < (source.data.size() * 3u));
  ASYNC_ASSERT_TRUE(link.send_count < (source.data.size() / 4096u));
  ASYNC_ASSERT_TRUE(link.max_send_len > 4096u);

  const uint8_t escaped_zdle[] = {
      TEST_ASYNC_ZDLE,
      static_cast<uint8_t>(TEST_ASYNC_ZDLE ^ 0x40u)};
  const uint8_t escaped_xon[] = {
      TEST_ASYNC_ZDLE,
      static_cast<uint8_t>(TEST_ASYNC_XON ^ 0x40u)};
  const uint8_t escaped_xoff[] = {
      TEST_ASYNC_ZDLE,
      static_cast<uint8_t>(TEST_ASYNC_XOFF ^ 0x40u)};
  const uint8_t escaped_7f[] = {TEST_ASYNC_ZDLE, static_cast<uint8_t>('l')};
  const uint8_t escaped_ff[] = {TEST_ASYNC_ZDLE, static_cast<uint8_t>('m')};
  const uint8_t escaped_at_cr[] = {
      static_cast<uint8_t>('@'),
      TEST_ASYNC_ZDLE,
      static_cast<uint8_t>('\r' ^ 0x40u)};
  ASYNC_ASSERT_TRUE(test_async_zmodem_contains_sequence(
      link.outgoing,
      escaped_zdle,
      sizeof(escaped_zdle)));
  ASYNC_ASSERT_TRUE(test_async_zmodem_contains_sequence(
      link.outgoing,
      escaped_xon,
      sizeof(escaped_xon)));
  ASYNC_ASSERT_TRUE(test_async_zmodem_contains_sequence(
      link.outgoing,
      escaped_xoff,
      sizeof(escaped_xoff)));
  ASYNC_ASSERT_TRUE(test_async_zmodem_contains_sequence(
      link.outgoing,
      escaped_7f,
      sizeof(escaped_7f)));
  ASYNC_ASSERT_TRUE(test_async_zmodem_contains_sequence(
      link.outgoing,
      escaped_ff,
      sizeof(escaped_ff)));
  ASYNC_ASSERT_TRUE(test_async_zmodem_contains_sequence(
      link.outgoing,
      escaped_at_cr,
      sizeof(escaped_at_cr)));
}

static void test_async_zmodem_receive_batches_link_reads(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_sink_t sink{};
  xyzm_async_sink_ops_t sink_ops{};
  auto options = test_async_zmodem_options();
  xyzm_zmodem_receive_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  const std::vector<uint8_t> payload =
      test_async_make_zmodem_large_payload((256u * 1024u) + 37u);

  link.incoming = test_async_make_zmodem_receive_wire(payload);
  test_async_build_fake_link_ops(&link, &link_ops);
  test_async_build_memory_sink_ops(&sink, &sink_ops);

  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  test_async_run([&]() {
    return xyzm_zmodem_receive_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_TRUE(report.payload_bytes == payload.size());
  ASYNC_ASSERT_TRUE(report.completed_files == 1u);
  ASYNC_ASSERT_TRUE(sink.data == payload);
  ASYNC_ASSERT_TRUE(link.recv_count < 64u);
  ASYNC_ASSERT_TRUE(link.max_recv_len >= (16u * 1024u));
}

}  // namespace

extern const test_async_case_t xyzm_async_zmodem_integration_tests[] = {
  ASYNC_TEST_CASE(test_async_zmodem_large_send_direct, "Asynchronous ZMODEM send handles MiB payloads without per-byte coroutine dispatch."),
  ASYNC_TEST_CASE(test_async_zmodem_receive_batches_link_reads, "Asynchronous ZMODEM receive batches link reads instead of awaiting per byte."),
};

extern const std::size_t xyzm_async_zmodem_integration_tests_count =
    sizeof(xyzm_async_zmodem_integration_tests) /
    sizeof(xyzm_async_zmodem_integration_tests[0]);
