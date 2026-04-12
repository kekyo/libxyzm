/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_async_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  XYZM_YMODEM_ASYNC_RECEIVE_WRITE_BUFFER_SIZE = 64 * 1024,
};

/**
 * @brief Reads a control byte and folds a CAN CAN sequence into remote cancellation.
 */
static inline cardio::promise<void> xyzm_wait_control_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &value,
    uint32_t timeout_ms) {
  co_await xyzm_recv_byte_async(runtime, value, timeout_ms);
  if (value == XYZM_CAN) {
    uint8_t second;

    try {
      co_await xyzm_recv_byte_async(runtime, second, runtime.options->block_timeout_ms);
      if (second == XYZM_CAN) {
        xyzm_throw_remote_cancelled_async();
      }
    } catch (const xyzm_async_timeout_error &) {
    } catch (const xyzm_async_io_error &) {
    }
  }
  co_return;
}

/**
 * @brief Waits for a specific handshake byte while counting retryable timeouts.
 */
static inline cardio::promise<void> xyzm_wait_for_byte_async(
    xyzm_async_runtime_t &runtime,
    uint8_t expected) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    try {
      co_await xyzm_wait_control_async(runtime, control, runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control == expected) {
      co_return;
    }
    if ((control == XYZM_ACK) && (expected == XYZM_G_REQ)) {
      continue;
    }
    if (control == XYZM_CAN) {
      xyzm_throw_remote_cancelled_async();
    }
    xyzm_report_retry_async(runtime);
  }
  xyzm_throw_timeout_async();
}

/**
 * @brief Waits for the initial YMODEM receiver request and resolves auto mode.
 */
static inline cardio::promise<void> xyzm_wait_for_ymodem_request_async(
    xyzm_async_runtime_t &runtime,
    bool &streaming) {
  switch (runtime.options->ymodem_variant) {
    case XYZM_YMODEM_VARIANT_STANDARD:
      streaming = false;
      co_await xyzm_wait_for_byte_async(runtime, XYZM_CRC_REQ);
      co_return;
    case XYZM_YMODEM_VARIANT_G:
      streaming = true;
      co_await xyzm_wait_for_byte_async(runtime, XYZM_G_REQ);
      co_return;
    case XYZM_YMODEM_VARIANT_AUTO:
      break;
    default:
      xyzm_throw_unsupported_async();
  }

  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    try {
      co_await xyzm_wait_control_async(runtime, control, runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control == XYZM_CRC_REQ) {
      streaming = false;
      co_return;
    }
    if (control == XYZM_G_REQ) {
      streaming = true;
      co_return;
    }
    if (control == XYZM_CAN) {
      xyzm_throw_remote_cancelled_async();
    }
    xyzm_report_retry_async(runtime);
  }
  xyzm_throw_timeout_async();
}

static inline bool xyzm_is_ymodem_request_echo(uint8_t value) {
  return value == XYZM_CRC_REQ || value == XYZM_G_REQ || value == '\r' || value == '\n';
}

/**
 * @brief Reads a YMODEM frame header while ignoring local request echo.
 *
 * @remarks
 * This is not part of Chuck Forsberg's X/YMODEM/YMODEM-g protocol flow. It is a
 * defensive compatibility hack for echo-back tty/pty/ssh/telnet paths where the
 * receiver's own `C` or `G` request byte can be reflected before the sender's
 * metadata block. Without this guard the receiver treats the echoed request
 * byte as a frame header and aborts before the real SOH metadata block arrives.
 */
static inline cardio::promise<void> xyzm_recv_ymodem_frame_header_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &header) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    co_await xyzm_wait_control_async(runtime, header, runtime.options->block_timeout_ms);
    if (header == XYZM_SOH || header == XYZM_STX || header == XYZM_EOT) {
      co_return;
    }
    if (header == XYZM_CAN) {
      xyzm_throw_remote_cancelled_async();
    }
    if (xyzm_is_ymodem_request_echo(header)) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    xyzm_throw_protocol_async();
  }
  xyzm_throw_timeout_async();
}

/**
 * @brief Serializes YMODEM filename and size metadata into the initial 128-byte block.
 */
static inline bool xyzm_make_ymodem_header(
    const xyzm_async_file_info_t &info,
    uint8_t payload[128]) {
  size_t offset;

  memset(payload, 0, 128u);
  offset = 0u;
  if (info.name.has_value() && !info.name->empty()) {
    size_t name_len;

    name_len = info.name->size();
    if (name_len > 126u) {
      name_len = 126u;
    }
    memcpy(payload, info.name->data(), name_len);
    offset = name_len + 1u;
  } else {
    payload[0] = '\0';
    return false;
  }

  if (info.size_bytes.has_value() && (offset < 127u)) {
    char size_text[32];
    int written_len;

    written_len = snprintf(
        size_text,
        sizeof(size_text),
        "%llu",
        (unsigned long long)*info.size_bytes);
    if (written_len > 0) {
      size_t copy_len;

      copy_len = (size_t)written_len;
      if (copy_len > (127u - offset)) {
        copy_len = 127u - offset;
      }
      memcpy(payload + offset, size_text, copy_len);
    }
  }
  return true;
}

/**
 * @brief Parses the metadata header and detects the terminal empty header block.
 */
static inline bool xyzm_parse_ymodem_header(
    xyzm_async_receive_file_t &file,
    const uint8_t payload[128],
    bool &is_terminal_header) {
  const char *size_text;
  size_t name_len;

  is_terminal_header = (payload[0] == '\0');
  file.info = {};
  file.active = false;
  if (is_terminal_header) {
    return true;
  }

  name_len = 0u;
  for (size_t index = 0u; index < 128u; ++index) {
    if (payload[index] == '\0') {
      break;
    }
    ++name_len;
  }
  if (name_len == 0u) {
    return false;
  }
  file.info.name = std::string((const char *)payload, name_len);

  size_text = (const char *)(payload + name_len + 1u);
  if ((name_len + 1u) < 128u && (*size_text != '\0')) {
    char *end_ptr;
    unsigned long long value;

    value = strtoull(size_text, &end_ptr, 10);
    if ((end_ptr != size_text) && ((*end_ptr == '\0') || (*end_ptr == ' '))) {
      file.info.size_bytes = (uint64_t)value;
    }
  }
  return true;
}

/**
 * @brief Sends one CRC-protected YMODEM block and optionally waits for the ACK path.
 */
static inline cardio::promise<void> xyzm_send_crc_block_async(
    xyzm_async_runtime_t &runtime,
    uint8_t header,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool wait_for_ack,
    bool retry_ack_failures) {
  uint8_t frame[3u + 1024u + 2u];
  size_t frame_len;
  uint16_t crc;

  frame[0] = header;
  frame[1] = block_number;
  frame[2] = (uint8_t)(255u - block_number);
  memcpy(frame + 3u, payload, payload_len);
  crc = xyzm_crc16_xmodem_update_buffer(0u, payload, payload_len);
  frame[3u + payload_len] = (uint8_t)(crc >> 8);
  frame[3u + payload_len + 1u] = (uint8_t)(crc & 0xffu);
  frame_len = 3u + payload_len + 2u;

  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    co_await xyzm_send_exact_async(
        runtime,
        std::span<const uint8_t>(frame, frame_len),
        runtime.options->block_timeout_ms);
    if (!wait_for_ack) {
      co_return;
    }

    try {
      co_await xyzm_wait_control_async(runtime, control, runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      if (!retry_ack_failures) {
        co_return;
      }
      xyzm_report_retry_async(runtime);
      continue;
    } catch (const xyzm_async_io_error &) {
      if (!retry_ack_failures) {
        co_return;
      }
      throw;
    }
    if (control == XYZM_ACK) {
      co_return;
    }
    if (control == XYZM_CAN) {
      xyzm_throw_remote_cancelled_async();
    }
    if (!retry_ack_failures) {
      co_return;
    }
    if (control == XYZM_NAK) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    xyzm_report_retry_async(runtime);
  }

  xyzm_throw_timeout_async();
}

/**
 * @brief Handles the YMODEM and YMODEM-g EOT shutdown differences behind one helper.
 */
static inline cardio::promise<void> xyzm_send_ymodem_eot_async(
    xyzm_async_runtime_t &runtime,
    bool streaming) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    co_await xyzm_send_byte_async(runtime, XYZM_EOT, runtime.options->block_timeout_ms);
    try {
      co_await xyzm_wait_control_async(runtime, control, runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control == XYZM_ACK) {
      /*
       * Chuck Forsberg's X/YMODEM Protocol Reference (Figure 3, "YMODEM
       * Batch Transmission Session") shows the strict non-streaming shutdown
       * path as EOT, NAK, EOT, ACK. Accept a direct ACK after the first EOT
       * as a deliberate interoperability deviation because lrzsz's sb/rb
       * implementations complete that shorter handshake.
       */
      co_return;
    }
    if (streaming) {
      if (control == XYZM_CAN) {
        xyzm_throw_remote_cancelled_async();
      }
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control != XYZM_NAK) {
      if (control == XYZM_CAN) {
        xyzm_throw_remote_cancelled_async();
      }
      xyzm_report_retry_async(runtime);
      continue;
    }

    co_await xyzm_send_byte_async(runtime, XYZM_EOT, runtime.options->block_timeout_ms);
    try {
      co_await xyzm_wait_for_byte_async(runtime, XYZM_ACK);
      co_return;
    } catch (const xyzm_async_timeout_error &) {
    }
  }
  xyzm_throw_timeout_async();
}

static inline cardio::promise<void> xyzm_recv_crc_block_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &header,
    uint8_t &block_number,
    uint8_t *payload,
    size_t &payload_len);

/**
 * @brief Receives the terminal empty pathname header that closes one YMODEM batch.
 *
 * @remarks
 * Chuck Forsberg's X/YMODEM Protocol Reference (Figure 3, "YMODEM Batch
 * Transmission Session") shows the receiver asking for the terminal empty
 * pathname header after the EOT handshake and then consuming that SOH block
 * immediately. Real-world lrzsz deployments over PTY-backed shells can leak an
 * echoed 'C', CR/LF noise, or even an extra EOT into this phase. Treat those
 * bytes as retryable terminal-fetch noise so strict peers still follow the
 * reference sequence, while lrzsz-style peers remain interoperable.
 */
static inline cardio::promise<bool> xyzm_recv_ymodem_terminal_header_async(
    xyzm_async_runtime_t &runtime,
    xyzm_async_receive_file_t &trailer,
    bool allow_terminal_loss) {
  bool saw_timeout;

  saw_timeout = false;
  trailer = {};
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t header = 0u;
    uint8_t block_number = 0u;
    uint8_t payload[1024];
    size_t payload_len = 0u;
    bool is_terminal_header = false;
    enum {
      terminal_error_none,
      terminal_error_timeout,
      terminal_error_io,
      terminal_error_protocol
    } terminal_error = terminal_error_none;

    try {
      co_await xyzm_recv_crc_block_async(runtime, header, block_number, payload, payload_len);
    } catch (const xyzm_async_timeout_error &) {
      terminal_error = terminal_error_timeout;
    } catch (const xyzm_async_io_error &) {
      terminal_error = terminal_error_io;
    } catch (const xyzm_async_protocol_error &) {
      terminal_error = terminal_error_protocol;
    }
    if (terminal_error == terminal_error_timeout) {
      if (allow_terminal_loss) {
        co_return false;
      }
      saw_timeout = true;
      co_await xyzm_send_byte_async(runtime, XYZM_CRC_REQ, runtime.options->block_timeout_ms);
      if (attempt == runtime.options->retry_limit) {
        xyzm_throw_timeout_async();
      }
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (terminal_error == terminal_error_io) {
      if (allow_terminal_loss) {
        co_return false;
      }
      xyzm_throw_io_async();
    }
    if (terminal_error == terminal_error_protocol) {
      if (allow_terminal_loss) {
        co_return false;
      }
      if (attempt == runtime.options->retry_limit) {
        xyzm_throw_protocol_async();
      }
      xyzm_report_retry_async(runtime);
      continue;
    }

    if (header == XYZM_EOT) {
      if (allow_terminal_loss) {
        co_return false;
      }
      /*
       * This is another deliberate lrzsz-compatibility deviation from the
       * strict reference sequence above: tolerate one more EOT while the
       * sender/receiver pair converges on the terminal pathname block.
       */
      co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
      co_await xyzm_send_byte_async(runtime, XYZM_CRC_REQ, runtime.options->block_timeout_ms);
    } else if ((header == XYZM_SOH) &&
               (block_number == 0u) &&
               xyzm_parse_ymodem_header(trailer, payload, is_terminal_header) &&
               is_terminal_header) {
      co_return true;
    } else if (allow_terminal_loss) {
      co_return false;
    }

    if (attempt == runtime.options->retry_limit) {
      if (saw_timeout) {
        xyzm_throw_timeout_async();
      }
      xyzm_throw_protocol_async();
    }
    xyzm_report_retry_async(runtime);
  }
  if (saw_timeout) {
    xyzm_throw_timeout_async();
  }
  xyzm_throw_protocol_async();
}

/**
 * @brief Fills the next YMODEM payload buffer from the source stream.
 */
static inline cardio::promise<void> xyzm_fill_data_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    uint8_t *payload,
    size_t payload_len,
    size_t &actual_len) {
  size_t filled_len;

  filled_len = 0u;
  while (filled_len < payload_len) {
    size_t read_len;
    xyzm_async_status_t status;

    read_len = 0u;
    status = co_await xyzm_source_read_async(
        runtime,
        source,
        std::span<uint8_t>(payload + filled_len, payload_len - filled_len),
        read_len);
    if (status == xyzm_async_status_t::end) {
      break;
    }
    if (read_len == 0u) {
      xyzm_throw_io_async();
    }
    filled_len += read_len;
  }
  actual_len = filled_len;
  co_return;
}

/**
 * @brief Receives one CRC-backed YMODEM block and validates its framing bytes.
 */
static inline cardio::promise<void> xyzm_recv_crc_block_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &header,
    uint8_t &block_number,
    uint8_t *payload,
    size_t &payload_len) {
  uint8_t block_header[2];
  size_t data_len;
  uint8_t crc_bytes[2];
  uint16_t expected_crc;

  co_await xyzm_recv_ymodem_frame_header_async(runtime, header);
  if (header == XYZM_EOT) {
    payload_len = 0u;
    co_return;
  }
  if ((header != XYZM_SOH) && (header != XYZM_STX)) {
    xyzm_throw_protocol_async();
  }

  data_len = (header == XYZM_SOH) ? 128u : 1024u;
  co_await xyzm_recv_exact_async(
      runtime,
      std::span<uint8_t>(block_header, sizeof(block_header)),
      runtime.options->block_timeout_ms);
  block_number = block_header[0];
  if ((uint8_t)(block_header[0] + block_header[1]) != 0xffu) {
    xyzm_throw_protocol_async();
  }

  co_await xyzm_recv_exact_async(
      runtime,
      std::span<uint8_t>(payload, data_len),
      runtime.options->block_timeout_ms);
  co_await xyzm_recv_exact_async(
      runtime,
      std::span<uint8_t>(crc_bytes, sizeof(crc_bytes)),
      runtime.options->block_timeout_ms);
  expected_crc = xyzm_crc16_xmodem_update_buffer(0u, payload, data_len);
  if ((crc_bytes[0] != (uint8_t)(expected_crc >> 8)) ||
      (crc_bytes[1] != (uint8_t)(expected_crc & 0xffu))) {
    xyzm_throw_protocol_async();
  }

  payload_len = data_len;
  co_return;
}

static inline cardio::promise<void> xyzm_send_ymodem_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file,
    bool streaming) {
  uint8_t metadata_payload[128];
  uint8_t block_number;

  if (!xyzm_make_ymodem_header(file->info, metadata_payload)) {
    xyzm_throw_argument_async();
  }
  std::exception_ptr error;
  try {
    co_await xyzm_send_crc_block_async(
        runtime,
        XYZM_SOH,
        0u,
        metadata_payload,
        sizeof(metadata_payload),
        !streaming,
        true);

    co_await xyzm_wait_for_byte_async(runtime, streaming ? XYZM_G_REQ : XYZM_CRC_REQ);

    block_number = 1u;
    for (;;) {
      uint8_t payload[1024];
      size_t actual_len;

      memset(payload, runtime.options->pad_byte, sizeof(payload));
      co_await xyzm_fill_data_async(
          runtime,
          source,
          payload,
          sizeof(payload),
          actual_len);
      if (actual_len == 0u) {
        break;
      }

      co_await xyzm_send_crc_block_async(
          runtime,
          XYZM_STX,
          block_number,
          payload,
          sizeof(payload),
          !streaming,
          true);
      xyzm_report_payload_async(runtime, actual_len, file->info);
      block_number = (uint8_t)(block_number + 1u);
    }

    co_await xyzm_send_ymodem_eot_async(runtime, streaming);
    co_await xyzm_wait_for_byte_async(runtime, streaming ? XYZM_G_REQ : XYZM_CRC_REQ);
    co_return;
  } catch (...) {
    error = std::current_exception();
  }
  co_await xyzm_send_cancel_and_rethrow_async(runtime, error);
}

static inline cardio::promise<void> xyzm_send_ymodem_terminal_header_async(
    xyzm_async_runtime_t &runtime,
    bool streaming) {
  uint8_t metadata_payload[128];

  memset(metadata_payload, 0, sizeof(metadata_payload));
  /*
   * The terminal empty pathname block is only the YMODEM batch terminator. The
   * file bytes have already been accepted by the receiver and acknowledged by
   * the EOT handshake before this block is sent.
   *
   * lrzsz's rb sends ACK for this block and then immediately restores the tty.
   * Its io_mode(0, 0) implementation runs tcdrain() followed by
   * tcflush(TCIOFLUSH). The preceding fflush(stdout) only hands the ACK to the
   * local tty/pty; it does not guarantee that a sender behind pty, telnetd, TCP,
   * and an application event loop has already observed that byte. The final
   * TCIOFLUSH can therefore race with the ACK's propagation and leave a sender
   * waiting even though rb already consumed the terminal header.
   *
   * lrzsz's sb often hides the race because it waits for that ACK immediately
   * and does not make a final empty-header failure very visible once the payload
   * is complete. An async library sender that retries the terminal block after
   * rb exits can expose the race by writing SOH 00 FF into the user's shell.
   *
   * Preserve retries for real metadata and payload blocks. For this terminator,
   * write the frame once, give the ACK one chance to arrive, and then finish on
   * timeout, peer close, NAK, or stale receiver-request bytes. CAN still means
   * remote cancellation, and a failure to write the frame is still an error.
   */
  co_await xyzm_send_crc_block_async(
      runtime,
      XYZM_SOH,
      0u,
      metadata_payload,
      sizeof(metadata_payload),
      !streaming,
      false);
  co_return;
}

cardio::promise<void> xyzm_ymodem_send_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file) {
  bool streaming = false;

  co_await xyzm_wait_for_ymodem_request_async(runtime, streaming);
  co_await xyzm_send_ymodem_file_async(runtime, source, file, streaming);
  co_await xyzm_send_ymodem_terminal_header_async(runtime, streaming);
  xyzm_report_completed_file_async(runtime);
  co_return;
}

cardio::promise<void> xyzm_ymodem_send_batch_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file) {
  bool streaming = false;

  co_await xyzm_wait_for_ymodem_request_async(runtime, streaming);

  for (;;) {
    co_await xyzm_send_ymodem_file_async(runtime, source, file, streaming);
    xyzm_report_completed_file_async(runtime);
    co_await xyzm_finish_send_file_async(runtime, source, file, nullptr);
    if ((co_await xyzm_send_single_file_async(runtime, source, file)) ==
        xyzm_async_status_t::end) {
      co_await xyzm_send_ymodem_terminal_header_async(runtime, streaming);
      co_return;
    }
  }
}

static inline void xyzm_get_ymodem_receive_mode_async(
    xyzm_async_runtime_t &runtime,
    bool &streaming) {
  switch (runtime.options->ymodem_variant) {
    case XYZM_YMODEM_VARIANT_STANDARD:
      streaming = false;
      return;
    case XYZM_YMODEM_VARIANT_G:
      streaming = true;
      return;
    case XYZM_YMODEM_VARIANT_AUTO:
    default:
      xyzm_throw_unsupported_async();
  }
}

static inline cardio::promise<void> xyzm_send_cancel_preserving_async(
    xyzm_async_runtime_t &runtime,
    std::exception_ptr error) {
  try {
    co_await xyzm_send_cancel_async(runtime);
  } catch (...) {
  }
  std::rethrow_exception(error);
}

static inline cardio::promise<void> xyzm_flush_ymodem_receive_payload_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t &file,
    uint8_t *pending_payload,
    size_t &pending_len) {
  if (pending_len == 0u) {
    co_return;
  }
  std::exception_ptr write_error;
  try {
    co_await xyzm_sink_write_async(
        runtime,
        sink,
        std::span<const uint8_t>(pending_payload, pending_len));
  } catch (...) {
    write_error = std::current_exception();
  }
  if (write_error) {
    co_await xyzm_send_cancel_preserving_async(runtime, write_error);
  }
  xyzm_report_payload_async(runtime, pending_len, file.info);
  pending_len = 0u;
  co_return;
}

static inline cardio::promise<void> xyzm_finish_ymodem_receive_success_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t &file) {
  co_await xyzm_finish_receive_file_async(runtime, sink, &file, nullptr);
  xyzm_report_completed_file_async(runtime);
  co_return;
}

static inline cardio::promise<void> xyzm_receive_ymodem_file_payload_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t &file,
    bool streaming,
    bool request_next_header) {
  uint8_t expected_block;
  uint64_t remaining_bytes;
  uint8_t pending_payload[XYZM_YMODEM_ASYNC_RECEIVE_WRITE_BUFFER_SIZE];
  size_t pending_len;

  if (!streaming) {
    co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
  }
  co_await xyzm_send_byte_async(
      runtime,
      streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
      runtime.options->block_timeout_ms);

  expected_block = 1u;
  remaining_bytes = file.info.size_bytes.value_or(UINT64_MAX);
  pending_len = 0u;
  for (;;) {
    uint8_t header;
    uint8_t block_number;
    uint8_t payload[1024];
    size_t payload_len;

    std::exception_ptr protocol_error;
    try {
      co_await xyzm_recv_crc_block_async(runtime, header, block_number, payload, payload_len);
    } catch (const xyzm_async_protocol_error &) {
      protocol_error = std::current_exception();
    }
    if (protocol_error) {
      if (streaming) {
        co_await xyzm_send_cancel_preserving_async(runtime, protocol_error);
      }
      xyzm_report_retry_async(runtime);
      co_await xyzm_send_byte_async(runtime, XYZM_NAK, runtime.options->block_timeout_ms);
      continue;
    }

    if (header == XYZM_EOT) {
      if ((remaining_bytes != UINT64_MAX) && (remaining_bytes != 0u)) {
        xyzm_throw_protocol_async();
      }
      co_await xyzm_flush_ymodem_receive_payload_async(
          runtime,
          sink,
          file,
          pending_payload,
          pending_len);
      if (streaming) {
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
      } else {
        uint8_t second_eot;

        co_await xyzm_send_byte_async(runtime, XYZM_NAK, runtime.options->block_timeout_ms);
        co_await xyzm_wait_control_async(runtime, second_eot, runtime.options->block_timeout_ms);
        if (second_eot != XYZM_EOT) {
          xyzm_throw_protocol_async();
        }
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
      }
      if (request_next_header) {
        co_await xyzm_send_byte_async(
            runtime,
            streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
            runtime.options->block_timeout_ms);
      }
      co_return;
    }

    if (block_number == expected_block) {
      size_t write_len;

      if (remaining_bytes == 0u) {
        xyzm_throw_protocol_async();
      }
      write_len = payload_len;
      if ((remaining_bytes != UINT64_MAX) && (remaining_bytes < write_len)) {
        write_len = (size_t)remaining_bytes;
      }
      if (write_len > 0u) {
        size_t copied_len = 0u;
        while (copied_len < write_len) {
          const size_t available_len =
              sizeof(pending_payload) - pending_len;
          const size_t copy_len =
              (write_len - copied_len) < available_len ?
              (write_len - copied_len) :
              available_len;
          memcpy(pending_payload + pending_len, payload + copied_len, copy_len);
          pending_len += copy_len;
          copied_len += copy_len;
          if (pending_len == sizeof(pending_payload)) {
            co_await xyzm_flush_ymodem_receive_payload_async(
                runtime,
                sink,
                file,
                pending_payload,
                pending_len);
          }
        }
        if (remaining_bytes != UINT64_MAX) {
          remaining_bytes -= write_len;
        }
      }
      if (!streaming) {
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
      }
      expected_block = (uint8_t)(expected_block + 1u);
      continue;
    }

    if (block_number == (uint8_t)(expected_block - 1u)) {
      if (!streaming) {
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
      }
      continue;
    }

    if (streaming) {
      co_await xyzm_send_cancel_preserving_async(runtime, xyzm_make_protocol_error_async());
    }
    xyzm_report_retry_async(runtime);
    co_await xyzm_send_byte_async(runtime, XYZM_NAK, runtime.options->block_timeout_ms);
  }
}

static inline cardio::promise<void> xyzm_begin_ymodem_receive_file_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t &file) {
  uint64_t resume_offset;

  resume_offset = 0u;
  std::exception_ptr begin_error;
  try {
    co_await xyzm_sink_begin_async(runtime, sink, file.info, resume_offset);
  } catch (...) {
    begin_error = std::current_exception();
  }
  if (begin_error) {
    co_await xyzm_send_cancel_preserving_async(runtime, begin_error);
  }
  file.active = true;
  if (resume_offset != 0u) {
    try {
      co_await xyzm_send_cancel_async(runtime);
    } catch (...) {
    }
    co_await xyzm_finish_receive_file_and_rethrow_async(
        runtime,
        sink,
        &file,
        xyzm_make_unsupported_error_async());
  }
  xyzm_notify_file_started_async(runtime, file.info);
  co_return;
}

static inline cardio::promise<void> xyzm_ymodem_receive_batch_loop_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink) {
  bool streaming;
  uint32_t completed_files;

  xyzm_get_ymodem_receive_mode_async(runtime, streaming);
  co_await xyzm_send_byte_async(
      runtime,
      streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
      runtime.options->handshake_timeout_ms);

  completed_files = 0u;
  for (;;) {
    xyzm_async_receive_file_t file{};
    uint8_t header;
    uint8_t block_number;
    uint8_t payload[1024];
    size_t payload_len;
    bool is_terminal_header;

    try {
      co_await xyzm_recv_crc_block_async(runtime, header, block_number, payload, payload_len);
    } catch (const xyzm_async_timeout_error &) {
      if (streaming && completed_files > 0u) {
        co_return;
      }
      throw;
    } catch (const xyzm_async_io_error &) {
      if (streaming && completed_files > 0u) {
        co_return;
      }
      throw;
    } catch (const xyzm_async_protocol_error &) {
      if (streaming && completed_files > 0u) {
        co_return;
      }
      throw;
    }
    if ((header != XYZM_SOH) || (block_number != 0u)) {
      xyzm_throw_protocol_async();
    }
    if (!xyzm_parse_ymodem_header(file, payload, is_terminal_header)) {
      xyzm_throw_protocol_async();
    }
    if (is_terminal_header) {
      co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
      co_return;
    }

    co_await xyzm_begin_ymodem_receive_file_async(runtime, sink, file);
    std::exception_ptr file_error;
    try {
      co_await xyzm_receive_ymodem_file_payload_async(runtime, sink, file, streaming, true);
      co_await xyzm_finish_ymodem_receive_success_async(runtime, sink, file);
      completed_files += 1u;
    } catch (...) {
      file_error = std::current_exception();
    }
    if (file_error) {
      co_await xyzm_finish_receive_file_and_rethrow_async(runtime, sink, &file, file_error);
    }
  }
}

cardio::promise<void> xyzm_ymodem_receive_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink) {
  xyzm_async_receive_file_t file{};
  xyzm_async_receive_file_t trailer{};
  uint8_t header;
  uint8_t block_number;
  uint8_t payload[1024];
  size_t payload_len;
  bool is_terminal_header;
  bool streaming;

  if (runtime.options->batch_mode) {
    co_await xyzm_ymodem_receive_batch_loop_async(runtime, sink);
    co_return;
  }

  xyzm_get_ymodem_receive_mode_async(runtime, streaming);
  co_await xyzm_send_byte_async(
      runtime,
      streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
      runtime.options->handshake_timeout_ms);
  co_await xyzm_recv_crc_block_async(runtime, header, block_number, payload, payload_len);
  if ((header != XYZM_SOH) || (block_number != 0u)) {
    xyzm_throw_protocol_async();
  }
  if (!xyzm_parse_ymodem_header(file, payload, is_terminal_header) || is_terminal_header) {
    xyzm_throw_protocol_async();
  }

  co_await xyzm_begin_ymodem_receive_file_async(runtime, sink, file);
  std::exception_ptr receive_error;
  try {
    co_await xyzm_receive_ymodem_file_payload_async(runtime, sink, file, streaming, true);
    if (!(co_await xyzm_recv_ymodem_terminal_header_async(runtime, trailer, streaming))) {
      /*
       * This is not specified by the X/YMODEM/YMODEM-g protocol. lrzsz's `sb`
       * keeps its no-ACK `Optiong` behavior even for the terminal empty
       * pathname block, so a tty/pty/ssh/telnet path can close after the EOT
       * ACK with the file payload complete but before the terminal block is
       * observed locally. Treat that post-EOT close/timeout as a successful
       * lrzsz YMODEM-g single-file completion instead of failing an already
       * durable payload.
       */
      co_await xyzm_finish_ymodem_receive_success_async(runtime, sink, file);
      co_return;
    }
    try {
      co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      if (!streaming) {
        throw;
      }
    } catch (const xyzm_async_io_error &) {
      if (!streaming) {
        throw;
      }
    }
    /*
     * lrzsz can exit immediately after writing the terminal header on a
     * YMODEM-g single-file transfer. At this point the payload and terminal
     * header have both been consumed, so a closed peer during the final ACK is
     * a successful completion rather than a transfer failure.
     */
    co_await xyzm_finish_ymodem_receive_success_async(runtime, sink, file);
    co_return;
  } catch (...) {
    receive_error = std::current_exception();
  }
  co_await xyzm_finish_receive_file_and_rethrow_async(runtime, sink, &file, receive_error);
}
