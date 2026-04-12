/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_async_internal.h"

#include <limits.h>
#include <string.h>

enum {
  XYZM_XMODEM_ASYNC_RECEIVE_WRITE_BUFFER_SIZE = 64 * 1024,
};

/**
 * @brief Captures the framing rules selected by the active XMODEM mode.
 */
struct xyzm_xmodem_mode_t {
  size_t block_len;
  bool use_crc;
  bool checksum_auto;
  uint8_t start_char;
};

/**
 * @brief Resolves the preferred outbound XMODEM payload size.
 */
static inline void xyzm_get_xmodem_block_len_async(
    xyzm_async_runtime_t &runtime,
    size_t &block_len) {
  switch (runtime.options->xmodem_packet_size) {
    case XYZM_XMODEM_PACKET_SIZE_128:
      block_len = 128u;
      return;
    case XYZM_XMODEM_PACKET_SIZE_1K:
      block_len = 1024u;
      return;
    default:
      xyzm_throw_unsupported_async();
  }
}

/**
 * @brief Resolves one XMODEM send mode from the configured checksum and packet-size axes.
 */
static inline void xyzm_get_xmodem_send_mode_async(
    xyzm_async_runtime_t &runtime,
    xyzm_xmodem_mode_t &mode) {
  xyzm_get_xmodem_block_len_async(runtime, mode.block_len);

  switch (runtime.options->xmodem_checksum_mode) {
    case XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM:
      mode.use_crc = false;
      mode.checksum_auto = false;
      mode.start_char = XYZM_NAK;
      return;
    case XYZM_XMODEM_CHECKSUM_MODE_CRC:
      mode.use_crc = true;
      mode.checksum_auto = false;
      mode.start_char = XYZM_CRC_REQ;
      return;
    case XYZM_XMODEM_CHECKSUM_MODE_AUTO:
      mode.use_crc = false;
      mode.checksum_auto = true;
      mode.start_char = 0u;
      return;
    default:
      xyzm_throw_unsupported_async();
  }
}

/**
 * @brief Resolves one XMODEM receive mode from the configured checksum axis.
 */
static inline void xyzm_get_xmodem_receive_mode_async(
    xyzm_async_runtime_t &runtime,
    xyzm_xmodem_mode_t &mode) {
  mode.block_len = 128u;
  mode.checksum_auto = false;
  switch (runtime.options->xmodem_checksum_mode) {
    case XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM:
      mode.use_crc = false;
      mode.start_char = XYZM_NAK;
      return;
    case XYZM_XMODEM_CHECKSUM_MODE_CRC:
      mode.use_crc = true;
      mode.start_char = XYZM_CRC_REQ;
      return;
    case XYZM_XMODEM_CHECKSUM_MODE_AUTO:
      xyzm_throw_argument_async();
    default:
      xyzm_throw_unsupported_async();
  }
}

/**
 * @brief Reads a control byte and collapses a CAN CAN sequence into remote cancellation.
 */
static inline cardio::promise<void> xyzm_read_control_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &value,
    uint32_t timeout_ms) {
  co_await xyzm_recv_byte_async(runtime, value, timeout_ms);
  if (value == XYZM_CAN) {
    uint8_t next_value;

    try {
      co_await xyzm_recv_byte_async(
          runtime,
          next_value,
          runtime.options->block_timeout_ms);
      if (next_value == XYZM_CAN) {
        xyzm_throw_remote_cancelled_async();
      }
    } catch (const xyzm_async_timeout_error &) {
    } catch (const xyzm_async_io_error &) {
    }
  }
  co_return;
}

static inline cardio::promise<void> xyzm_flush_xmodem_receive_payload_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink,
    xyzm_async_receive_file_t &file,
    uint8_t *pending_payload,
    size_t &pending_len) {
  if (pending_len == 0u) {
    co_return;
  }
  std::exception_ptr error;
  try {
    co_await xyzm_sink_write_async(
        runtime,
        sink,
        std::span<const uint8_t>(pending_payload, pending_len));
  } catch (...) {
    error = std::current_exception();
  }
  if (error) {
    co_await xyzm_send_cancel_and_rethrow_async(runtime, error);
  }
  xyzm_report_payload_async(runtime, pending_len, file.info);
  pending_len = 0u;
  co_return;
}

/**
 * @brief Waits for the receiver's startup request, retrying handshake timeouts.
 */
static inline cardio::promise<void> xyzm_wait_xmodem_start_async(
    xyzm_async_runtime_t &runtime,
    xyzm_xmodem_mode_t &mode) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    try {
      co_await xyzm_read_control_async(
          runtime,
          control,
          runtime.options->handshake_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if ((!mode.checksum_auto && (control == mode.start_char)) ||
        (mode.checksum_auto && ((control == XYZM_NAK) || (control == XYZM_CRC_REQ)))) {
      if (mode.checksum_auto) {
        mode.use_crc = (control == XYZM_CRC_REQ);
      }
      co_return;
    }
    if (control == XYZM_CAN) {
      xyzm_throw_remote_cancelled_async();
    }
  }
  xyzm_throw_timeout_async();
}

/**
 * @brief Builds one XMODEM frame and appends the checksum or CRC trailer.
 */
static inline cardio::promise<void> xyzm_send_frame_async(
    xyzm_async_runtime_t &runtime,
    uint8_t header,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool use_crc) {
  uint8_t frame[3u + 1024u + 2u];
  frame[0] = header;
  frame[1] = block_number;
  frame[2] = (uint8_t)(255u - block_number);
  memcpy(frame + 3u, payload, payload_len);
  size_t frame_len;
  if (use_crc) {
    uint16_t crc;

    crc = xyzm_crc16_xmodem_update_buffer(0u, payload, payload_len);
    frame[3u + payload_len] = (uint8_t)(crc >> 8);
    frame[3u + payload_len + 1u] = (uint8_t)(crc & 0xffu);
    frame_len = 3u + payload_len + 2u;
  } else {
    frame[3u + payload_len] = xyzm_checksum8_update_buffer(0u, payload, payload_len);
    frame_len = 3u + payload_len + 1u;
  }

  co_await xyzm_send_exact_async(
      runtime,
      std::span<const uint8_t>(frame, frame_len),
      runtime.options->block_timeout_ms);
  co_return;
}

/**
 * @brief Retries a data block until it is acknowledged or a terminal condition occurs.
 */
static inline cardio::promise<void> xyzm_send_block_with_retry_async(
    xyzm_async_runtime_t &runtime,
    uint8_t header,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool use_crc) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    co_await xyzm_send_frame_async(
        runtime,
        header,
        block_number,
        payload,
        payload_len,
        use_crc);

    try {
      co_await xyzm_read_control_async(
          runtime,
          control,
          runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control == XYZM_ACK) {
      co_return;
    }
    if (control == XYZM_NAK) {
      xyzm_report_retry_async(runtime);
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
 * @brief Performs the two-step EOT handshake used by XMODEM-family senders.
 */
static inline cardio::promise<void> xyzm_send_eot_sequence_async(
    xyzm_async_runtime_t &runtime) {
  for (unsigned attempt = 0u; attempt <= runtime.options->retry_limit; ++attempt) {
    uint8_t control;

    co_await xyzm_send_byte_async(runtime, XYZM_EOT, runtime.options->block_timeout_ms);
    try {
      co_await xyzm_read_control_async(
          runtime,
          control,
          runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control == XYZM_ACK) {
      co_return;
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
      co_await xyzm_read_control_async(
          runtime,
          control,
          runtime.options->block_timeout_ms);
    } catch (const xyzm_async_timeout_error &) {
      xyzm_report_retry_async(runtime);
      continue;
    }
    if (control == XYZM_ACK) {
      co_return;
    }
    if (control == XYZM_CAN) {
      xyzm_throw_remote_cancelled_async();
    }
    xyzm_report_retry_async(runtime);
  }

  xyzm_throw_timeout_async();
}

/**
 * @brief Reads source bytes until the next payload buffer is full or EOF is reached.
 */
static inline cardio::promise<void> xyzm_fill_block_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    uint8_t *payload,
    size_t payload_len,
    size_t &actual_len) {
  size_t filled_len = 0u;
  while (filled_len < payload_len) {
    size_t read_len = 0u;
    xyzm_async_status_t status = co_await xyzm_source_read_async(
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
 * @brief Parses a received frame and validates its block number and integrity trailer.
 */
static inline cardio::promise<void> xyzm_read_frame_async(
    xyzm_async_runtime_t &runtime,
    uint8_t &header,
    uint8_t &block_number,
    uint8_t *payload,
    size_t &payload_len,
    bool use_crc) {
  block_number = 0u;
  payload_len = 0u;
  co_await xyzm_read_control_async(runtime, header, runtime.options->block_timeout_ms);
  if ((header == XYZM_EOT) || (header == XYZM_CAN)) {
    co_return;
  }
  if ((header != XYZM_SOH) && (header != XYZM_STX)) {
    xyzm_throw_protocol_async();
  }

  size_t data_len = (header == XYZM_SOH) ? 128u : 1024u;
  uint8_t block_header[2];
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
  if (use_crc) {
    uint8_t crc_bytes[2];
    uint16_t expected_crc;

    co_await xyzm_recv_exact_async(
        runtime,
        std::span<uint8_t>(crc_bytes, sizeof(crc_bytes)),
        runtime.options->block_timeout_ms);
    expected_crc = xyzm_crc16_xmodem_update_buffer(0u, payload, data_len);
    if ((crc_bytes[0] != (uint8_t)(expected_crc >> 8)) ||
        (crc_bytes[1] != (uint8_t)(expected_crc & 0xffu))) {
      xyzm_throw_protocol_async();
    }
  } else {
    uint8_t checksum;

    co_await xyzm_recv_exact_async(
        runtime,
        std::span<uint8_t>(&checksum, 1u),
        runtime.options->block_timeout_ms);
    if (checksum != xyzm_checksum8_update_buffer(0u, payload, data_len)) {
      xyzm_throw_protocol_async();
    }
  }

  payload_len = data_len;
  co_return;
}

cardio::promise<void> xyzm_xmodem_send_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_source_ops_t *source,
    xyzm_async_send_file_t *file) {
  xyzm_xmodem_mode_t mode;
  xyzm_get_xmodem_send_mode_async(runtime, mode);
  co_await xyzm_wait_xmodem_start_async(runtime, mode);

  uint8_t block_number = 1u;
  while (true) {
    uint8_t payload[1024];
    size_t frame_len;
    size_t actual_len;

    frame_len = mode.block_len;
    memset(payload, runtime.options->pad_byte, frame_len);
    std::exception_ptr error;
    try {
      co_await xyzm_fill_block_async(runtime, source, payload, frame_len, actual_len);
    } catch (...) {
      error = std::current_exception();
    }
    if (error) {
      co_await xyzm_send_cancel_and_rethrow_async(runtime, error);
    }
    if (actual_len == 0u) {
      break;
    }
    if ((mode.block_len == 1024u) && (actual_len <= 128u)) {
      frame_len = 128u;
    }

    error = nullptr;
    try {
      co_await xyzm_send_block_with_retry_async(
          runtime,
          (frame_len == 1024u) ? XYZM_STX : XYZM_SOH,
          block_number,
          payload,
          frame_len,
          mode.use_crc);
    } catch (...) {
      error = std::current_exception();
    }
    if (error) {
      co_await xyzm_send_cancel_and_rethrow_async(runtime, error);
    }
    xyzm_report_payload_async(runtime, actual_len, file->info);
    block_number = (uint8_t)(block_number + 1u);
  }

  co_await xyzm_send_eot_sequence_async(runtime);
  xyzm_report_completed_files_async(runtime, 1u);
  co_return;
}

cardio::promise<void> xyzm_xmodem_receive_impl_async(
    xyzm_async_runtime_t &runtime,
    const xyzm_async_sink_ops_t *sink) {
  xyzm_async_receive_file_t file{};
  xyzm_xmodem_mode_t mode;
  uint64_t resume_offset = 0u;
  xyzm_get_xmodem_receive_mode_async(runtime, mode);
  co_await xyzm_sink_begin_async(runtime, sink, file.info, resume_offset);
  file.active = true;

  std::exception_ptr receive_error;
  try {
    if (resume_offset != 0u) {
      xyzm_throw_unsupported_async();
    }
    xyzm_notify_file_started_async(runtime, file.info);

    uint8_t expected_block = 1u;
    unsigned retry_count = 0u;
    bool started = false;
    uint8_t pending_payload[XYZM_XMODEM_ASYNC_RECEIVE_WRITE_BUFFER_SIZE];
    size_t pending_len = 0u;
    co_await xyzm_send_byte_async(
        runtime,
        mode.start_char,
        runtime.options->handshake_timeout_ms);

    for (;;) {
      uint8_t header;
      uint8_t block_number;
      uint8_t payload[1024];
      size_t payload_len;

      bool retryable_error = false;
      std::exception_ptr read_error;
      try {
        co_await xyzm_read_frame_async(
            runtime,
            header,
            block_number,
            payload,
            payload_len,
            mode.use_crc);
      } catch (const xyzm_async_timeout_error &) {
        retryable_error = true;
        read_error = std::current_exception();
      } catch (const xyzm_async_protocol_error &) {
        retryable_error = true;
        read_error = std::current_exception();
      }
      if (retryable_error) {
        if (retry_count >= runtime.options->retry_limit) {
          std::rethrow_exception(read_error);
        }
        retry_count += 1u;
        xyzm_report_retry_async(runtime);
        co_await xyzm_send_byte_async(
            runtime,
            mode.start_char,
            runtime.options->block_timeout_ms);
        continue;
      }

      if (header == XYZM_EOT) {
        uint8_t second_eot;

        co_await xyzm_flush_xmodem_receive_payload_async(
            runtime,
            sink,
            file,
            pending_payload,
            pending_len);
        co_await xyzm_send_byte_async(runtime, XYZM_NAK, runtime.options->block_timeout_ms);
        co_await xyzm_read_control_async(runtime, second_eot, runtime.options->block_timeout_ms);
        if (second_eot != XYZM_EOT) {
          xyzm_throw_protocol_async();
        }
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
        xyzm_report_completed_files_async(runtime, started ? 1u : 0u);
        co_await xyzm_finish_receive_file_async(runtime, sink, &file, nullptr);
        co_return;
      }

      if (block_number == expected_block) {
        started = true;
        retry_count = 0u;
        size_t copied_len = 0u;
        while (copied_len < payload_len) {
          const size_t available_len =
              sizeof(pending_payload) - pending_len;
          const size_t copy_len =
              (payload_len - copied_len) < available_len ?
              (payload_len - copied_len) :
              available_len;
          memcpy(pending_payload + pending_len, payload + copied_len, copy_len);
          pending_len += copy_len;
          copied_len += copy_len;
          if (pending_len == sizeof(pending_payload)) {
            co_await xyzm_flush_xmodem_receive_payload_async(
                runtime,
                sink,
                file,
                pending_payload,
                pending_len);
          }
        }
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
        expected_block = (uint8_t)(expected_block + 1u);
        continue;
      }

      if (block_number == (uint8_t)(expected_block - 1u)) {
        co_await xyzm_send_byte_async(runtime, XYZM_ACK, runtime.options->block_timeout_ms);
        continue;
      }

      if (retry_count >= runtime.options->retry_limit) {
        xyzm_throw_protocol_async();
      }
      retry_count += 1u;
      xyzm_report_retry_async(runtime);
      co_await xyzm_send_byte_async(
          runtime,
          mode.start_char,
          runtime.options->block_timeout_ms);
    }
  } catch (...) {
    receive_error = std::current_exception();
  }
  co_await xyzm_finish_receive_file_and_rethrow_async(runtime, sink, &file, receive_error);
}
