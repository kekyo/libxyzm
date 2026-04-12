/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef XYZM_ASYNC_INTERNAL_H
#define XYZM_ASYNC_INTERNAL_H

#include "libxyzm/async.h"
#include "xyzm_common_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>

constexpr std::size_t XYZM_ASYNC_RECV_CACHE_SIZE = 32u * 1024u;

struct xyzm_async_runtime_t {
  const xyzm_async_link_ops_t *link;
  const xyzm_runtime_options_t *options;
  xyzm_async_transfer_report_t *report;
  const xyzm_async_transfer_observer_t *observer;
  cardio::cancellation cancellation;
  xyzm_transfer_protocol_t protocol;
  xyzm_transfer_direction_t direction;
  std::array<uint8_t, XYZM_ASYNC_RECV_CACHE_SIZE> recv_cache {};
  std::size_t recv_cache_offset {0u};
  std::size_t recv_cache_size {0u};
};

struct xyzm_async_send_file_t {
  xyzm_async_file_info_t info;
  bool active;
};

struct xyzm_async_receive_file_t {
  xyzm_async_file_info_t info;
  bool active;
};

void xyzm_report_init_async(xyzm_async_transfer_report_t *report);
void xyzm_report_retry_async(xyzm_async_runtime_t &runtime);
void xyzm_report_payload_async(
    xyzm_async_runtime_t &runtime,
    uint64_t payload_delta,
    const xyzm_async_file_info_t &file_info);
void xyzm_report_completed_files_async(
    xyzm_async_runtime_t &runtime,
    uint32_t completed_files);
void xyzm_report_completed_file_async(xyzm_async_runtime_t &runtime);
void xyzm_notify_transfer_event_async(
    xyzm_async_runtime_t &runtime,
    xyzm_transfer_event_kind_t kind,
    std::optional<xyzm_async_status_t> result,
    std::exception_ptr error,
    std::optional<std::reference_wrapper<const xyzm_async_file_info_t>> file_info,
    uint64_t payload_delta);
void xyzm_notify_transfer_started_async(xyzm_async_runtime_t &runtime);
void xyzm_notify_file_started_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_file_info_t &file_info);
void xyzm_notify_file_finished_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_file_info_t &file_info,
    std::exception_ptr error);
void xyzm_finish_transfer_success_async(xyzm_async_runtime_t &runtime);
void xyzm_finish_transfer_failure_async(
    xyzm_async_runtime_t &runtime,
    std::exception_ptr error);
bool xyzm_is_valid_async_link(const xyzm_async_link_ops_t *link);
void xyzm_validate_async_send_request(
    const xyzm_async_link_ops_t *link,
    const xyzm_async_source_ops_t *source);
void xyzm_validate_async_receive_request(
    const xyzm_async_link_ops_t *link,
    const xyzm_async_sink_ops_t *sink);
void xyzm_check_cancelled_async(xyzm_async_runtime_t &runtime);
std::exception_ptr xyzm_normalize_error_async(std::exception_ptr error);

[[noreturn]] void xyzm_throw_timeout_async();
[[noreturn]] void xyzm_throw_cancelled_async();
[[noreturn]] void xyzm_throw_remote_cancelled_async();
[[noreturn]] void xyzm_throw_io_async();
[[noreturn]] void xyzm_throw_protocol_async();
[[noreturn]] void xyzm_throw_argument_async();
[[noreturn]] void xyzm_throw_unsupported_async();
std::exception_ptr xyzm_make_timeout_error_async();
std::exception_ptr xyzm_make_cancelled_error_async();
std::exception_ptr xyzm_make_remote_cancelled_error_async();
std::exception_ptr xyzm_make_io_error_async();
std::exception_ptr xyzm_make_protocol_error_async();
std::exception_ptr xyzm_make_argument_error_async();
std::exception_ptr xyzm_make_unsupported_error_async();

cardio::promise<void> xyzm_send_exact_async(
    xyzm_async_runtime_t &runtime,
    std::span<const uint8_t> buf,
    uint32_t timeout_ms);

cardio::promise<void> xyzm_link_send_async(
    xyzm_async_runtime_t &runtime,
    std::span<const uint8_t> buf,
    uint32_t timeout_ms,
    size_t &written_len);

cardio::promise<void> xyzm_recv_exact_async(
    xyzm_async_runtime_t &runtime,
    std::span<uint8_t> buf,
    uint32_t timeout_ms);

cardio::promise<void> xyzm_link_recv_async(
    xyzm_async_runtime_t &runtime,
    std::span<uint8_t> buf,
    uint32_t timeout_ms,
    size_t &read_len);

size_t xyzm_consume_recv_cache_async(
    xyzm_async_runtime_t &runtime,
    std::span<uint8_t> buf);
cardio::promise<void> xyzm_refill_recv_cache_async(
    xyzm_async_runtime_t &runtime,
    uint32_t timeout_ms);

cardio::promise<void> xyzm_send_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t value,
    uint32_t timeout_ms);

cardio::promise<void> xyzm_recv_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &value,
    uint32_t timeout_ms);

cardio::promise<void> xyzm_send_cancel_async(
    xyzm_async_runtime_t &runtime);
cardio::promise<void> xyzm_send_cancel_and_rethrow_async(
    xyzm_async_runtime_t &runtime,
    std::exception_ptr error);

cardio::promise<xyzm_async_status_t> xyzm_send_single_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file);

cardio::promise<xyzm_async_status_t> xyzm_source_read_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    std::span<uint8_t> buf,
    size_t &read_len);
cardio::promise<void> xyzm_source_seek_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    uint64_t offset);
cardio::promise<void> xyzm_sink_begin_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    const xyzm_async_file_info_t &info,
    uint64_t &resume_offset);
cardio::promise<void> xyzm_sink_write_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    std::span<const uint8_t> buf);

cardio::promise<void> xyzm_finish_send_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    std::exception_ptr error);

cardio::promise<void> xyzm_finish_receive_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t *file,
    std::exception_ptr error);
cardio::promise<void> xyzm_finish_send_file_and_rethrow_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    std::exception_ptr error);
cardio::promise<void> xyzm_finish_receive_file_and_rethrow_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t *file,
    std::exception_ptr error);

cardio::promise<void> xyzm_xmodem_send_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file);

cardio::promise<void> xyzm_xmodem_receive_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink);

cardio::promise<void> xyzm_ymodem_send_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file);
cardio::promise<void> xyzm_ymodem_send_batch_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file);

cardio::promise<void> xyzm_ymodem_receive_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink);

cardio::promise<void> xyzm_zmodem_send_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file);
cardio::promise<void> xyzm_zmodem_send_batch_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file);

cardio::promise<void> xyzm_zmodem_receive_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink);

#endif
