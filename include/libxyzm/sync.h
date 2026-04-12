/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef XYZM_SYNC_H
#define XYZM_SYNC_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status codes used by the C synchronous API and callback interfaces.
 */
typedef enum xyzm_status {
  XYZM_STATUS_OK = 0,             /**< The operation completed successfully. */
  XYZM_STATUS_END = 1,            /**< A finite source reached its natural end. */
  XYZM_STATUS_TIMEOUT = 2,        /**< No progress was made before the timeout expired. */
  XYZM_STATUS_CANCELLED = 3,      /**< The local side cancelled the transfer intentionally. */
  XYZM_STATUS_REMOTE_CANCELLED =
      4,                     /**< The remote peer cancelled the transfer. */
  XYZM_STATUS_IO_ERROR = 5,       /**< The transport or local storage reported an I/O failure. */
  XYZM_STATUS_PROTOCOL_ERROR = 6, /**< The peer violated the protocol. */
  XYZM_STATUS_ARGUMENT_ERROR = 7, /**< The caller supplied an invalid argument. */
  XYZM_STATUS_UNSUPPORTED = 8     /**< The requested behavior is not supported. */
} xyzm_status_t;

/**
 * @brief Transfer outcome summary returned to the synchronous caller.
 */
typedef struct xyzm_transfer_report {
  xyzm_status_t final_status;   /**< Final status returned by the entry point. */
  uint64_t payload_bytes;       /**< Successfully transferred payload bytes, excluding framing. */
  uint32_t completed_files;     /**< Number of files completed successfully. */
  uint32_t retry_count;         /**< Number of protocol retries performed. */
} xyzm_transfer_report_t;

/**
 * @brief Validity bits for @ref xyzm_file_info_t.
 */
typedef enum xyzm_file_info_field {
  XYZM_FILE_INFO_NAME = 1u << 0, /**< @ref xyzm_file_info_t.name is valid. */
  XYZM_FILE_INFO_SIZE = 1u << 1, /**< @ref xyzm_file_info_t.size_bytes is valid. */
  XYZM_FILE_INFO_MTIME = 1u << 2, /**< @ref xyzm_file_info_t.mtime_unix_seconds is valid. */
  XYZM_FILE_INFO_MODE = 1u << 3   /**< @ref xyzm_file_info_t.mode is valid. */
} xyzm_file_info_field_t;

/**
 * @brief File metadata used by the C synchronous API.
 */
typedef struct xyzm_file_info {
  uint32_t valid_mask;          /**< Bitwise OR of @ref xyzm_file_info_field_t values. */
  const char *name;             /**< File name owned by the application callback. */
  uint64_t size_bytes;          /**< Logical payload size before protocol padding. */
  uint64_t mtime_unix_seconds;  /**< Modification time in UNIX seconds. */
  uint32_t mode;                /**< Platform-neutral mode bits chosen by the application. */
} xyzm_file_info_t;

/**
 * @brief Payload passed to synchronous transfer notification callbacks.
 */
typedef struct xyzm_transfer_event {
  xyzm_transfer_event_kind_t kind;           /**< Event kind. */
  xyzm_transfer_protocol_t protocol;         /**< Protocol family that emitted the event. */
  xyzm_transfer_direction_t direction;       /**< Transfer direction. */
  xyzm_status_t status;                      /**< Status for terminal events, otherwise @ref XYZM_STATUS_OK. */
  const xyzm_file_info_t *file_info;         /**< Current file metadata when meaningful, otherwise NULL. */
  uint64_t payload_delta;                    /**< Newly transferred payload bytes for progress events. */
  const xyzm_transfer_report_t *report;      /**< Current transfer report when one was supplied. */
} xyzm_transfer_event_t;

/**
 * @brief Receives transfer lifecycle notifications from the synchronous protocol core.
 *
 * @param opaque User pointer from @ref xyzm_transfer_observer_t.
 * @param event Transfer event payload owned by the library.
 * @remarks @p event, @ref xyzm_transfer_event_t.file_info, and
 *          @ref xyzm_transfer_event_t.report are valid only while the callback
 *          is running. Copy any required values before returning.
 */
typedef void (*xyzm_transfer_event_cb)(
    void *opaque,
    const xyzm_transfer_event_t *event);

/**
 * @brief Optional synchronous transfer notification sink supplied by the caller.
 */
typedef struct xyzm_transfer_observer {
  void *opaque;                       /**< User storage passed to @ref notify. */
  xyzm_transfer_event_cb notify;      /**< Transfer notification callback. */
} xyzm_transfer_observer_t;

/**
 * @brief Writes raw bytes to the underlying transport.
 *
 * @param opaque User pointer from @ref xyzm_link_ops_t.
 * @param buf Bytes to write.
 * @param len Requested byte count.
 * @param timeout_ms Maximum time to wait for progress.
 * @param written_len Output byte count written during this call.
 * @return @ref XYZM_STATUS_OK when at least one byte was written.
 * @retval XYZM_STATUS_TIMEOUT No byte was written before @p timeout_ms expired.
 * @retval XYZM_STATUS_CANCELLED The local side aborted the transfer.
 * @retval XYZM_STATUS_IO_ERROR The transport failed.
 * @remarks Partial writes are allowed. When @ref XYZM_STATUS_OK is returned,
 *          @p written_len must be in the range 1..len.
 * @remarks The transport must preserve raw bytes. PTY cooked mode, CR/LF
 *          translation, telnet negotiation, and SSH terminal setup are outside
 *          the library boundary and must be handled before these callbacks.
 */
typedef xyzm_status_t (*xyzm_send_cb)(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len);

/**
 * @brief Reads raw bytes from the underlying transport.
 *
 * @param opaque User pointer from @ref xyzm_link_ops_t.
 * @param buf Destination buffer.
 * @param capacity Maximum number of bytes to read.
 * @param timeout_ms Maximum time to wait for progress.
 * @param read_len Output byte count read during this call.
 * @return @ref XYZM_STATUS_OK when at least one byte was read.
 * @retval XYZM_STATUS_TIMEOUT No byte was received before @p timeout_ms expired.
 * @retval XYZM_STATUS_CANCELLED The local side aborted the transfer.
 * @retval XYZM_STATUS_IO_ERROR The transport failed.
 * @remarks Partial reads are allowed. When @ref XYZM_STATUS_OK is returned,
 *          @p read_len must be in the range 1..capacity.
 * @remarks The transport must be configured as an 8-bit-clean byte stream by
 *          the application. Protocol-level escaping can mitigate only a subset
 *          of terminal-path issues.
 */
typedef xyzm_status_t (*xyzm_recv_cb)(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len);

/**
 * @brief Returns a monotonic timestamp in milliseconds.
 *
 * @param opaque User pointer from @ref xyzm_link_ops_t.
 * @return Monotonic time in milliseconds.
 * @remarks The implementation must be monotonic across the whole transfer.
 */
typedef uint64_t (*xyzm_now_ms_cb)(void *opaque);

/**
 * @brief Raw byte-stream transport callbacks.
 */
typedef struct xyzm_link_ops {
  void *opaque;          /**< User storage passed back to every transport callback. */
  xyzm_send_cb send;     /**< Byte-stream write callback. */
  xyzm_recv_cb recv;     /**< Byte-stream read callback. */
  xyzm_now_ms_cb now_ms; /**< Monotonic clock callback. */
} xyzm_link_ops_t;

/**
 * @brief Produces the next file that should be sent.
 *
 * @param opaque User pointer from @ref xyzm_source_ops_t.
 * @param info Output metadata for the next file.
 * @param file_opaque Output per-file handle passed to later source callbacks.
 * @return @ref XYZM_STATUS_OK when a file is available.
 * @retval XYZM_STATUS_END No more files remain.
 * @retval XYZM_STATUS_CANCELLED The local side aborted the transfer.
 * @retval XYZM_STATUS_IO_ERROR The local source failed.
 * @remarks Progress 1 treats both XMODEM and YMODEM as single-file transfers.
 *          The callback shape remains enumerative so later protocols can extend it.
 */
typedef xyzm_status_t (*xyzm_source_next_cb)(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque);

/**
 * @brief Reads payload bytes from the current send-side file.
 *
 * @param opaque User pointer from @ref xyzm_source_ops_t.
 * @param file_opaque Per-file handle produced by @ref xyzm_source_next_cb.
 * @param buf Destination buffer.
 * @param capacity Maximum number of bytes to read.
 * @param read_len Output byte count read during this call.
 * @return @ref XYZM_STATUS_OK when at least one payload byte was read.
 * @retval XYZM_STATUS_END The current file reached its end.
 * @retval XYZM_STATUS_CANCELLED The local side aborted the transfer.
 * @retval XYZM_STATUS_IO_ERROR The local source failed.
 * @remarks A successful read must set @p read_len to a value in the range
 *          1..capacity. The @ref XYZM_STATUS_END case must set @p read_len to 0.
 */
typedef xyzm_status_t (*xyzm_source_read_cb)(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len);

/**
 * @brief Moves the current send-side file to a protocol-selected offset.
 *
 * @param opaque User pointer from @ref xyzm_source_ops_t.
 * @param file_opaque Per-file handle produced by @ref xyzm_source_next_cb.
 * @param offset Logical payload offset that should be read next.
 * @return @ref XYZM_STATUS_OK when the source was positioned successfully.
 * @retval XYZM_STATUS_UNSUPPORTED The source cannot reposition this file.
 * @retval XYZM_STATUS_IO_ERROR The local source failed while seeking.
 * @remarks ZMODEM uses this callback when the receiver replies with a non-zero
 *          `ZRPOS` value to resume a partial transfer. XMODEM and YMODEM never
 *          call this callback.
 */
typedef xyzm_status_t (*xyzm_source_seek_cb)(
    void *opaque,
    void *file_opaque,
    uint64_t offset);

/**
 * @brief Finalizes a send-side file handle.
 *
 * @param opaque User pointer from @ref xyzm_source_ops_t.
 * @param file_opaque Per-file handle produced by @ref xyzm_source_next_cb.
 * @param info Metadata originally returned for the file.
 * @param result Final per-file transfer result known by the library.
 * @return @ref XYZM_STATUS_OK when cleanup succeeded.
 * @retval XYZM_STATUS_IO_ERROR Cleanup failed locally.
 * @remarks Called exactly once for every file returned with
 *          @ref XYZM_STATUS_OK from @ref xyzm_source_next_cb.
 */
typedef xyzm_status_t (*xyzm_source_end_cb)(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result);

/**
 * @brief Send-side file enumeration callbacks.
 */
typedef struct xyzm_source_ops {
  void *opaque;              /**< User storage passed back to every source callback. */
  xyzm_source_next_cb next;  /**< Produces the next file to send. */
  xyzm_source_read_cb read;  /**< Reads payload bytes for the current file. */
  xyzm_source_seek_cb seek;  /**< Repositions the current file for resumed ZMODEM sends. */
  xyzm_source_end_cb end;    /**< Finalizes the current file handle. */
} xyzm_source_ops_t;

/**
 * @brief Starts receiving a single file.
 *
 * @param opaque User pointer from @ref xyzm_sink_ops_t.
 * @param info Metadata announced by the protocol.
 * @param resume_offset Output logical payload offset that has already been
 *        received and should be resumed from. Set to zero for normal receives.
 * @param file_opaque Output per-file handle passed to later sink callbacks.
 * @return @ref XYZM_STATUS_OK when the sink accepted the file.
 * @retval XYZM_STATUS_CANCELLED The local side rejected the file intentionally.
 * @retval XYZM_STATUS_IO_ERROR The local sink failed to prepare storage.
 * @retval XYZM_STATUS_UNSUPPORTED The announced metadata cannot be handled.
 * @remarks XMODEM can deliver @p info with an empty @p valid_mask. YMODEM can
 *          provide file name and size fields for its single transferred file.
 * @remarks ZMODEM honors non-zero @p resume_offset values by sending `ZRPOS`
 *          for that offset. XMODEM and YMODEM require this value to remain zero.
 */
typedef xyzm_status_t (*xyzm_sink_begin_cb)(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque);

/**
 * @brief Consumes payload bytes for the current receive-side file.
 *
 * @param opaque User pointer from @ref xyzm_sink_ops_t.
 * @param file_opaque Per-file handle produced by @ref xyzm_sink_begin_cb.
 * @param buf Payload bytes supplied by the protocol layer.
 * @param len Payload byte count.
 * @return @ref XYZM_STATUS_OK when the sink accepted the bytes.
 * @retval XYZM_STATUS_CANCELLED The local side aborted the transfer intentionally.
 * @retval XYZM_STATUS_IO_ERROR The local sink failed.
 * @remarks XMODEM payload is not auto-trimmed. Final padding bytes remain visible
 *          to the sink because the wire format does not encode the original size.
 */
typedef xyzm_status_t (*xyzm_sink_write_cb)(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len);

/**
 * @brief Finalizes a receive-side file handle.
 *
 * @param opaque User pointer from @ref xyzm_sink_ops_t.
 * @param file_opaque Per-file handle produced by @ref xyzm_sink_begin_cb.
 * @param info Metadata announced for the file.
 * @param result Final per-file transfer result known by the library.
 * @return @ref XYZM_STATUS_OK when cleanup succeeded.
 * @retval XYZM_STATUS_IO_ERROR Cleanup failed locally.
 * @remarks Called exactly once for every file accepted by
 *          @ref xyzm_sink_begin_cb, even when the transfer later fails.
 */
typedef xyzm_status_t (*xyzm_sink_end_cb)(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result);

/**
 * @brief Receive-side file creation callbacks.
 */
typedef struct xyzm_sink_ops {
  void *opaque;              /**< User storage passed back to every sink callback. */
  xyzm_sink_begin_cb begin;  /**< Starts a new receive-side file. */
  xyzm_sink_write_cb write;  /**< Writes payload bytes into the current file. */
  xyzm_sink_end_cb end;      /**< Finalizes the current file handle. */
} xyzm_sink_ops_t;

/**
 * @brief Parameters for @ref xyzm_xmodem_send.
 */
typedef struct xyzm_xmodem_send_request {
  const xyzm_link_ops_t *link;               /**< Transport callbacks used for the transfer. */
  const xyzm_source_ops_t *source;           /**< Application callbacks producing send-side files. */
  const xyzm_xmodem_send_opts_t *options;    /**< XMODEM send configuration. */
  const xyzm_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
} xyzm_xmodem_send_request_t;

/**
 * @brief Parameters for @ref xyzm_xmodem_receive.
 */
typedef struct xyzm_xmodem_receive_request {
  const xyzm_link_ops_t *link;                  /**< Transport callbacks used for the transfer. */
  const xyzm_sink_ops_t *sink;                  /**< Application callbacks consuming receive-side files. */
  const xyzm_xmodem_receive_opts_t *options;    /**< XMODEM receive configuration. */
  const xyzm_transfer_observer_t *observer;     /**< Optional transfer notification sink. */
} xyzm_xmodem_receive_request_t;

/**
 * @brief Parameters for @ref xyzm_ymodem_send.
 */
typedef struct xyzm_ymodem_send_request {
  const xyzm_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_source_ops_t *source;      /**< Application callbacks producing send-side files. */
  const xyzm_ymodem_opts_t *options;    /**< YMODEM transfer configuration. */
  const xyzm_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
} xyzm_ymodem_send_request_t;

/**
 * @brief Parameters for @ref xyzm_ymodem_receive.
 */
typedef struct xyzm_ymodem_receive_request {
  const xyzm_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_sink_ops_t *sink;          /**< Application callbacks consuming receive-side files. */
  const xyzm_ymodem_opts_t *options;    /**< YMODEM transfer configuration. */
  const xyzm_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
} xyzm_ymodem_receive_request_t;

/**
 * @brief Parameters for @ref xyzm_zmodem_send.
 */
typedef struct xyzm_zmodem_send_request {
  const xyzm_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_source_ops_t *source;      /**< Application callbacks producing send-side files. */
  const xyzm_zmodem_opts_t *options;    /**< ZMODEM transfer configuration. */
  const xyzm_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
} xyzm_zmodem_send_request_t;

/**
 * @brief Parameters for @ref xyzm_zmodem_receive.
 */
typedef struct xyzm_zmodem_receive_request {
  const xyzm_link_ops_t *link;          /**< Transport callbacks used for the transfer. */
  const xyzm_sink_ops_t *sink;          /**< Application callbacks consuming receive-side files. */
  const xyzm_zmodem_opts_t *options;    /**< ZMODEM transfer configuration. */
  const xyzm_transfer_observer_t *observer;  /**< Optional transfer notification sink. */
} xyzm_zmodem_receive_request_t;

/**
 * @brief Sends one XMODEM transfer over the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK All requested files were transferred successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @retval XYZM_STATUS_UNSUPPORTED The chosen XMODEM mode is not supported.
 * @remarks This call is synchronous. XMODEM requires exactly one file from
 *          @ref xyzm_source_next_cb.
 */
xyzm_status_t xyzm_xmodem_send(
    const xyzm_xmodem_send_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Receives one XMODEM transfer from the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK The announced file was received successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @retval XYZM_STATUS_UNSUPPORTED The chosen XMODEM mode is not supported.
 */
xyzm_status_t xyzm_xmodem_receive(
    const xyzm_xmodem_receive_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Sends one YMODEM transfer over the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK All requested files were transferred successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @retval XYZM_STATUS_UNSUPPORTED The chosen YMODEM variant is not supported.
 * @remarks Progress 1 requires exactly one file from @ref xyzm_source_next_cb.
 */
xyzm_status_t xyzm_ymodem_send(
    const xyzm_ymodem_send_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Sends one YMODEM batch transfer over the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK Every source file was transferred successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @retval XYZM_STATUS_UNSUPPORTED The chosen YMODEM variant is not supported.
 * @remarks The source callback is enumerated until it returns @ref XYZM_STATUS_END.
 */
xyzm_status_t xyzm_ymodem_send_batch(
    const xyzm_ymodem_send_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Receives one YMODEM transfer from the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK Every announced file was received successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @retval XYZM_STATUS_UNSUPPORTED The chosen YMODEM variant is not supported.
 */
xyzm_status_t xyzm_ymodem_receive(
    const xyzm_ymodem_receive_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Receives one YMODEM batch transfer from the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK Every announced file was received successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @retval XYZM_STATUS_UNSUPPORTED The chosen YMODEM variant is not supported.
 * @remarks The sink callback is invoked once for each announced non-terminal file.
 */
xyzm_status_t xyzm_ymodem_receive_batch(
    const xyzm_ymodem_receive_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Sends one ZMODEM transfer over the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK All requested files were transferred successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 */
xyzm_status_t xyzm_zmodem_send(
    const xyzm_zmodem_send_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Sends one ZMODEM batch transfer over the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK Every source file was transferred successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @remarks The source callback is enumerated until it returns @ref XYZM_STATUS_END.
 */
xyzm_status_t xyzm_zmodem_send_batch(
    const xyzm_zmodem_send_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Receives one ZMODEM transfer from the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK Every announced file was received successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 */
xyzm_status_t xyzm_zmodem_receive(
    const xyzm_zmodem_receive_request_t *request,
    xyzm_transfer_report_t *report);

/**
 * @brief Receives one ZMODEM batch transfer from the configured link.
 *
 * @param request Transfer request.
 * @param report Optional transfer summary written on return.
 * @return Final transfer status.
 * @retval XYZM_STATUS_OK Every announced file was received successfully.
 * @retval XYZM_STATUS_ARGUMENT_ERROR A required callback or option was missing.
 * @remarks The sink callback is invoked once for each announced file.
 */
xyzm_status_t xyzm_zmodem_receive_batch(
    const xyzm_zmodem_receive_request_t *request,
    xyzm_transfer_report_t *report);

#ifdef __cplusplus
}
#endif

#endif
