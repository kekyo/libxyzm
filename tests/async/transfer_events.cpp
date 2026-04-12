/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_async_support.h"

#include <vector>

struct observed_async_event_t {
  xyzm_transfer_event_kind_t kind;
  xyzm_transfer_protocol_t protocol;
  xyzm_transfer_direction_t direction;
  bool normal_result;
  uint64_t payload_delta;
  uint64_t report_payload_bytes;
};

static void test_async_xmodem_send_events_direct(void) {
  test_async_fake_link_t link{};
  xyzm_async_link_ops_t link_ops{};
  test_async_memory_source_t source{};
  xyzm_async_source_ops_t source_ops{};
  auto options = test_async_xmodem_send_options(
      XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
      XYZM_XMODEM_PACKET_SIZE_128);
  xyzm_xmodem_send_async_request_t request{};
  xyzm_async_transfer_report_t report{};
  xyzm_async_transfer_observer_t observer{};
  std::vector<observed_async_event_t> events;

  link.timeout_recv_count = 1u;
  link.incoming = {XYZM_NAK, XYZM_ACK, XYZM_NAK, XYZM_ACK};
  test_async_build_fake_link_ops(&link, &link_ops);

  source.info.name = "events.bin";
  source.info.size_bytes = 6u;
  source.data = {'e', 'v', 'e', 'n', 't', 's'};
  test_async_build_memory_source_ops(&source, &source_ops);

  observer.notify = [&events](const xyzm_async_transfer_event_t &event) {
    observed_async_event_t record{};
    record.kind = event.kind;
    record.protocol = event.protocol;
    record.direction = event.direction;
    record.normal_result =
        event.result.has_value() && *event.result == xyzm_async_status_t::ok;
    record.payload_delta = event.payload_delta;
    if (event.report.has_value()) {
      record.report_payload_bytes = event.report->get().payload_bytes;
    }
    events.push_back(record);
  };

  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;
  request.observer = &observer;

  test_async_run([&]() {
    return xyzm_xmodem_send_async(&request, &report, {});
  });

  ASYNC_ASSERT_TRUE(events.size() >= 6u);
  ASYNC_ASSERT_TRUE(events.front().kind == XYZM_TRANSFER_EVENT_TRANSFER_STARTED);
  ASYNC_ASSERT_TRUE(events.back().kind == XYZM_TRANSFER_EVENT_TRANSFER_FINISHED);
  ASYNC_ASSERT_TRUE(events.back().normal_result);
  uint64_t payload_delta_sum = 0u;
  bool saw_file_started = false;
  bool saw_file_finished = false;
  bool saw_retry = false;
  for (const auto &event : events) {
    ASYNC_ASSERT_TRUE(event.protocol == XYZM_TRANSFER_PROTOCOL_XMODEM);
    ASYNC_ASSERT_TRUE(event.direction == XYZM_TRANSFER_DIRECTION_SEND);
    payload_delta_sum += event.payload_delta;
    saw_file_started = saw_file_started || event.kind == XYZM_TRANSFER_EVENT_FILE_STARTED;
    saw_file_finished = saw_file_finished || event.kind == XYZM_TRANSFER_EVENT_FILE_FINISHED;
    saw_retry = saw_retry || event.kind == XYZM_TRANSFER_EVENT_RETRY;
  }
  ASYNC_ASSERT_TRUE(saw_file_started);
  ASYNC_ASSERT_TRUE(saw_file_finished);
  ASYNC_ASSERT_TRUE(saw_retry);
  ASYNC_ASSERT_TRUE(payload_delta_sum == source.data.size());
  ASYNC_ASSERT_TRUE(report.final_result.has_value());
}

extern const test_async_case_t xyzm_async_transfer_event_tests[] = {
  ASYNC_TEST_CASE(test_async_xmodem_send_events_direct, "Asynchronous transfer events are observed without sync event conversion."),
};

extern const std::size_t xyzm_async_transfer_event_tests_count =
    sizeof(xyzm_async_transfer_event_tests) / sizeof(xyzm_async_transfer_event_tests[0]);
