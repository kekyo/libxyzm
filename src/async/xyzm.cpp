/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_async_internal.h"

#include <algorithm>
#include <cstring>

/**
 * @brief Converts an absolute deadline into the remaining timeout expected by link ops.
 *
 * @param runtime Active transfer state that supplies the monotonic clock.
 * @param deadline_ms Absolute timeout deadline in milliseconds.
 * @return Remaining timeout in milliseconds.
 * @remarks Returns zero when the deadline already passed.
 */
static inline uint32_t xyzm_remaining_timeout(
    xyzm_async_runtime_t &runtime,
    uint64_t deadline_ms) {
  uint64_t now_ms;

  now_ms = runtime.link->now_ms();
  if (now_ms >= deadline_ms) {
    return 0u;
  }
  if ((deadline_ms - now_ms) > 0xffffffffu) {
    return 0xffffffffu;
  }
  return (uint32_t)(deadline_ms - now_ms);
}

std::exception_ptr xyzm_make_timeout_error_async() {
  return std::make_exception_ptr(xyzm_async_timeout_error("xyzm async timeout"));
}

std::exception_ptr xyzm_make_cancelled_error_async() {
  return std::make_exception_ptr(xyzm_async_cancelled_error("xyzm async cancelled"));
}

std::exception_ptr xyzm_make_remote_cancelled_error_async() {
  return std::make_exception_ptr(xyzm_async_remote_cancelled_error("xyzm async remote cancelled"));
}

std::exception_ptr xyzm_make_io_error_async() {
  return std::make_exception_ptr(xyzm_async_io_error("xyzm async io error"));
}

std::exception_ptr xyzm_make_protocol_error_async() {
  return std::make_exception_ptr(xyzm_async_protocol_error("xyzm async protocol error"));
}

std::exception_ptr xyzm_make_argument_error_async() {
  return std::make_exception_ptr(xyzm_async_argument_error("xyzm async argument error"));
}

std::exception_ptr xyzm_make_unsupported_error_async() {
  return std::make_exception_ptr(xyzm_async_unsupported_error("xyzm async unsupported"));
}

[[noreturn]] void xyzm_throw_timeout_async() {
  std::rethrow_exception(xyzm_make_timeout_error_async());
}

[[noreturn]] void xyzm_throw_cancelled_async() {
  std::rethrow_exception(xyzm_make_cancelled_error_async());
}

[[noreturn]] void xyzm_throw_remote_cancelled_async() {
  std::rethrow_exception(xyzm_make_remote_cancelled_error_async());
}

[[noreturn]] void xyzm_throw_io_async() {
  std::rethrow_exception(xyzm_make_io_error_async());
}

[[noreturn]] void xyzm_throw_protocol_async() {
  std::rethrow_exception(xyzm_make_protocol_error_async());
}

[[noreturn]] void xyzm_throw_argument_async() {
  std::rethrow_exception(xyzm_make_argument_error_async());
}

[[noreturn]] void xyzm_throw_unsupported_async() {
  std::rethrow_exception(xyzm_make_unsupported_error_async());
}

void xyzm_check_cancelled_async(xyzm_async_runtime_t &runtime) {
  if (runtime.cancellation.is_cancellation_requested()) {
    xyzm_throw_cancelled_async();
  }
}

std::exception_ptr xyzm_normalize_error_async(std::exception_ptr error) {
  if (!error) {
    return nullptr;
  }
  try {
    std::rethrow_exception(error);
  } catch (const cardio::canceled_exception &) {
    return xyzm_make_cancelled_error_async();
  } catch (...) {
    return error;
  }
}

void xyzm_report_init_async(xyzm_async_transfer_report_t *report) {
  if (report == NULL) {
    return;
  }
  report->final_result = std::nullopt;
  report->payload_bytes = 0u;
  report->completed_files = 0u;
  report->retry_count = 0u;
}

void xyzm_report_retry_async(xyzm_async_runtime_t &runtime) {
  if (runtime.report != NULL) {
    runtime.report->retry_count += 1u;
  }
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_RETRY,
      std::nullopt,
      nullptr,
      std::nullopt,
      0u);
}

void xyzm_report_payload_async(
    xyzm_async_runtime_t &runtime,
    uint64_t payload_delta,
    const xyzm_async_file_info_t &file_info) {
  if (runtime.report != NULL) {
    runtime.report->payload_bytes += payload_delta;
  }
  if (payload_delta == 0u) {
    return;
  }
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_PROGRESS,
      std::nullopt,
      nullptr,
      std::cref(file_info),
      payload_delta);
}

void xyzm_report_completed_files_async(
    xyzm_async_runtime_t &runtime,
    uint32_t completed_files) {
  if (runtime.report == NULL) {
    return;
  }
  runtime.report->completed_files = completed_files;
}

void xyzm_report_completed_file_async(xyzm_async_runtime_t &runtime) {
  if (runtime.report == NULL) {
    return;
  }
  runtime.report->completed_files += 1u;
}

void xyzm_notify_transfer_event_async(
    xyzm_async_runtime_t &runtime,
    xyzm_transfer_event_kind_t kind,
    std::optional<xyzm_async_status_t> result,
    std::exception_ptr error,
    std::optional<std::reference_wrapper<const xyzm_async_file_info_t>> file_info,
    uint64_t payload_delta) {
  xyzm_async_transfer_event_t event;

  if ((runtime.observer == NULL) || !runtime.observer->notify) {
    return;
  }
  event.kind = kind;
  event.protocol = runtime.protocol;
  event.direction = runtime.direction;
  event.result = result;
  event.error = error;
  event.file_info = file_info;
  event.payload_delta = payload_delta;
  if (runtime.report != NULL) {
    event.report = std::cref(*runtime.report);
  } else {
    event.report = std::nullopt;
  }
  runtime.observer->notify(event);
}

void xyzm_notify_transfer_started_async(xyzm_async_runtime_t &runtime) {
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_TRANSFER_STARTED,
      std::nullopt,
      nullptr,
      std::nullopt,
      0u);
}

void xyzm_notify_file_started_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_file_info_t &file_info) {
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_FILE_STARTED,
      std::nullopt,
      nullptr,
      std::cref(file_info),
      0u);
}

void xyzm_notify_file_finished_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_file_info_t &file_info,
    std::exception_ptr error) {
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_FILE_FINISHED,
      error ? std::nullopt : std::optional<xyzm_async_status_t>(xyzm_async_status_t::ok),
      error,
      std::cref(file_info),
      0u);
}

void xyzm_finish_transfer_success_async(xyzm_async_runtime_t &runtime) {
  if (runtime.report != NULL) {
    runtime.report->final_result = xyzm_async_status_t::ok;
  }
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_TRANSFER_FINISHED,
      xyzm_async_status_t::ok,
      nullptr,
      std::nullopt,
      0u);
}

void xyzm_finish_transfer_failure_async(
    xyzm_async_runtime_t &runtime,
    std::exception_ptr error) {
  if (runtime.report != NULL) {
    runtime.report->final_result = std::nullopt;
  }
  xyzm_notify_transfer_event_async(
      runtime,
      XYZM_TRANSFER_EVENT_TRANSFER_FINISHED,
      std::nullopt,
      error,
      std::nullopt,
      0u);
}

cardio::promise<void> xyzm_link_send_async(
    xyzm_async_runtime_t &runtime,
    std::span<const uint8_t> buf,
    uint32_t timeout_ms,
    size_t &written_len) {
  xyzm_check_cancelled_async(runtime);
  try {
    co_await runtime.link->send(
        buf,
        timeout_ms,
        written_len,
        runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  co_return;
}

cardio::promise<void> xyzm_send_exact_async(
    xyzm_async_runtime_t &runtime,
    std::span<const uint8_t> buf,
    uint32_t timeout_ms) {
  uint64_t deadline_ms;
  size_t sent_len;

  deadline_ms =
      runtime.link->now_ms() + (uint64_t)timeout_ms;
  sent_len = 0u;
  while (sent_len < buf.size()) {
    size_t written_len;
    uint32_t remaining_ms;

    remaining_ms = xyzm_remaining_timeout(runtime, deadline_ms);
    if (remaining_ms == 0u) {
      xyzm_throw_timeout_async();
    }
    written_len = 0u;
    co_await xyzm_link_send_async(
        runtime,
        buf.subspan(sent_len),
        remaining_ms,
        written_len);
    if (written_len == 0u) {
      xyzm_throw_io_async();
    }
    sent_len += written_len;
  }
  co_return;
}

cardio::promise<void> xyzm_link_recv_async(
    xyzm_async_runtime_t &runtime,
    std::span<uint8_t> buf,
    uint32_t timeout_ms,
    size_t &read_len) {
  xyzm_check_cancelled_async(runtime);
  try {
    co_await runtime.link->recv(
        buf,
        timeout_ms,
        read_len,
        runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  co_return;
}

size_t xyzm_consume_recv_cache_async(
    xyzm_async_runtime_t &runtime,
    std::span<uint8_t> buf) {
  const size_t available_len =
      runtime.recv_cache_size - runtime.recv_cache_offset;
  const size_t copy_len = std::min(buf.size(), available_len);
  if (copy_len == 0u) {
    return 0u;
  }
  memcpy(
      buf.data(),
      runtime.recv_cache.data() + runtime.recv_cache_offset,
      copy_len);
  runtime.recv_cache_offset += copy_len;
  if (runtime.recv_cache_offset == runtime.recv_cache_size) {
    runtime.recv_cache_offset = 0u;
    runtime.recv_cache_size = 0u;
  }
  return copy_len;
}

cardio::promise<void> xyzm_refill_recv_cache_async(
    xyzm_async_runtime_t &runtime,
    uint32_t timeout_ms) {
  if (timeout_ms == 0u) {
    xyzm_throw_timeout_async();
  }

  runtime.recv_cache_offset = 0u;
  runtime.recv_cache_size = 0u;
  co_await xyzm_link_recv_async(
      runtime,
      std::span<uint8_t>(runtime.recv_cache.data(), runtime.recv_cache.size()),
      timeout_ms,
      runtime.recv_cache_size);
  if (runtime.recv_cache_size == 0u) {
    xyzm_throw_io_async();
  }
  co_return;
}

cardio::promise<void> xyzm_recv_exact_async(
    xyzm_async_runtime_t &runtime,
    std::span<uint8_t> buf,
    uint32_t timeout_ms) {
  uint64_t deadline_ms;
  size_t read_total;

  deadline_ms =
      runtime.link->now_ms() + (uint64_t)timeout_ms;
  read_total = 0u;
  while (read_total < buf.size()) {
    size_t read_len;

    read_len = xyzm_consume_recv_cache_async(runtime, buf.subspan(read_total));
    if (read_len == 0u) {
      uint32_t remaining_ms;

      remaining_ms = xyzm_remaining_timeout(runtime, deadline_ms);
      if (remaining_ms == 0u) {
        xyzm_throw_timeout_async();
      }
      co_await xyzm_refill_recv_cache_async(runtime, remaining_ms);
      read_len = xyzm_consume_recv_cache_async(runtime, buf.subspan(read_total));
      if (read_len == 0u) {
        xyzm_throw_io_async();
      }
    }
    read_total += read_len;
  }
  co_return;
}

cardio::promise<void> xyzm_send_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t value,
    uint32_t timeout_ms) {
  co_await xyzm_send_exact_async(
      runtime,
      std::span<const uint8_t>(&value, 1u),
      timeout_ms);
  co_return;
}

cardio::promise<void> xyzm_recv_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &value,
    uint32_t timeout_ms) {
  co_await xyzm_recv_exact_async(
      runtime,
      std::span<uint8_t>(&value, 1u),
      timeout_ms);
  co_return;
}

cardio::promise<void> xyzm_send_cancel_async(xyzm_async_runtime_t &runtime) {
  uint8_t bytes[2];

  bytes[0] = XYZM_CAN;
  bytes[1] = XYZM_CAN;
  co_await xyzm_send_exact_async(
      runtime,
      std::span<const uint8_t>(bytes, sizeof(bytes)),
      runtime.options->block_timeout_ms);
  co_return;
}

cardio::promise<void> xyzm_send_cancel_and_rethrow_async(
    xyzm_async_runtime_t &runtime,
    std::exception_ptr error) {
  error = xyzm_normalize_error_async(error);
  try {
    co_await xyzm_send_cancel_async(runtime);
  } catch (...) {
  }
  std::rethrow_exception(error);
}

cardio::promise<xyzm_async_status_t> xyzm_send_single_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file) {
  xyzm_async_status_t status;

  *file = {};
  xyzm_check_cancelled_async(runtime);
  try {
    status = co_await source->next(file->info, runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  if (status != xyzm_async_status_t::ok) {
    co_return status;
  }
  file->active = true;
  xyzm_notify_file_started_async(runtime, file->info);
  co_return xyzm_async_status_t::ok;
}

cardio::promise<xyzm_async_status_t> xyzm_source_read_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    std::span<uint8_t> buf,
    size_t &read_len) {
  xyzm_async_status_t status;

  xyzm_check_cancelled_async(runtime);
  try {
    status = co_await source->read(buf, read_len, runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  co_return status;
}

cardio::promise<void> xyzm_source_seek_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    uint64_t offset) {
  xyzm_check_cancelled_async(runtime);
  try {
    co_await source->seek(offset, runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  co_return;
}

static cardio::promise<void> xyzm_source_end_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    const xyzm_async_file_info_t &info,
    std::exception_ptr error) {
  try {
    co_await source->end(
        info,
        xyzm_normalize_error_async(error),
        runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  co_return;
}

cardio::promise<void> xyzm_sink_begin_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    const xyzm_async_file_info_t &info,
    uint64_t &resume_offset) {
  xyzm_check_cancelled_async(runtime);
  try {
    co_await sink->begin(info, resume_offset, runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  co_return;
}

cardio::promise<void> xyzm_sink_write_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    std::span<const uint8_t> buf) {
  xyzm_check_cancelled_async(runtime);
  try {
    co_await sink->write(buf, runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  xyzm_check_cancelled_async(runtime);
  co_return;
}

static cardio::promise<void> xyzm_sink_end_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    const xyzm_async_file_info_t &info,
    std::exception_ptr error) {
  try {
    co_await sink->end(
        info,
        xyzm_normalize_error_async(error),
        runtime.cancellation);
  } catch (const cardio::canceled_exception &) {
    xyzm_throw_cancelled_async();
  }
  co_return;
}

cardio::promise<void> xyzm_finish_send_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    std::exception_ptr error) {
  bool had_file;
  std::exception_ptr notify_error;

  had_file = (file != NULL) && file->active;
  if ((source == NULL) || !source->end || (file == NULL) ||
      !file->active) {
    if (had_file) {
      xyzm_notify_file_finished_async(
          runtime,
          file->info,
          xyzm_normalize_error_async(error));
    }
    co_return;
  }
  notify_error = xyzm_normalize_error_async(error);
  try {
    co_await xyzm_source_end_async(runtime, source, file->info, notify_error);
  } catch (...) {
    if (!notify_error) {
      notify_error = xyzm_normalize_error_async(std::current_exception());
    }
  }
  file->active = false;
  if (had_file) {
    xyzm_notify_file_finished_async(runtime, file->info, notify_error);
  }
  if (!error && notify_error) {
    std::rethrow_exception(notify_error);
  }
  co_return;
}

cardio::promise<void> xyzm_finish_receive_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t *file,
    std::exception_ptr error) {
  bool had_file;
  std::exception_ptr notify_error;

  had_file = (file != NULL) && file->active;
  if ((sink == NULL) || !sink->end || (file == NULL) ||
      !file->active) {
    if (had_file) {
      xyzm_notify_file_finished_async(
          runtime,
          file->info,
          xyzm_normalize_error_async(error));
    }
    co_return;
  }
  notify_error = xyzm_normalize_error_async(error);
  try {
    co_await xyzm_sink_end_async(runtime, sink, file->info, notify_error);
  } catch (...) {
    if (!notify_error) {
      notify_error = xyzm_normalize_error_async(std::current_exception());
    }
  }
  file->active = false;
  if (had_file) {
    xyzm_notify_file_finished_async(runtime, file->info, notify_error);
  }
  if (!error && notify_error) {
    std::rethrow_exception(notify_error);
  }
  co_return;
}

cardio::promise<void> xyzm_finish_send_file_and_rethrow_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    std::exception_ptr error) {
  error = xyzm_normalize_error_async(error);
  co_await xyzm_finish_send_file_async(runtime, source, file, error);
  std::rethrow_exception(error);
}

cardio::promise<void> xyzm_finish_receive_file_and_rethrow_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t *file,
    std::exception_ptr error) {
  error = xyzm_normalize_error_async(error);
  co_await xyzm_finish_receive_file_async(runtime, sink, file, error);
  std::rethrow_exception(error);
}

bool xyzm_is_valid_async_link(const xyzm_async_link_ops_t *link) {
  return (link != NULL) && link->send && link->recv && link->now_ms;
}

void xyzm_validate_async_send_request(
    const xyzm_async_link_ops_t *link,
    const xyzm_async_source_ops_t *source) {
  if (!xyzm_is_valid_async_link(link) || (source == NULL) || !source->next ||
      !source->read) {
    xyzm_throw_argument_async();
  }
}

void xyzm_validate_async_receive_request(
    const xyzm_async_link_ops_t *link,
    const xyzm_async_sink_ops_t *sink) {
  if (!xyzm_is_valid_async_link(link) || (sink == NULL) || !sink->begin ||
      !sink->write) {
    xyzm_throw_argument_async();
  }
}
