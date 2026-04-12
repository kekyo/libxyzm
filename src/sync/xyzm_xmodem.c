/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_sync_internal.h"

#include <limits.h>
#include <string.h>

/**
 * @brief Captures the framing rules selected by the active XMODEM mode.
 */
typedef struct xyzm_xmodem_mode {
  size_t block_len;
  bool use_crc;
  bool checksum_auto;
  uint8_t start_char;
} xyzm_xmodem_mode_t;

/**
 * @brief Resolves the preferred outbound XMODEM payload size.
 *
 * @param runtime Active transfer state that holds the selected packet size.
 * @param block_len Output payload size used when constructing outbound blocks.
 * @return @ref XYZM_STATUS_OK when the packet size is handled by this module.
 * @retval XYZM_STATUS_UNSUPPORTED The active packet size is not handled by this module.
 */
static inline xyzm_status_t xyzm_get_xmodem_block_len(
    xyzm_runtime_t *runtime,
    size_t *block_len) {
  switch (runtime->options->xmodem_packet_size) {
    case XYZM_XMODEM_PACKET_SIZE_128:
      *block_len = 128u;
      return XYZM_STATUS_OK;
    case XYZM_XMODEM_PACKET_SIZE_1K:
      *block_len = 1024u;
      return XYZM_STATUS_OK;
    default:
      return XYZM_STATUS_UNSUPPORTED;
  }
}

/**
 * @brief Resolves one XMODEM send mode from the configured checksum and packet-size axes.
 *
 * @param runtime Active transfer state that holds the selected send configuration.
 * @param mode Output mode description for the selected configuration.
 * @return @ref XYZM_STATUS_OK when the configuration is handled by this module.
 * @retval XYZM_STATUS_UNSUPPORTED The active send configuration is not handled by this module.
 */
static inline xyzm_status_t xyzm_get_xmodem_send_mode(
    xyzm_runtime_t *runtime,
    xyzm_xmodem_mode_t *mode) {
  xyzm_status_t status = xyzm_get_xmodem_block_len(runtime, &mode->block_len);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  switch (runtime->options->xmodem_checksum_mode) {
    case XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM:
      mode->use_crc = false;
      mode->checksum_auto = false;
      mode->start_char = XYZM_NAK;
      return XYZM_STATUS_OK;
    case XYZM_XMODEM_CHECKSUM_MODE_CRC:
      mode->use_crc = true;
      mode->checksum_auto = false;
      mode->start_char = XYZM_CRC_REQ;
      return XYZM_STATUS_OK;
    case XYZM_XMODEM_CHECKSUM_MODE_AUTO:
      mode->use_crc = false;
      mode->checksum_auto = true;
      mode->start_char = 0u;
      return XYZM_STATUS_OK;
    default:
      return XYZM_STATUS_UNSUPPORTED;
  }
}

/**
 * @brief Resolves one XMODEM receive mode from the configured checksum axis.
 *
 * @param runtime Active transfer state that holds the selected receive configuration.
 * @param mode Output mode description for the selected configuration.
 * @return @ref XYZM_STATUS_OK when the configuration is handled by this module.
 * @retval XYZM_STATUS_ARGUMENT_ERROR The receive configuration requested auto checksum selection.
 * @retval XYZM_STATUS_UNSUPPORTED The active receive configuration is not handled by this module.
 */
static inline xyzm_status_t xyzm_get_xmodem_receive_mode(
    xyzm_runtime_t *runtime,
    xyzm_xmodem_mode_t *mode) {
  mode->block_len = 128u;
  mode->checksum_auto = false;
  switch (runtime->options->xmodem_checksum_mode) {
    case XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM:
      mode->use_crc = false;
      mode->start_char = XYZM_NAK;
      return XYZM_STATUS_OK;
    case XYZM_XMODEM_CHECKSUM_MODE_CRC:
      mode->use_crc = true;
      mode->start_char = XYZM_CRC_REQ;
      return XYZM_STATUS_OK;
    case XYZM_XMODEM_CHECKSUM_MODE_AUTO:
      return XYZM_STATUS_ARGUMENT_ERROR;
    default:
      return XYZM_STATUS_UNSUPPORTED;
  }
}

/**
 * @brief Reads a control byte and collapses a CAN CAN sequence into remote cancellation.
 *
 * @param runtime Active transfer state.
 * @param value Output control byte.
 * @param timeout_ms Maximum time to wait for the first control byte.
 * @return Completion status for the control read.
 * @retval XYZM_STATUS_OK A control byte was read successfully.
 * @retval XYZM_STATUS_TIMEOUT No byte arrived before the timeout expired.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer sent `CAN CAN`.
 */
static inline xyzm_status_t xyzm_read_control(
    xyzm_runtime_t *runtime,
    uint8_t *value,
    uint32_t timeout_ms) {
  xyzm_status_t status = xyzm_recv_byte(runtime, value, timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if (*value == XYZM_CAN) {
    uint8_t next_value;

    status = xyzm_recv_byte(runtime, &next_value, runtime->options->block_timeout_ms);
    if ((status == XYZM_STATUS_OK) && (next_value == XYZM_CAN)) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Waits for the receiver's startup request, retrying handshake timeouts.
 *
 * @param runtime Active transfer state.
 * @param mode Active send-side XMODEM framing mode.
 * @return Final handshake status.
 * @retval XYZM_STATUS_OK One supported startup byte was observed.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted without the expected byte.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_wait_xmodem_start(
    xyzm_runtime_t *runtime,
    xyzm_xmodem_mode_t *mode) {
  for (unsigned attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t control;
    xyzm_status_t status = xyzm_read_control(
        runtime,
        &control,
        runtime->options->handshake_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if ((!mode->checksum_auto && (control == mode->start_char)) ||
        (mode->checksum_auto && ((control == XYZM_NAK) || (control == XYZM_CRC_REQ)))) {
      if (mode->checksum_auto) {
        mode->use_crc = (control == XYZM_CRC_REQ);
      }
      return XYZM_STATUS_OK;
    }
    if (control == XYZM_CAN) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
  }
  return XYZM_STATUS_TIMEOUT;
}

/**
 * @brief Builds one XMODEM frame and appends the checksum or CRC trailer.
 *
 * @param runtime Active transfer state.
 * @param header Frame start byte such as @ref XYZM_SOH or @ref XYZM_STX.
 * @param block_number Sequential block number to encode.
 * @param payload Block payload bytes.
 * @param payload_len Number of payload bytes to encode.
 * @param use_crc Whether the trailer should use CRC16 instead of checksum8.
 * @return Status returned by the underlying transport write path.
 */
static inline xyzm_status_t xyzm_send_frame(
    xyzm_runtime_t *runtime,
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

  return xyzm_send_exact(runtime, frame, frame_len, runtime->options->block_timeout_ms);
}

/**
 * @brief Retries a data block until it is acknowledged or a terminal condition occurs.
 *
 * @param runtime Active transfer state.
 * @param header Frame start byte such as @ref XYZM_SOH or @ref XYZM_STX.
 * @param block_number Sequential block number to encode.
 * @param payload Block payload bytes.
 * @param payload_len Number of payload bytes to encode.
 * @param use_crc Whether the trailer should use CRC16 instead of checksum8.
 * @return Final status for the block exchange.
 * @retval XYZM_STATUS_OK The peer acknowledged the block.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted without an acknowledgement.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_send_block_with_retry(
    xyzm_runtime_t *runtime,
    uint8_t header,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool use_crc) {
  for (unsigned attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    xyzm_status_t status = xyzm_send_frame(
        runtime,
        header,
        block_number,
        payload,
        payload_len,
        use_crc);
    if (status != XYZM_STATUS_OK) {
      return status;
    }

    uint8_t control;
    status = xyzm_read_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (control == XYZM_ACK) {
      return XYZM_STATUS_OK;
    }
    if (control == XYZM_NAK) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (control == XYZM_CAN) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
    xyzm_report_retry(runtime);
  }
  return XYZM_STATUS_TIMEOUT;
}

/**
 * @brief Performs the two-step EOT handshake used by XMODEM-family senders.
 *
 * @param runtime Active transfer state.
 * @return Final status for the end-of-transfer handshake.
 * @retval XYZM_STATUS_OK The peer acknowledged the transfer end.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted before the handshake completed.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_send_eot_sequence(xyzm_runtime_t *runtime) {
  for (unsigned attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    xyzm_status_t status = xyzm_send_byte(runtime, XYZM_EOT, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    uint8_t control;
    status = xyzm_read_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (control == XYZM_ACK) {
      return XYZM_STATUS_OK;
    }
    if (control != XYZM_NAK) {
      if (control == XYZM_CAN) {
        return XYZM_STATUS_REMOTE_CANCELLED;
      }
      xyzm_report_retry(runtime);
      continue;
    }

    status = xyzm_send_byte(runtime, XYZM_EOT, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_read_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (control == XYZM_ACK) {
      return XYZM_STATUS_OK;
    }
    if (control == XYZM_CAN) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
    xyzm_report_retry(runtime);
  }

  return XYZM_STATUS_TIMEOUT;
}

/**
 * @brief Reads source bytes until the next payload buffer is full or EOF is reached.
 *
 * @param source Send-side file callbacks.
 * @param file Active file state.
 * @param payload Destination payload buffer.
 * @param payload_len Capacity of @p payload in bytes.
 * @param actual_len Output byte count copied into @p payload.
 * @return Status returned by the source callbacks.
 * @retval XYZM_STATUS_OK Zero or more bytes were read successfully.
 * @retval XYZM_STATUS_IO_ERROR The source reported invalid zero-byte progress.
 */
static inline xyzm_status_t xyzm_fill_block(
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    uint8_t *payload,
    size_t payload_len,
    size_t *actual_len) {
  size_t filled_len = 0u;
  while (filled_len < payload_len) {
    size_t read_len = 0u;
    xyzm_status_t status = source->read(
        source->opaque,
        file->file_opaque,
        payload + filled_len,
        payload_len - filled_len,
        &read_len);
    if (status == XYZM_STATUS_END) {
      break;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (read_len == 0u) {
      return XYZM_STATUS_IO_ERROR;
    }
    filled_len += read_len;
  }

  *actual_len = filled_len;
  return XYZM_STATUS_OK;
}

/**
 * @brief Parses a received frame and validates its block number and integrity trailer.
 *
 * @param runtime Active transfer state.
 * @param header Output frame start byte.
 * @param block_number Output block number extracted from the frame.
 * @param payload Destination payload buffer.
 * @param payload_len Output payload byte count.
 * @param use_crc Whether the frame is expected to use a CRC16 trailer.
 * @return Status for the received frame.
 * @retval XYZM_STATUS_OK The frame was parsed successfully.
 * @retval XYZM_STATUS_PROTOCOL_ERROR The frame layout or integrity trailer was invalid.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_read_frame(
    xyzm_runtime_t *runtime,
    uint8_t *header,
    uint8_t *block_number,
    uint8_t *payload,
    size_t *payload_len,
    bool use_crc) {
  *block_number = 0u;
  *payload_len = 0u;
  xyzm_status_t status = xyzm_read_control(runtime, header, runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if ((*header == XYZM_EOT) || (*header == XYZM_CAN)) {
    return XYZM_STATUS_OK;
  }
  if ((*header != XYZM_SOH) && (*header != XYZM_STX)) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  size_t data_len = (*header == XYZM_SOH) ? 128u : 1024u;
  uint8_t block_header[2];
  status = xyzm_recv_exact(runtime, block_header, sizeof(block_header), runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  *block_number = block_header[0];
  if ((uint8_t)(block_header[0] + block_header[1]) != 0xffu) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  status = xyzm_recv_exact(runtime, payload, data_len, runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if (use_crc) {
    uint8_t crc_bytes[2];
    uint16_t expected_crc;

    status = xyzm_recv_exact(runtime, crc_bytes, sizeof(crc_bytes), runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    expected_crc = xyzm_crc16_xmodem_update_buffer(0u, payload, data_len);
    if ((crc_bytes[0] != (uint8_t)(expected_crc >> 8)) ||
        (crc_bytes[1] != (uint8_t)(expected_crc & 0xffu))) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
  } else {
    uint8_t checksum;

    status = xyzm_recv_exact(runtime, &checksum, 1u, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (checksum != xyzm_checksum8_update_buffer(0u, payload, data_len)) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
  }

  *payload_len = data_len;
  return XYZM_STATUS_OK;
}

xyzm_status_t xyzm_xmodem_send_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file) {
  xyzm_xmodem_mode_t mode;
  xyzm_status_t status = xyzm_get_xmodem_send_mode(runtime, &mode);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  status = xyzm_wait_xmodem_start(runtime, &mode);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  uint8_t block_number = 1u;
  while (true) {
    uint8_t payload[1024];
    size_t frame_len;
    size_t actual_len;

    frame_len = mode.block_len;
    memset(payload, runtime->options->pad_byte, frame_len);
    status = xyzm_fill_block(source, file, payload, frame_len, &actual_len);
    if (status != XYZM_STATUS_OK) {
      xyzm_send_cancel(runtime);
      return status;
    }
    if (actual_len == 0u) {
      break;
    }
    if ((mode.block_len == 1024u) && (actual_len <= 128u)) {
      frame_len = 128u;
    }

    status = xyzm_send_block_with_retry(
        runtime,
        (frame_len == 1024u) ? XYZM_STX : XYZM_SOH,
        block_number,
        payload,
        frame_len,
        mode.use_crc);
    if (status != XYZM_STATUS_OK) {
      xyzm_send_cancel(runtime);
      return status;
    }
    xyzm_report_payload(runtime, actual_len, &file->info);
    block_number = (uint8_t)(block_number + 1u);
  }

  status = xyzm_send_eot_sequence(runtime);
  if (status == XYZM_STATUS_OK) {
    xyzm_report_completed_files(runtime, 1u);
  }
  return status;
}

xyzm_status_t xyzm_xmodem_receive_impl(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink) {
  xyzm_receive_file_t file;
  memset(&file, 0, sizeof(file));
  memset(&file.info, 0, sizeof(file.info));
  xyzm_xmodem_mode_t mode;
  uint64_t resume_offset = 0u;
  xyzm_status_t status = xyzm_get_xmodem_receive_mode(runtime, &mode);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = sink->begin(sink->opaque, &file.info, &resume_offset, &file.file_opaque);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if (resume_offset != 0u) {
    return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_UNSUPPORTED);
  }
  xyzm_notify_file_started(runtime, &file.info);

  uint8_t expected_block = 1u;
  unsigned retry_count = 0u;
  bool started = false;
  status = xyzm_send_byte(runtime, mode.start_char, runtime->options->handshake_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return xyzm_finish_receive_file(runtime, sink, &file, status);
  }

  for (;;) {
    uint8_t header;
    uint8_t block_number;
    uint8_t payload[1024];
    size_t payload_len;

    status = xyzm_read_frame(
        runtime,
        &header,
        &block_number,
        payload,
        &payload_len,
        mode.use_crc);
    if ((status == XYZM_STATUS_TIMEOUT) || (status == XYZM_STATUS_PROTOCOL_ERROR)) {
      if (retry_count >= runtime->options->retry_limit) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      retry_count += 1u;
      xyzm_report_retry(runtime);
      (void)xyzm_send_byte(runtime, mode.start_char, runtime->options->block_timeout_ms);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }

    if (header == XYZM_EOT) {
      uint8_t second_eot;

      status = xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      status = xyzm_read_control(runtime, &second_eot, runtime->options->block_timeout_ms);
      if ((status != XYZM_STATUS_OK) || (second_eot != XYZM_EOT)) {
        status = (status == XYZM_STATUS_OK) ? XYZM_STATUS_PROTOCOL_ERROR : status;
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      xyzm_report_completed_files(runtime, started ? 1u : 0u);
      return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_OK);
    }

    if (block_number == expected_block) {
      started = true;
      retry_count = 0u;
      status = sink->write(sink->opaque, file.file_opaque, payload, payload_len);
      if (status != XYZM_STATUS_OK) {
        xyzm_send_cancel(runtime);
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      xyzm_report_payload(runtime, payload_len, &file.info);
      status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      expected_block = (uint8_t)(expected_block + 1u);
      continue;
    }

    if (block_number == (uint8_t)(expected_block - 1u)) {
      status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      continue;
    }

    if (retry_count >= runtime->options->retry_limit) {
      return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
    }
    retry_count += 1u;
    xyzm_report_retry(runtime);
    status = xyzm_send_byte(runtime, mode.start_char, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }
  }
}
