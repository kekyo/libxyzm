/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "libxyzm/async.h"

#include <functional>
#include <type_traits>

int main() {
  static_assert(std::is_same_v<
      xyzm_async_send_cb,
      std::function<cardio::promise<void>(
          std::span<const uint8_t>,
          uint32_t,
          size_t &,
          cardio::cancellation)>>);
  static_assert(std::is_same_v<
      xyzm_async_source_read_cb,
      std::function<cardio::promise<xyzm_async_status_t>(
          std::span<uint8_t>,
          size_t &,
          cardio::cancellation)>>);

  int captured_state = 0;
  xyzm_async_link_ops_t link{};
  link.send =
      [&captured_state](
          std::span<const uint8_t> buf,
          uint32_t,
          size_t &written_len,
          cardio::cancellation cancellation) -> cardio::promise<void> {
        cancellation.throw_if_cancellation_requested();
        captured_state += 1;
        written_len = buf.size();
        co_return;
      };
  link.recv =
      [&captured_state](
          std::span<uint8_t>,
          uint32_t,
          size_t &read_len,
          cardio::cancellation cancellation) -> cardio::promise<void> {
        cancellation.throw_if_cancellation_requested();
        captured_state += 1;
        read_len = 0u;
        throw xyzm_async_timeout_error("smoke timeout");
      };
  link.now_ms = [&captured_state]() -> uint64_t {
    return static_cast<uint64_t>(captured_state);
  };

  xyzm_async_source_ops_t source{};
  source.next =
      [](
          xyzm_async_file_info_t &info,
          cardio::cancellation cancellation) -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    info.name = std::string("smoke.bin");
    info.size_bytes = 0u;
    co_return xyzm_async_status_t::end;
  };
  source.read =
      [](
          std::span<uint8_t>,
          size_t &,
          cardio::cancellation cancellation) -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    co_return xyzm_async_status_t::end;
  };
  source.end =
      [](
          const xyzm_async_file_info_t &,
          std::exception_ptr,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    co_return;
  };

  xyzm_async_sink_ops_t sink{};
  sink.begin =
      [](
          const xyzm_async_file_info_t &,
          uint64_t &resume_offset,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    resume_offset = 0u;
    co_return;
  };
  sink.write =
      [](
          std::span<const uint8_t>,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    co_return;
  };
  sink.end =
      [](
          const xyzm_async_file_info_t &,
          std::exception_ptr,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    co_return;
  };

  xyzm_async_transfer_observer_t observer{};
  observer.notify = [&captured_state](const xyzm_async_transfer_event_t &event) {
    if (event.file_info.has_value() && event.file_info->get().name.has_value()) {
      captured_state += 1;
    }
    if (event.report.has_value() && event.report->get().final_result.has_value()) {
      captured_state += 1;
    }
    if (event.error) {
      captured_state += 1;
    }
  };

  xyzm_async_transfer_report_t report{};
  xyzm_xmodem_send_async_request_t request{};

  request.link = &link;
  request.source = &source;
  request.observer = &observer;
  report.final_result = xyzm_async_status_t::ok;
  return (request.link == &link) && (request.source == &source) && link.send &&
          link.recv && link.now_ms && source.read && sink.write && observer.notify &&
          report.final_result.has_value() ?
      0 :
      1;
}
