/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_async_support.h"
#include "../../src/xyzm_async_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <vector>

static std::size_t count_ymodem_terminal_header_frames(
    const std::vector<uint8_t> &data) {
  std::array<uint8_t, 3u + 128u + 2u> frame{};
  std::size_t count = 0;
  auto cursor = data.begin();

  frame[0] = XYZM_SOH;
  frame[1] = 0u;
  frame[2] = 0xffu;
  for (;;) {
    cursor = std::search(cursor, data.end(), frame.begin(), frame.end());
    if (cursor == data.end()) {
      return count;
    }
    ++count;
    ++cursor;
  }
}

static void test_async_xmodem_send_direct(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_source_t source{};
  xyzm_async_source_ops_t source_ops{};
  auto options = test_async_xmodem_send_options(
      XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
      XYZM_XMODEM_PACKET_SIZE_128);
  xyzm_xmodem_send_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  const std::vector<uint8_t> trailing = {'p', 'r', 'o', 'm', 'p', 't', '>', 0, 0xffu};
  xyzm_async_transfer_observer_t observer{};
  bool saw_finished = false;
  report.unread_bytes = {0x55u};
  observer.notify = [&](const xyzm_async_transfer_event_t &event) {
    ASYNC_ASSERT_TRUE(event.report.has_value());
    if (event.kind == XYZM_TRANSFER_EVENT_TRANSFER_STARTED)
      ASYNC_ASSERT_TRUE(event.report->get().unread_bytes.empty());
    if (event.kind == XYZM_TRANSFER_EVENT_TRANSFER_FINISHED) {
      ASYNC_ASSERT_TRUE(event.report->get().unread_bytes == trailing);
      saw_finished = true;
    }
  };

  link.timeout_recv_count = 1u;
  link.incoming = {XYZM_NAK, XYZM_ACK, XYZM_NAK, XYZM_ACK};
  link.incoming.insert(link.incoming.end(), trailing.begin(), trailing.end());
  test_async_build_fake_link_ops(&link, &link_ops);

  source.info.name = "direct.bin";
  source.info.size_bytes = 4u;
  source.data = {'d', 'a', 't', 'a'};
  test_async_build_memory_source_ops(&source, &source_ops);

  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;
  request.observer = &observer;

  test_async_run([&]() {
    return xyzm_xmodem_send_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(saw_finished);
  ASYNC_ASSERT_TRUE(report.unread_bytes == trailing);
  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_TRUE(report.payload_bytes == source.data.size());
  ASYNC_ASSERT_TRUE(link.outgoing.size() >= 133u);
  ASYNC_ASSERT_EQ_U8(link.outgoing[0], XYZM_SOH);
  ASYNC_ASSERT_EQ_U8(link.outgoing[3], 'd');
  ASYNC_ASSERT_EQ_U8(link.outgoing[4], 'a');
  ASYNC_ASSERT_EQ_U8(link.outgoing[5], 't');
  ASYNC_ASSERT_EQ_U8(link.outgoing[6], 'a');
}

static void test_async_xmodem_receive_direct(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_sink_t sink{};
  xyzm_async_sink_ops_t sink_ops{};
  auto options = test_async_xmodem_receive_options(XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM);
  xyzm_xmodem_receive_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  std::array<uint8_t, 128> payload{};

  std::fill(payload.begin(), payload.end(), 0x1au);
  std::memcpy(payload.data(), "async", 5u);
  link.timeout_recv_count = 1u;
  test_async_append_xmodem_block(
      &link.incoming,
      1u,
      std::span<const uint8_t>(payload.data(), payload.size()),
      false,
      XYZM_SOH);
  link.incoming.push_back(XYZM_EOT);
  link.incoming.push_back(XYZM_EOT);
  test_async_build_fake_link_ops(&link, &link_ops);
  test_async_build_memory_sink_ops(&sink, &sink_ops);

  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  test_async_run([&]() {
    return xyzm_xmodem_receive_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_EQ_SIZE(sink.data.size(), payload.size());
  ASYNC_ASSERT_TRUE(std::memcmp(sink.data.data(), payload.data(), payload.size()) == 0);
  ASYNC_ASSERT_TRUE(!link.outgoing.empty());
}

static void test_async_xmodem_receive_batches_link_reads(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_sink_t sink{};
  xyzm_async_sink_ops_t sink_ops{};
  auto options = test_async_xmodem_receive_options(XYZM_XMODEM_CHECKSUM_MODE_CRC);
  xyzm_xmodem_receive_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  const std::vector<uint8_t> trailing = {'p', 'r', 'o', 'm', 'p', 't', '>', 0, 0xffu};
  xyzm_async_transfer_observer_t observer{};
  bool saw_finished = false;
  report.unread_bytes = {0x55u};
  observer.notify = [&](const xyzm_async_transfer_event_t &event) {
    ASYNC_ASSERT_TRUE(event.report.has_value());
    if (event.kind == XYZM_TRANSFER_EVENT_TRANSFER_STARTED)
      ASYNC_ASSERT_TRUE(event.report->get().unread_bytes.empty());
    if (event.kind == XYZM_TRANSFER_EVENT_TRANSFER_FINISHED) {
      ASYNC_ASSERT_TRUE(event.report->get().unread_bytes == trailing);
      saw_finished = true;
    }
  };
  std::array<uint8_t, 1024> payload{};

  test_async_fill_pattern(payload.data(), payload.size(), 17u);
  test_async_append_xmodem_block(
      &link.incoming,
      1u,
      std::span<const uint8_t>(payload.data(), payload.size()),
      true,
      XYZM_STX);
  link.incoming.push_back(XYZM_EOT);
  link.incoming.push_back(XYZM_EOT);
  link.incoming.insert(link.incoming.end(), trailing.begin(), trailing.end());
  test_async_build_fake_link_ops(&link, &link_ops);
  test_async_build_memory_sink_ops(&sink, &sink_ops);

  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;
  request.observer = &observer;

  test_async_run([&]() {
    return xyzm_xmodem_receive_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(saw_finished);
  ASYNC_ASSERT_TRUE(report.unread_bytes == trailing);
  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_EQ_SIZE(sink.data.size(), payload.size());
  ASYNC_ASSERT_TRUE(std::memcmp(sink.data.data(), payload.data(), payload.size()) == 0);
  ASYNC_ASSERT_TRUE(link.recv_count <= 2u);
  ASYNC_ASSERT_TRUE(link.max_recv_len >= 1024u);
}

static void test_async_ymodem_receive_batches_link_reads(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_sink_t sink{};
  xyzm_async_sink_ops_t sink_ops{};
  auto options = test_async_ymodem_options(XYZM_YMODEM_VARIANT_STANDARD);
  xyzm_ymodem_receive_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  std::array<uint8_t, 128> metadata{};
  std::array<uint8_t, 1024> payload{};
  std::array<uint8_t, 128> terminal{};
  const char name[] = "payload.bin";
  const char size[] = "1024";

  std::memcpy(metadata.data(), name, sizeof(name));
  std::memcpy(metadata.data() + sizeof(name), size, sizeof(size));
  test_async_fill_pattern(payload.data(), payload.size(), 23u);
  test_async_append_xmodem_block(
      &link.incoming,
      0u,
      std::span<const uint8_t>(metadata.data(), metadata.size()),
      true,
      XYZM_SOH);
  test_async_append_xmodem_block(
      &link.incoming,
      1u,
      std::span<const uint8_t>(payload.data(), payload.size()),
      true,
      XYZM_STX);
  link.incoming.push_back(XYZM_EOT);
  link.incoming.push_back(XYZM_EOT);
  test_async_append_xmodem_block(
      &link.incoming,
      0u,
      std::span<const uint8_t>(terminal.data(), terminal.size()),
      true,
      XYZM_SOH);
  test_async_build_fake_link_ops(&link, &link_ops);
  test_async_build_memory_sink_ops(&sink, &sink_ops);

  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  test_async_run([&]() {
    return xyzm_ymodem_receive_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_EQ_SIZE(sink.data.size(), payload.size());
  ASYNC_ASSERT_TRUE(std::memcmp(sink.data.data(), payload.data(), payload.size()) == 0);
  ASYNC_ASSERT_TRUE(link.recv_count <= 2u);
  ASYNC_ASSERT_TRUE(link.max_recv_len >= 1024u);
}

static void test_async_ymodem_send_terminal_header_ack_timeout(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_source_t source{};
  xyzm_async_source_ops_t source_ops{};
  auto options = test_async_ymodem_options(XYZM_YMODEM_VARIANT_STANDARD);
  xyzm_ymodem_send_async_request_t request{};
  xyzm_async_transfer_report_t report{};

  link.incoming = {
      XYZM_CRC_REQ,
      XYZM_ACK,
      XYZM_CRC_REQ,
      XYZM_ACK,
      XYZM_NAK,
      XYZM_ACK,
      XYZM_CRC_REQ,
  };
  test_async_build_fake_link_ops(&link, &link_ops);

  source.info.name = "demo.bin";
  source.info.size_bytes = 3u;
  source.data = {'a', 'b', 'c'};
  test_async_build_memory_source_ops(&source, &source_ops);

  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  test_async_run([&]() {
    return xyzm_ymodem_send_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(report.final_result.has_value());
  ASYNC_ASSERT_TRUE(*report.final_result == xyzm_async_status_t::ok);
  ASYNC_ASSERT_TRUE(report.payload_bytes == source.data.size());
  ASYNC_ASSERT_TRUE(report.completed_files == 1u);
  ASYNC_ASSERT_TRUE(!source.end_error);
  ASYNC_ASSERT_EQ_SIZE(count_ymodem_terminal_header_frames(link.outgoing), 1u);
}

static void test_async_recv_byte_uses_cached_data_with_zero_timeout(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  xyzm_runtime_options_t options{};
  xyzm_async_runtime_t runtime{};
  uint8_t first = 0u;
  uint8_t second = 0u;

  link.incoming = {'a', 'b'};
  test_async_build_fake_link_ops(&link, &link_ops);
  options.handshake_timeout_ms = 1000u;
  options.block_timeout_ms = 1000u;
  runtime.link = &link_ops;
  runtime.options = &options;
  runtime.protocol = XYZM_TRANSFER_PROTOCOL_XMODEM;
  runtime.direction = XYZM_TRANSFER_DIRECTION_RECEIVE;

  test_async_run([&]() -> cardio::promise<void> {
    co_await xyzm_recv_byte_async(runtime, first, 1000u);
    co_await xyzm_recv_byte_async(runtime, second, 0u);
  });

  ASYNC_ASSERT_EQ_U8(first, 'a');
  ASYNC_ASSERT_EQ_U8(second, 'b');
  ASYNC_ASSERT_TRUE(link.recv_count == 1u);
}

static void test_async_argument_error_uses_exception(void) {
  xyzm_async_transfer_report_t report{};
  report.unread_bytes = {0x55u};
  bool caught = false;

  try {
    test_async_run([&]() {
      return xyzm_xmodem_send_async(nullptr, &report, {});
    });
  } catch (const xyzm_async_argument_error &) {
    caught = true;
  }

  ASYNC_ASSERT_TRUE(caught);
  ASYNC_ASSERT_TRUE(report.unread_bytes.empty());
  ASYNC_ASSERT_TRUE(!report.final_result.has_value());
}

static void test_async_timeout_error_uses_exception(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_source_t source{};
  xyzm_async_source_ops_t source_ops{};
  auto options = test_async_xmodem_send_options(
      XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
      XYZM_XMODEM_PACKET_SIZE_128);
  xyzm_xmodem_send_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  bool caught = false;

  test_async_build_fake_link_ops(&link, &link_ops);
  source.info.name = "timeout.bin";
  source.info.size_bytes = 1u;
  source.data = {'x'};
  test_async_build_memory_source_ops(&source, &source_ops);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  try {
    test_async_run([&]() {
      return xyzm_xmodem_send_async(&request, &report, {});
    });
  } catch (const xyzm_async_timeout_error &) {
    caught = true;
  }

  ASYNC_ASSERT_TRUE(caught);
  ASYNC_ASSERT_TRUE(!report.final_result.has_value());
}

static void test_async_timeout_source_survives_dispatcher_teardown(void) {
  auto timeout_source = std::optional<cardio::cancellation_source>{};

  {
    cardio::dispatcher_host dispatcher;
    timeout_source.emplace(cardio::cancellations::timeout(60000u));
  }

  ASYNC_ASSERT_TRUE(timeout_source->cancel());
  ASYNC_ASSERT_TRUE(
      timeout_source->get_cancellation().is_cancellation_requested());
  timeout_source.reset();
  ASYNC_ASSERT_TRUE(!timeout_source.has_value());
}

static void test_async_failure_preserves_unread_bytes(void) {
  for (const bool cancel_during_read : {false, true}) {
    test_async_fake_link_t link{};
    xyzm_async_link_ops_t link_ops{};
    test_async_memory_sink_t sink{};
    xyzm_async_sink_ops_t sink_ops{};
    auto options = test_async_xmodem_receive_options(XYZM_XMODEM_CHECKSUM_MODE_CRC);
    xyzm_xmodem_receive_async_request_t request{};
    xyzm_async_transfer_report_t report{};
    xyzm_async_transfer_observer_t observer{};
    cardio::cancellation_source cancellation;
    const std::vector<uint8_t> trailing = {'a', 'f', 't', 'e', 'r', 0, 0xffu};
    link.incoming = {XYZM_CAN, XYZM_CAN};
    link.incoming.insert(link.incoming.end(), trailing.begin(), trailing.end());
    const auto expected = cancel_during_read ? link.incoming : trailing;
    test_async_build_fake_link_ops(&link, &link_ops);
    test_async_build_memory_sink_ops(&sink, &sink_ops);
    if (cancel_during_read) {
      const auto recv = link_ops.recv;
      link_ops.recv = [&, recv](std::span<uint8_t> bytes, uint32_t timeout_ms,
          std::size_t &read_len, cardio::cancellation stop) -> cardio::promise<void> {
        co_await recv(bytes, timeout_ms, read_len, stop);
        cancellation.cancel();
      };
    }
    bool saw_finished = false;
    observer.notify = [&](const xyzm_async_transfer_event_t &event) {
      if (event.kind == XYZM_TRANSFER_EVENT_TRANSFER_FINISHED) {
        ASYNC_ASSERT_TRUE(event.report.has_value());
        ASYNC_ASSERT_TRUE(event.report->get().unread_bytes == expected);
        ASYNC_ASSERT_TRUE(event.error != nullptr);
        saw_finished = true;
      }
    };
    request.link = &link_ops;
    request.sink = &sink_ops;
    request.options = &options;
    request.observer = &observer;
    bool caught = false;
    try {
      test_async_run([&]() {
        return xyzm_xmodem_receive_async(&request, &report, cancellation.get_cancellation());
      });
    } catch (const xyzm_async_cancelled_error &) {
      ASYNC_ASSERT_TRUE(cancel_during_read);
      caught = true;
    } catch (const xyzm_async_remote_cancelled_error &) {
      ASYNC_ASSERT_TRUE(!cancel_during_read);
      caught = true;
    }
    ASYNC_ASSERT_TRUE(caught && saw_finished);
    ASYNC_ASSERT_TRUE(!report.final_result.has_value());
    ASYNC_ASSERT_TRUE(report.unread_bytes == expected);
  }
}

extern const test_async_case_t xyzm_async_core_tests[] = {
  ASYNC_TEST_CASE(test_async_failure_preserves_unread_bytes, "Failure and cancellation return cached bytes before the terminal event."),
  ASYNC_TEST_CASE(test_async_xmodem_send_direct, "Asynchronous XMODEM send calls the async public API directly."),
  ASYNC_TEST_CASE(test_async_xmodem_receive_direct, "Asynchronous XMODEM receive calls the async public API directly."),
  ASYNC_TEST_CASE(test_async_xmodem_receive_batches_link_reads, "Asynchronous XMODEM receive batches link reads within a block."),
  ASYNC_TEST_CASE(test_async_ymodem_receive_batches_link_reads, "Asynchronous YMODEM receive batches link reads within a block."),
  ASYNC_TEST_CASE(test_async_ymodem_send_terminal_header_ack_timeout, "Asynchronous YMODEM send does not retry the terminal header after final ACK loss."),
  ASYNC_TEST_CASE(test_async_recv_byte_uses_cached_data_with_zero_timeout, "Asynchronous byte receive consumes cached data before a zero-timeout poll fails."),
  ASYNC_TEST_CASE(test_async_argument_error_uses_exception, "Asynchronous argument failures are reported as typed exceptions."),
  ASYNC_TEST_CASE(test_async_timeout_error_uses_exception, "Asynchronous timeout failures are reported as typed exceptions."),
  ASYNC_TEST_CASE(test_async_timeout_source_survives_dispatcher_teardown, "Asynchronous timeout cancellation remains safe after dispatcher teardown."),
};

extern const std::size_t xyzm_async_core_tests_count =
    sizeof(xyzm_async_core_tests) / sizeof(xyzm_async_core_tests[0]);
