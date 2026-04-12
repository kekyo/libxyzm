/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef XYZM_ASYNC_H
#define XYZM_ASYNC_H

#ifndef __cplusplus
#error "libxyzm async API requires C++20."
#endif

#include "base.h"

#include <cardio.h>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

/**
 * @file
 * @brief C++20 coroutine based asynchronous libxyzm API.
 *
 * @remarks Request objects, options, callback tables, observers, and report
 *          storage passed to the entry points must remain alive until the
 *          returned promise completes.
 * @remarks Every promise returned by a transport, source, or sink callback
 *          must belong to the active cardio dispatcher group used by the
 *          transfer.
 * @remarks Transfer observers and monotonic time callbacks remain synchronous.
 * @remarks Asynchronous callback state should be held by the callable object,
 *          for example through lambda captures.
 * @remarks Cancellation is cooperative. Public entry points and asynchronous
 *          callbacks receive a @c cardio::cancellation as their final argument.
 *          Callbacks should observe that signal while waiting for I/O or storage.
 */

/**
 * @brief Normal completion codes used by the C++ asynchronous API.
 */
enum class xyzm_async_status_t {
  ok,  /**< The operation completed successfully. */
  end  /**< A finite source reached its natural end. */
};

/**
 * @brief Base exception for asynchronous transfer failures.
 */
class xyzm_async_error : public std::runtime_error {
 public:
  explicit xyzm_async_error(const char *message) : std::runtime_error(message) {}
  explicit xyzm_async_error(const std::string &message) : std::runtime_error(message) {}
};

/**
 * @brief No progress was made before the timeout expired.
 */
class xyzm_async_timeout_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief The local side cancelled the transfer intentionally.
 */
class xyzm_async_cancelled_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief The remote peer cancelled the transfer.
 */
class xyzm_async_remote_cancelled_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief The transport or local storage reported an I/O failure.
 */
class xyzm_async_io_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief The peer violated the protocol.
 */
class xyzm_async_protocol_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief The caller supplied an invalid argument.
 */
class xyzm_async_argument_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief The requested behavior is not supported.
 */
class xyzm_async_unsupported_error : public xyzm_async_error {
 public:
  using xyzm_async_error::xyzm_async_error;
};

/**
 * @brief File metadata used by the C++ asynchronous API.
 */
struct xyzm_async_file_info_t {
  std::optional<std::string> name;                  /**< File name when known. */
  std::optional<uint64_t> size_bytes;               /**< Logical payload size before protocol padding. */
  std::optional<uint64_t> mtime_unix_seconds;       /**< Modification time in UNIX seconds when known. */
  std::optional<uint32_t> mode;                     /**< Platform-neutral mode bits when known. */
};

/**
 * @brief Transfer outcome summary returned to the asynchronous caller.
 */
struct xyzm_async_transfer_report_t {
  std::optional<xyzm_async_status_t> final_result;  /**< Final normal result when the transfer completed successfully. */
  uint64_t payload_bytes;                           /**< Successfully transferred payload bytes, excluding framing. */
  uint32_t completed_files;                         /**< Number of files completed successfully. */
  uint32_t retry_count;                             /**< Number of protocol retries performed. */
};

/**
 * @brief Payload passed to asynchronous transfer notification callbacks.
 */
struct xyzm_async_transfer_event_t {
  xyzm_transfer_event_kind_t kind;  /**< Event kind. */
  xyzm_transfer_protocol_t protocol;  /**< Protocol family that emitted the event. */
  xyzm_transfer_direction_t direction;  /**< Transfer direction. */
  std::optional<xyzm_async_status_t> result;  /**< Normal result for terminal success events. */
  std::exception_ptr error;  /**< Failure exception for terminal failure events. */
  std::optional<std::reference_wrapper<const xyzm_async_file_info_t>> file_info;  /**< Current file metadata when meaningful. */
  uint64_t payload_delta;  /**< Newly transferred payload bytes for progress events. */
  std::optional<std::reference_wrapper<const xyzm_async_transfer_report_t>> report;  /**< Current transfer report when one was supplied. */
};

/**
 * @brief Optional asynchronous transfer notification sink supplied by the caller.
 */
struct xyzm_async_transfer_observer_t {
  std::function<void(const xyzm_async_transfer_event_t &)> notify;  /**< Transfer notification callback. */
};

/**
 * @brief Writes raw bytes to the underlying asynchronous transport.
 *
 * @param buf Bytes to write.
 * @param timeout_ms Maximum time to wait for progress.
 * @param written_len Output byte count written during this call.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when at least one byte was written.
 * @remarks Partial writes are allowed. When the promise resolves successfully,
 *          @p written_len must be in the range 1..len.
 * @remarks The returned promise must belong to the active cardio dispatcher
 *          group used by the transfer.
 */
typedef std::function<cardio::promise<void>(
    std::span<const uint8_t>,
    uint32_t,
    size_t &,
    cardio::cancellation)> xyzm_async_send_cb;

/**
 * @brief Reads raw bytes from the underlying asynchronous transport.
 *
 * @param buf Destination buffer.
 * @param timeout_ms Maximum time to wait for progress.
 * @param read_len Output byte count read during this call.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when at least one byte was read.
 * @remarks Partial reads are allowed. When the promise resolves successfully,
 *          @p read_len must be in the range 1..capacity.
 * @remarks The returned promise must belong to the active cardio dispatcher
 *          group used by the transfer.
 */
typedef std::function<cardio::promise<void>(
    std::span<uint8_t>,
    uint32_t,
    size_t &,
    cardio::cancellation)> xyzm_async_recv_cb;

/**
 * @brief Returns a monotonic timestamp in milliseconds.
 *
 * @return Monotonic time in milliseconds.
 * @remarks This callback remains synchronous in the first async API revision.
 */
typedef std::function<uint64_t()> xyzm_async_now_ms_cb;

/**
 * @brief Asynchronous raw byte-stream transport callbacks.
 */
struct xyzm_async_link_ops_t {
  xyzm_async_send_cb send;     /**< Byte-stream write callback. */
  xyzm_async_recv_cb recv;     /**< Byte-stream read callback. */
  xyzm_async_now_ms_cb now_ms; /**< Monotonic clock callback. */
};

/**
 * @brief Produces the next file that should be sent.
 *
 * @param info Output metadata for the next file.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving to @ref xyzm_async_status_t::ok when a file is
 *         available, or @ref xyzm_async_status_t::end when no more files remain.
 */
typedef std::function<cardio::promise<xyzm_async_status_t>(
    xyzm_async_file_info_t &,
    cardio::cancellation)> xyzm_async_source_next_cb;

/**
 * @brief Reads payload bytes from the current send-side file.
 *
 * @param buf Destination buffer.
 * @param read_len Output byte count read during this call.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving to @ref xyzm_async_status_t::ok when bytes were read,
 *         or @ref xyzm_async_status_t::end when the current file reached its end.
 */
typedef std::function<cardio::promise<xyzm_async_status_t>(
    std::span<uint8_t>,
    size_t &,
    cardio::cancellation)> xyzm_async_source_read_cb;

/**
 * @brief Moves the current send-side file to a protocol-selected offset.
 *
 * @param offset Logical payload offset that should be read next.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the source was positioned successfully.
 */
typedef std::function<cardio::promise<void>(
    uint64_t,
    cardio::cancellation)> xyzm_async_source_seek_cb;

/**
 * @brief Finalizes a send-side file handle.
 *
 * @param info Metadata originally returned for the file.
 * @param error Final per-file transfer failure, or null on success.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when cleanup succeeded.
 */
typedef std::function<cardio::promise<void>(
    const xyzm_async_file_info_t &,
    std::exception_ptr,
    cardio::cancellation)> xyzm_async_source_end_cb;

/**
 * @brief Asynchronous send-side file enumeration callbacks.
 */
struct xyzm_async_source_ops_t {
  xyzm_async_source_next_cb next;  /**< Produces the next file to send. */
  xyzm_async_source_read_cb read;  /**< Reads payload bytes for the current file. */
  xyzm_async_source_seek_cb seek;  /**< Repositions the current file for resumed ZMODEM sends. */
  xyzm_async_source_end_cb end;    /**< Finalizes the current file handle. */
};

/**
 * @brief Starts receiving a single file.
 *
 * @param info Metadata announced by the protocol.
 * @param resume_offset Output logical payload offset already received.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the sink accepted the file.
 */
typedef std::function<cardio::promise<void>(
    const xyzm_async_file_info_t &,
    uint64_t &,
    cardio::cancellation)> xyzm_async_sink_begin_cb;

/**
 * @brief Consumes payload bytes for the current receive-side file.
 *
 * @param buf Payload bytes supplied by the protocol layer.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the sink accepted the bytes.
 */
typedef std::function<cardio::promise<void>(
    std::span<const uint8_t>,
    cardio::cancellation)> xyzm_async_sink_write_cb;

/**
 * @brief Finalizes a receive-side file handle.
 *
 * @param info Metadata announced for the file.
 * @param error Final per-file transfer failure, or null on success.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when cleanup succeeded.
 */
typedef std::function<cardio::promise<void>(
    const xyzm_async_file_info_t &,
    std::exception_ptr,
    cardio::cancellation)> xyzm_async_sink_end_cb;

/**
 * @brief Asynchronous receive-side file creation callbacks.
 */
struct xyzm_async_sink_ops_t {
  xyzm_async_sink_begin_cb begin; /**< Starts a new receive-side file. */
  xyzm_async_sink_write_cb write; /**< Writes payload bytes into the current file. */
  xyzm_async_sink_end_cb end;     /**< Finalizes the current file handle. */
};

/**
 * @brief Parameters for @ref xyzm_xmodem_send_async.
 *
 * @remarks The request, options, callback tables, and optional report storage
 *          must remain alive until the returned promise completes.
 */
struct xyzm_xmodem_send_async_request_t {
  const xyzm_async_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_async_source_ops_t *source;      /**< Application callbacks producing send-side files. */
  const xyzm_xmodem_send_opts_t *options;     /**< XMODEM send configuration. */
  const xyzm_async_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
};

/**
 * @brief Parameters for @ref xyzm_xmodem_receive_async.
 */
struct xyzm_xmodem_receive_async_request_t {
  const xyzm_async_link_ops_t *link;             /**< Transport callbacks used for the transfer. */
  const xyzm_async_sink_ops_t *sink;             /**< Application callbacks consuming receive-side files. */
  const xyzm_xmodem_receive_opts_t *options;     /**< XMODEM receive configuration. */
  const xyzm_async_transfer_observer_t *observer; /**< Optional transfer notification sink. */
};

/**
 * @brief Parameters for @ref xyzm_ymodem_send_async.
 */
struct xyzm_ymodem_send_async_request_t {
  const xyzm_async_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_async_source_ops_t *source;      /**< Application callbacks producing send-side files. */
  const xyzm_ymodem_opts_t *options;          /**< YMODEM transfer configuration. */
  const xyzm_async_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
};

/**
 * @brief Parameters for @ref xyzm_ymodem_receive_async.
 */
struct xyzm_ymodem_receive_async_request_t {
  const xyzm_async_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_async_sink_ops_t *sink;          /**< Application callbacks consuming receive-side files. */
  const xyzm_ymodem_opts_t *options;          /**< YMODEM transfer configuration. */
  const xyzm_async_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
};

/**
 * @brief Parameters for @ref xyzm_zmodem_send_async.
 */
struct xyzm_zmodem_send_async_request_t {
  const xyzm_async_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_async_source_ops_t *source;      /**< Application callbacks producing send-side files. */
  const xyzm_zmodem_opts_t *options;          /**< ZMODEM transfer configuration. */
  const xyzm_async_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
};

/**
 * @brief Parameters for @ref xyzm_zmodem_receive_async.
 */
struct xyzm_zmodem_receive_async_request_t {
  const xyzm_async_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_async_sink_ops_t *sink;          /**< Application callbacks consuming receive-side files. */
  const xyzm_zmodem_opts_t *options;          /**< ZMODEM transfer configuration. */
  const xyzm_async_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
};

/**
 * @brief Sends one XMODEM transfer over the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_xmodem_send_async(
    const xyzm_xmodem_send_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Receives one XMODEM transfer from the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_xmodem_receive_async(
    const xyzm_xmodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Sends one YMODEM transfer over the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_ymodem_send_async(
    const xyzm_ymodem_send_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Sends one YMODEM batch transfer over the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_ymodem_send_batch_async(
    const xyzm_ymodem_send_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Receives one YMODEM transfer from the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_ymodem_receive_async(
    const xyzm_ymodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Receives one YMODEM batch transfer from the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_ymodem_receive_batch_async(
    const xyzm_ymodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Sends one ZMODEM transfer over the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_zmodem_send_async(
    const xyzm_zmodem_send_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Sends one ZMODEM batch transfer over the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_zmodem_send_batch_async(
    const xyzm_zmodem_send_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Receives one ZMODEM transfer from the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_zmodem_receive_async(
    const xyzm_zmodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

/**
 * @brief Receives one ZMODEM batch transfer from the configured asynchronous link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written when the promise completes.
 * @param cancellation Cancellation signal for this transfer.
 * @return Promise resolving when the transfer completes successfully.
 */
cardio::promise<void> xyzm_zmodem_receive_batch_async(
    const xyzm_zmodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation);

#endif
