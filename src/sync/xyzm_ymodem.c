/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_sync_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Reads a control byte and folds a CAN CAN sequence into remote cancellation.
 *
 * @param runtime Active transfer state.
 * @param value Output control byte.
 * @param timeout_ms Maximum time to wait for the first control byte.
 * @return Completion status for the control read.
 * @retval XYZM_STATUS_OK A control byte was read successfully.
 * @retval XYZM_STATUS_TIMEOUT No byte arrived before the timeout expired.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer sent `CAN CAN`.
 */
static inline xyzm_status_t xyzm_wait_control(
    xyzm_runtime_t *runtime,
    uint8_t *value,
    uint32_t timeout_ms) {
  xyzm_status_t status;

  status = xyzm_recv_byte(runtime, value, timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if (*value == XYZM_CAN) {
    uint8_t second;

    status = xyzm_recv_byte(runtime, &second, runtime->options->block_timeout_ms);
    if ((status == XYZM_STATUS_OK) && (second == XYZM_CAN)) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
  }
  return XYZM_STATUS_OK;
}

/**
 * @brief Waits for a specific handshake byte while counting retryable timeouts.
 *
 * @param runtime Active transfer state.
 * @param expected Handshake byte expected from the peer.
 * @return Final handshake status.
 * @retval XYZM_STATUS_OK The expected byte was observed.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted without the expected byte.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_wait_for_byte(
    xyzm_runtime_t *runtime,
    uint8_t expected) {
  unsigned attempt;

  for (attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t control;
    xyzm_status_t status;

    status = xyzm_wait_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (control == expected) {
      return XYZM_STATUS_OK;
    }
    if ((control == XYZM_ACK) && (expected == XYZM_G_REQ)) {
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
 * @brief Waits for the initial YMODEM receiver request and resolves auto mode.
 *
 * @param runtime Active transfer state.
 * @param streaming Output send mode selected for the rest of the transfer.
 * @return Final handshake status.
 * @retval XYZM_STATUS_OK The receiver request was accepted successfully.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted without a valid request.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 * @retval XYZM_STATUS_UNSUPPORTED The configured YMODEM variant is invalid.
 */
static inline xyzm_status_t xyzm_wait_for_ymodem_request(
    xyzm_runtime_t *runtime,
    bool *streaming) {
  unsigned attempt;

  switch (runtime->options->ymodem_variant) {
    case XYZM_YMODEM_VARIANT_STANDARD:
      *streaming = false;
      return xyzm_wait_for_byte(runtime, XYZM_CRC_REQ);
    case XYZM_YMODEM_VARIANT_G:
      *streaming = true;
      return xyzm_wait_for_byte(runtime, XYZM_G_REQ);
    case XYZM_YMODEM_VARIANT_AUTO:
      break;
    default:
      return XYZM_STATUS_UNSUPPORTED;
  }

  for (attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t control;
    xyzm_status_t status;

    status = xyzm_wait_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (control == XYZM_CRC_REQ) {
      *streaming = false;
      return XYZM_STATUS_OK;
    }
    if (control == XYZM_G_REQ) {
      *streaming = true;
      return XYZM_STATUS_OK;
    }
    if (control == XYZM_CAN) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
    xyzm_report_retry(runtime);
  }
  return XYZM_STATUS_TIMEOUT;
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
static inline xyzm_status_t xyzm_recv_ymodem_frame_header(
    xyzm_runtime_t *runtime,
    uint8_t *header) {
  unsigned attempt;

  for (attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    xyzm_status_t status;

    status = xyzm_wait_control(runtime, header, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      return status;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (*header == XYZM_SOH || *header == XYZM_STX || *header == XYZM_EOT) {
      return XYZM_STATUS_OK;
    }
    if (*header == XYZM_CAN) {
      return XYZM_STATUS_REMOTE_CANCELLED;
    }
    if (xyzm_is_ymodem_request_echo(*header)) {
      xyzm_report_retry(runtime);
      continue;
    }
    return XYZM_STATUS_PROTOCOL_ERROR;
  }
  return XYZM_STATUS_TIMEOUT;
}

/**
 * @brief Serializes YMODEM filename and size metadata into the initial 128-byte block.
 *
 * @param info File metadata to serialize.
 * @param payload Output 128-byte metadata block.
 */
static inline void xyzm_make_ymodem_header(
    const xyzm_file_info_t *info,
    uint8_t payload[128]) {
  size_t offset;

  memset(payload, 0, 128u);
  offset = 0u;
  if (((info->valid_mask & XYZM_FILE_INFO_NAME) != 0u) && (info->name != NULL)) {
    size_t name_len;

    name_len = strlen(info->name);
    if (name_len > 126u) {
      name_len = 126u;
    }
    memcpy(payload, info->name, name_len);
    offset = name_len + 1u;
  } else {
    payload[0] = '\0';
    return;
  }

  if (((info->valid_mask & XYZM_FILE_INFO_SIZE) != 0u) && (offset < 127u)) {
    char size_text[32];
    int written_len;

    written_len = snprintf(size_text, sizeof(size_text), "%llu",
        (unsigned long long)info->size_bytes);
    if (written_len > 0) {
      size_t copy_len;

      copy_len = (size_t)written_len;
      if (copy_len > (127u - offset)) {
        copy_len = 127u - offset;
      }
      memcpy(payload + offset, size_text, copy_len);
    }
  }
}

/**
 * @brief Parses the metadata header and detects the terminal empty header block.
 *
 * @param file Output receive-side file state populated from the metadata.
 * @param payload Raw 128-byte metadata block.
 * @param is_terminal_header Output flag set when the header announces transfer end.
 * @return `true` when the metadata block is structurally valid.
 * @return `false` when the metadata block cannot be represented locally.
 */
static inline bool xyzm_parse_ymodem_header(
    xyzm_receive_file_t *file,
    const uint8_t payload[128],
    bool *is_terminal_header) {
  const char *size_text;
  size_t name_len;
  size_t index;

  *is_terminal_header = (payload[0] == '\0');
  memset(&file->info, 0, sizeof(file->info));
  memset(file->name_storage, 0, sizeof(file->name_storage));
  if (*is_terminal_header) {
    return true;
  }

  name_len = 0u;
  for (index = 0u; index < 128u; ++index) {
    if (payload[index] == '\0') {
      break;
    }
    ++name_len;
  }
  if ((name_len == 0u) || (name_len >= sizeof(file->name_storage))) {
    return false;
  }
  memcpy(file->name_storage, payload, name_len);
  file->name_storage[name_len] = '\0';
  file->info.valid_mask = XYZM_FILE_INFO_NAME;
  file->info.name = file->name_storage;

  size_text = (const char *)(payload + name_len + 1u);
  if ((name_len + 1u) < 128u && (*size_text != '\0')) {
    char *end_ptr;
    unsigned long long value;

    value = strtoull(size_text, &end_ptr, 10);
    if ((end_ptr != size_text) && ((*end_ptr == '\0') || (*end_ptr == ' '))) {
      file->info.valid_mask |= XYZM_FILE_INFO_SIZE;
      file->info.size_bytes = (uint64_t)value;
    }
  }
  return true;
}

/**
 * @brief Sends one CRC-protected YMODEM block and optionally waits for the ACK path.
 *
 * @param runtime Active transfer state.
 * @param header Frame start byte such as @ref XYZM_SOH or @ref XYZM_STX.
 * @param block_number Sequential block number to encode.
 * @param payload Block payload bytes.
 * @param payload_len Number of payload bytes to encode.
 * @param wait_for_ack Whether the helper should wait for the receiver acknowledgement.
 * @param retry_ack_failures Whether ACK timeouts or non-ACK controls should resend the frame.
 * @return Final status for the block exchange.
 * @retval XYZM_STATUS_OK The block was sent successfully.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted without a valid acknowledgement.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_send_crc_block(
    xyzm_runtime_t *runtime,
    uint8_t header,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool wait_for_ack,
    bool retry_ack_failures) {
  uint8_t frame[3u + 1024u + 2u];
  size_t frame_len;
  uint16_t crc;
  unsigned attempt;

  frame[0] = header;
  frame[1] = block_number;
  frame[2] = (uint8_t)(255u - block_number);
  memcpy(frame + 3u, payload, payload_len);
  crc = xyzm_crc16_xmodem_update_buffer(0u, payload, payload_len);
  frame[3u + payload_len] = (uint8_t)(crc >> 8);
  frame[3u + payload_len + 1u] = (uint8_t)(crc & 0xffu);
  frame_len = 3u + payload_len + 2u;

  for (attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t control;
    xyzm_status_t status;

    status = xyzm_send_exact(runtime, frame, frame_len, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (!wait_for_ack) {
      return XYZM_STATUS_OK;
    }

    status = xyzm_wait_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      if (!retry_ack_failures) {
        return XYZM_STATUS_OK;
      }
      xyzm_report_retry(runtime);
      continue;
    }
    if ((status == XYZM_STATUS_IO_ERROR) && !retry_ack_failures) {
      return XYZM_STATUS_OK;
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
    if (!retry_ack_failures) {
      return XYZM_STATUS_OK;
    }
    if (control == XYZM_NAK) {
      xyzm_report_retry(runtime);
      continue;
    }
    xyzm_report_retry(runtime);
  }

  return XYZM_STATUS_TIMEOUT;
}

/**
 * @brief Handles the YMODEM and YMODEM-g EOT shutdown differences behind one helper.
 *
 * @param runtime Active transfer state.
 * @param streaming `true` for YMODEM-g streaming shutdown rules.
 * @return Final status for the end-of-transfer handshake.
 * @retval XYZM_STATUS_OK The peer acknowledged the transfer end.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted before the handshake completed.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_send_ymodem_eot(
    xyzm_runtime_t *runtime,
    bool streaming) {
  unsigned attempt;

  for (attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t control;
    xyzm_status_t status;

    status = xyzm_send_byte(runtime, XYZM_EOT, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_wait_control(runtime, &control, runtime->options->block_timeout_ms);
    if (status == XYZM_STATUS_TIMEOUT) {
      xyzm_report_retry(runtime);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (control == XYZM_ACK) {
      /*
       * Chuck Forsberg's X/YMODEM Protocol Reference (Figure 3, "YMODEM
       * Batch Transmission Session") shows the strict non-streaming shutdown
       * path as EOT, NAK, EOT, ACK. Accept a direct ACK after the first EOT
       * as a deliberate interoperability deviation because lrzsz's sb/rb
       * implementations complete that shorter handshake.
       */
      return XYZM_STATUS_OK;
    }
    if (streaming) {
      if (control == XYZM_CAN) {
        return XYZM_STATUS_REMOTE_CANCELLED;
      }
      xyzm_report_retry(runtime);
      continue;
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
    status = xyzm_wait_for_byte(runtime, XYZM_ACK);
    if (status == XYZM_STATUS_OK) {
      return XYZM_STATUS_OK;
    }
    if (status != XYZM_STATUS_TIMEOUT) {
      return status;
    }
  }
  return XYZM_STATUS_TIMEOUT;
}

static inline xyzm_status_t xyzm_recv_crc_block(
    xyzm_runtime_t *runtime,
    uint8_t *header,
    uint8_t *block_number,
    uint8_t *payload,
    size_t *payload_len);

/**
 * @brief Receives the terminal empty pathname header that closes one YMODEM batch.
 *
 * @param runtime Active transfer state.
 * @param trailer Output receive-side file state populated from the terminal header.
 * @return Final status for the terminal-header fetch.
 * @retval XYZM_STATUS_OK The terminal empty pathname block was parsed successfully.
 * @retval XYZM_STATUS_TIMEOUT Retries were exhausted while waiting for the terminal block.
 * @retval XYZM_STATUS_PROTOCOL_ERROR Retries were exhausted after malformed terminal data.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
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
static inline xyzm_status_t xyzm_recv_ymodem_terminal_header(
    xyzm_runtime_t *runtime,
    xyzm_receive_file_t *trailer,
    bool allow_terminal_loss) {
  unsigned attempt;
  bool saw_timeout;

  saw_timeout = false;
  memset(trailer, 0, sizeof(*trailer));
  for (attempt = 0u; attempt <= runtime->options->retry_limit; ++attempt) {
    uint8_t header = 0u;
    uint8_t block_number = 0u;
    uint8_t payload[1024];
    size_t payload_len = 0u;
    bool is_terminal_header = false;
    xyzm_status_t status;

    status = xyzm_recv_crc_block(runtime, &header, &block_number, payload, &payload_len);
    if (allow_terminal_loss && status != XYZM_STATUS_OK) {
      return status;
    }
    if (status == XYZM_STATUS_OK) {
      if (header == XYZM_CAN) {
        return XYZM_STATUS_REMOTE_CANCELLED;
      }
      if (header == XYZM_EOT) {
        if (allow_terminal_loss) {
          return XYZM_STATUS_TIMEOUT;
        }
        /*
         * This is another deliberate lrzsz-compatibility deviation from the
         * strict reference sequence above: tolerate one more EOT while the
         * sender/receiver pair converges on the terminal pathname block.
         */
        status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return status;
        }
        status = xyzm_send_byte(runtime, XYZM_CRC_REQ, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return status;
        }
      } else if ((header == XYZM_SOH) &&
                 (block_number == 0u) &&
                 xyzm_parse_ymodem_header(trailer, payload, &is_terminal_header) &&
                 is_terminal_header) {
        return XYZM_STATUS_OK;
      }
      if (allow_terminal_loss) {
        return XYZM_STATUS_TIMEOUT;
      }
    } else if (status == XYZM_STATUS_TIMEOUT) {
      if (allow_terminal_loss) {
        return status;
      }
      saw_timeout = true;
      status = xyzm_send_byte(runtime, XYZM_CRC_REQ, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
    } else if (status != XYZM_STATUS_PROTOCOL_ERROR) {
      return status;
    }

    if (attempt == runtime->options->retry_limit) {
      return saw_timeout ? XYZM_STATUS_TIMEOUT : XYZM_STATUS_PROTOCOL_ERROR;
    }
    xyzm_report_retry(runtime);
  }
  return saw_timeout ? XYZM_STATUS_TIMEOUT : XYZM_STATUS_PROTOCOL_ERROR;
}

/**
 * @brief Fills the next YMODEM payload buffer from the source stream.
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
static inline xyzm_status_t xyzm_fill_data(
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    uint8_t *payload,
    size_t payload_len,
    size_t *actual_len) {
  size_t filled_len;

  filled_len = 0u;
  while (filled_len < payload_len) {
    size_t read_len;
    xyzm_status_t status;

    read_len = 0u;
    status = source->read(
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
 * @brief Receives one CRC-backed YMODEM block and validates its framing bytes.
 *
 * @param runtime Active transfer state.
 * @param header Output frame start byte.
 * @param block_number Output block number extracted from the frame.
 * @param payload Destination payload buffer.
 * @param payload_len Output payload byte count.
 * @return Status for the received frame.
 * @retval XYZM_STATUS_OK The frame was parsed successfully.
 * @retval XYZM_STATUS_PROTOCOL_ERROR The frame layout or CRC trailer was invalid.
 * @retval XYZM_STATUS_REMOTE_CANCELLED The peer cancelled the transfer.
 */
static inline xyzm_status_t xyzm_recv_crc_block(
    xyzm_runtime_t *runtime,
    uint8_t *header,
    uint8_t *block_number,
    uint8_t *payload,
    size_t *payload_len) {
  uint8_t block_header[2];
  size_t data_len;
  uint8_t crc_bytes[2];
  uint16_t expected_crc;
  xyzm_status_t status;

  status = xyzm_recv_ymodem_frame_header(runtime, header);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if ((*header == XYZM_EOT) || (*header == XYZM_CAN)) {
    *payload_len = 0u;
    return XYZM_STATUS_OK;
  }
  if ((*header != XYZM_SOH) && (*header != XYZM_STX)) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  data_len = (*header == XYZM_SOH) ? 128u : 1024u;
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
  status = xyzm_recv_exact(runtime, crc_bytes, sizeof(crc_bytes), runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  expected_crc = xyzm_crc16_xmodem_update_buffer(0u, payload, data_len);
  if ((crc_bytes[0] != (uint8_t)(expected_crc >> 8)) ||
      (crc_bytes[1] != (uint8_t)(expected_crc & 0xffu))) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  *payload_len = data_len;
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_recv_initial_ymodem_block(
    xyzm_runtime_t *runtime,
    uint8_t *header,
    uint8_t *block_number,
    uint8_t *payload,
    size_t *payload_len) {
  uint8_t block_header[2];
  size_t data_len;
  uint8_t crc_bytes[2];
  uint16_t expected_crc;
  xyzm_status_t status;

  status = xyzm_recv_ymodem_frame_header(runtime, header);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if ((*header == XYZM_EOT) || (*header == XYZM_CAN)) {
    *payload_len = 0u;
    return XYZM_STATUS_OK;
  }
  if ((*header != XYZM_SOH) && (*header != XYZM_STX)) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  data_len = (*header == XYZM_SOH) ? 128u : 1024u;
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
  status = xyzm_recv_exact(runtime, crc_bytes, sizeof(crc_bytes), runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  expected_crc = xyzm_crc16_xmodem_update_buffer(0u, payload, data_len);
  if ((crc_bytes[0] != (uint8_t)(expected_crc >> 8)) ||
      (crc_bytes[1] != (uint8_t)(expected_crc & 0xffu))) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  *payload_len = data_len;
  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_send_ymodem_file(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    bool streaming) {
  uint8_t metadata_payload[128];
  uint8_t block_number;
  xyzm_status_t status;

  xyzm_make_ymodem_header(&file->info, metadata_payload);
  status = xyzm_send_crc_block(
      runtime,
      XYZM_SOH,
      0u,
      metadata_payload,
      sizeof(metadata_payload),
      !streaming,
      true);
  if (status != XYZM_STATUS_OK) {
    xyzm_send_cancel(runtime);
    return status;
  }

  status = xyzm_wait_for_byte(runtime, streaming ? XYZM_G_REQ : XYZM_CRC_REQ);
  if (status != XYZM_STATUS_OK) {
    xyzm_send_cancel(runtime);
    return status;
  }

  block_number = 1u;
  for (;;) {
    uint8_t payload[1024];
    size_t actual_len;

    memset(payload, runtime->options->pad_byte, sizeof(payload));
    status = xyzm_fill_data(source, file, payload, sizeof(payload), &actual_len);
    if (status != XYZM_STATUS_OK) {
      xyzm_send_cancel(runtime);
      return status;
    }
    if (actual_len == 0u) {
      break;
    }

    status = xyzm_send_crc_block(
        runtime,
        XYZM_STX,
        block_number,
        payload,
        sizeof(payload),
        !streaming,
        true);
    if (status != XYZM_STATUS_OK) {
      xyzm_send_cancel(runtime);
      return status;
    }
    xyzm_report_payload(runtime, actual_len, &file->info);
    block_number = (uint8_t)(block_number + 1u);
  }

  status = xyzm_send_ymodem_eot(runtime, streaming);
  if (status != XYZM_STATUS_OK) {
    xyzm_send_cancel(runtime);
    return status;
  }

  status = xyzm_wait_for_byte(runtime, streaming ? XYZM_G_REQ : XYZM_CRC_REQ);
  if (status != XYZM_STATUS_OK) {
    xyzm_send_cancel(runtime);
    return status;
  }

  return XYZM_STATUS_OK;
}

static inline xyzm_status_t xyzm_send_ymodem_terminal_header(
    xyzm_runtime_t *runtime,
    bool streaming) {
  uint8_t metadata_payload[128];

  memset(metadata_payload, 0, sizeof(metadata_payload));
  /*
   * The terminal empty pathname block is not a file-content block; it only
   * closes the YMODEM batch after the payload has already been acknowledged
   * through the EOT handshake.
   *
   * lrzsz's rb acknowledges this empty block and then immediately leaves raw
   * mode. Its io_mode(0, 0) path calls tcdrain() and then tcflush(TCIOFLUSH).
   * fflush(stdout) before that point only moves the ACK from stdio into the
   * tty/pty side; it does not prove that the remote sender has already read it.
   * On a pty/telnet path, the ACK still has to travel through the pty master,
   * telnetd, TCP, and the local event loop. The following TCIOFLUSH can race
   * with that propagation and discard or hide the final one-byte ACK from the
   * sender.
   *
   * lrzsz's sb normally masks this race because it is already blocked in the
   * ACK read when rb writes the byte, and its caller does not strongly surface
   * a failure of the final empty header after the file payload is complete.
   * A library sender that retries the same empty block after rb has exited can
   * instead leak the SOH 00 FF terminal header into the user's shell.
   *
   * Keep the normal retry policy for metadata and data blocks, but for this
   * terminal header wait for one ACK opportunity only. If the frame itself was
   * written, a timeout, peer close, NAK, or stray receiver request is treated as
   * successful batch closure. CAN remains a remote cancellation.
   */
  return xyzm_send_crc_block(
      runtime,
      XYZM_SOH,
      0u,
      metadata_payload,
      sizeof(metadata_payload),
      !streaming,
      false);
}

xyzm_status_t xyzm_ymodem_send_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file) {
  bool streaming = false;
  xyzm_status_t status;

  status = xyzm_wait_for_ymodem_request(runtime, &streaming);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_send_ymodem_file(runtime, source, file, streaming);
  if (status == XYZM_STATUS_OK) {
    status = xyzm_send_ymodem_terminal_header(runtime, streaming);
  }
  if (status == XYZM_STATUS_OK) {
    xyzm_report_completed_file(runtime);
  }
  return status;
}

xyzm_status_t xyzm_ymodem_send_batch_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file) {
  bool streaming = false;
  xyzm_status_t status;

  status = xyzm_wait_for_ymodem_request(runtime, &streaming);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  for (;;) {
    status = xyzm_send_ymodem_file(runtime, source, file, streaming);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    xyzm_report_completed_file(runtime);
    status = xyzm_finish_send_file(runtime, source, file, XYZM_STATUS_OK);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    status = xyzm_send_single_file(runtime, source, file);
    if (status == XYZM_STATUS_END) {
      return xyzm_send_ymodem_terminal_header(runtime, streaming);
    }
    if (status != XYZM_STATUS_OK) {
      return status;
    }
  }
}

static inline xyzm_status_t xyzm_ymodem_receive_batch_loop(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink) {
  xyzm_receive_file_t file;
  uint8_t header = 0u;
  uint8_t block_number = 0u;
  uint8_t payload[1024];
  size_t payload_len;
  uint8_t expected_block;
  bool is_terminal_header;
  bool streaming;
  uint64_t remaining_bytes;
  uint64_t resume_offset;
  xyzm_status_t status;
  uint32_t completed_files;

  memset(&file, 0, sizeof(file));
  switch (runtime->options->ymodem_variant) {
    case XYZM_YMODEM_VARIANT_STANDARD:
      streaming = false;
      break;
    case XYZM_YMODEM_VARIANT_G:
      streaming = true;
      break;
    case XYZM_YMODEM_VARIANT_AUTO:
    default:
      return XYZM_STATUS_UNSUPPORTED;
  }

  status = xyzm_send_byte(
      runtime,
      streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
      runtime->options->handshake_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  completed_files = 0u;
  for (;;) {
    memset(&file, 0, sizeof(file));
    status = xyzm_recv_initial_ymodem_block(runtime, &header, &block_number, payload, &payload_len);
    if (status != XYZM_STATUS_OK) {
      if (streaming && completed_files > 0u &&
          (status == XYZM_STATUS_TIMEOUT ||
           status == XYZM_STATUS_IO_ERROR ||
           status == XYZM_STATUS_PROTOCOL_ERROR)) {
        return XYZM_STATUS_OK;
      }
      return status;
    }
    if ((header != XYZM_SOH) || (block_number != 0u)) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
    if (!xyzm_parse_ymodem_header(&file, payload, &is_terminal_header)) {
      return XYZM_STATUS_PROTOCOL_ERROR;
    }
    if (is_terminal_header) {
      status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return status;
      }
      return XYZM_STATUS_OK;
    }

    resume_offset = 0u;
    status = sink->begin(sink->opaque, &file.info, &resume_offset, &file.file_opaque);
    if (status != XYZM_STATUS_OK) {
      xyzm_send_cancel(runtime);
      return status;
    }
    if (resume_offset != 0u) {
      xyzm_send_cancel(runtime);
      return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_UNSUPPORTED);
    }
    xyzm_notify_file_started(runtime, &file.info);

    if (!streaming) {
      status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
    }
    status = xyzm_send_byte(
        runtime,
        streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
        runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }

    expected_block = 1u;
    remaining_bytes =
        ((file.info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) ? file.info.size_bytes : UINT64_MAX;
    for (;;) {
      status = xyzm_recv_crc_block(runtime, &header, &block_number, payload, &payload_len);
      if (status == XYZM_STATUS_PROTOCOL_ERROR) {
        if (streaming) {
          xyzm_send_cancel(runtime);
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
        xyzm_report_retry(runtime);
        (void)xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
        continue;
      }
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }

      if (header == XYZM_EOT) {
        if ((remaining_bytes != UINT64_MAX) && (remaining_bytes != 0u)) {
          return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
        }
        if (streaming) {
          status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
        } else {
          uint8_t second_eot;

          status = xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
          status = xyzm_wait_control(runtime, &second_eot, runtime->options->block_timeout_ms);
          if ((status != XYZM_STATUS_OK) || (second_eot != XYZM_EOT)) {
            status = (status == XYZM_STATUS_OK) ? XYZM_STATUS_PROTOCOL_ERROR : status;
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
          status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
        }
        status = xyzm_send_byte(
            runtime,
            streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
            runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
        status = xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_OK);
        if (status != XYZM_STATUS_OK) {
          return status;
        }
        xyzm_report_completed_file(runtime);
        completed_files += 1u;
        break;
      }

      if (block_number == expected_block) {
        size_t write_len;

        if (remaining_bytes == 0u) {
          return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
        }
        write_len = payload_len;
        if ((remaining_bytes != UINT64_MAX) && (remaining_bytes < write_len)) {
          write_len = (size_t)remaining_bytes;
        }
        if (write_len > 0u) {
          status = sink->write(sink->opaque, file.file_opaque, payload, write_len);
          if (status != XYZM_STATUS_OK) {
            xyzm_send_cancel(runtime);
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
          xyzm_report_payload(runtime, write_len, &file.info);
          if (remaining_bytes != UINT64_MAX) {
            remaining_bytes -= write_len;
          }
        }
        if (!streaming) {
          status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
        }
        expected_block = (uint8_t)(expected_block + 1u);
        continue;
      }

      if (block_number == (uint8_t)(expected_block - 1u)) {
        if (!streaming) {
          status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
          if (status != XYZM_STATUS_OK) {
            return xyzm_finish_receive_file(runtime, sink, &file, status);
          }
        }
        continue;
      }

      if (streaming) {
        xyzm_send_cancel(runtime);
        return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
      }
      xyzm_report_retry(runtime);
      status = xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
    }
  }
}

xyzm_status_t xyzm_ymodem_receive_impl(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink) {
  xyzm_receive_file_t file;
  xyzm_receive_file_t trailer;
  uint8_t header = 0u;
  uint8_t block_number = 0u;
  uint8_t payload[1024];
  size_t payload_len;
  uint8_t expected_block;
  bool is_terminal_header;
  bool streaming;
  uint64_t remaining_bytes;
  uint64_t resume_offset;
  xyzm_status_t status;

  if (runtime->options->batch_mode) {
    return xyzm_ymodem_receive_batch_loop(runtime, sink);
  }

  memset(&file, 0, sizeof(file));
  memset(&trailer, 0, sizeof(trailer));
  switch (runtime->options->ymodem_variant) {
    case XYZM_YMODEM_VARIANT_STANDARD:
      streaming = false;
      break;
    case XYZM_YMODEM_VARIANT_G:
      streaming = true;
      break;
    case XYZM_YMODEM_VARIANT_AUTO:
    default:
      return XYZM_STATUS_UNSUPPORTED;
  }

  status = xyzm_send_byte(
      runtime,
      streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
      runtime->options->handshake_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  status = xyzm_recv_initial_ymodem_block(runtime, &header, &block_number, payload, &payload_len);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  if ((header != XYZM_SOH) || (block_number != 0u)) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }
  if (!xyzm_parse_ymodem_header(&file, payload, &is_terminal_header) || is_terminal_header) {
    return XYZM_STATUS_PROTOCOL_ERROR;
  }

  resume_offset = 0u;
  status = sink->begin(sink->opaque, &file.info, &resume_offset, &file.file_opaque);
  if (status != XYZM_STATUS_OK) {
    xyzm_send_cancel(runtime);
    return status;
  }
  if (resume_offset != 0u) {
    xyzm_send_cancel(runtime);
    return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_UNSUPPORTED);
  }
  xyzm_notify_file_started(runtime, &file.info);

  if (!streaming) {
    status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }
  }
  status = xyzm_send_byte(
      runtime,
      streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
      runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    return xyzm_finish_receive_file(runtime, sink, &file, status);
  }

  expected_block = 1u;
  remaining_bytes =
      ((file.info.valid_mask & XYZM_FILE_INFO_SIZE) != 0u) ? file.info.size_bytes : UINT64_MAX;
  for (;;) {
    status = xyzm_recv_crc_block(runtime, &header, &block_number, payload, &payload_len);
    if (status == XYZM_STATUS_PROTOCOL_ERROR) {
      if (streaming) {
        xyzm_send_cancel(runtime);
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      xyzm_report_retry(runtime);
      (void)xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
      continue;
    }
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }

    if (header == XYZM_EOT) {
      if ((remaining_bytes != UINT64_MAX) && (remaining_bytes != 0u)) {
        return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
      }
      if (streaming) {
        status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
      } else {
        uint8_t second_eot;

        status = xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
        status = xyzm_wait_control(runtime, &second_eot, runtime->options->block_timeout_ms);
        if ((status != XYZM_STATUS_OK) || (second_eot != XYZM_EOT)) {
          status = (status == XYZM_STATUS_OK) ? XYZM_STATUS_PROTOCOL_ERROR : status;
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
        status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
      }
      status = xyzm_send_byte(
          runtime,
          streaming ? XYZM_G_REQ : XYZM_CRC_REQ,
          runtime->options->block_timeout_ms);
      if (status != XYZM_STATUS_OK) {
        return xyzm_finish_receive_file(runtime, sink, &file, status);
      }
      break;
    }

    if (block_number == expected_block) {
      size_t write_len;

      if (remaining_bytes == 0u) {
        return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
      }
      write_len = payload_len;
      if ((remaining_bytes != UINT64_MAX) && (remaining_bytes < write_len)) {
        write_len = (size_t)remaining_bytes;
      }
      if (write_len > 0u) {
        status = sink->write(sink->opaque, file.file_opaque, payload, write_len);
        if (status != XYZM_STATUS_OK) {
          xyzm_send_cancel(runtime);
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
        xyzm_report_payload(runtime, write_len, &file.info);
        if (remaining_bytes != UINT64_MAX) {
          remaining_bytes -= write_len;
        }
      }
      if (!streaming) {
        status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
      }
      expected_block = (uint8_t)(expected_block + 1u);
      continue;
    }

    if (block_number == (uint8_t)(expected_block - 1u)) {
      if (!streaming) {
        status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
        if (status != XYZM_STATUS_OK) {
          return xyzm_finish_receive_file(runtime, sink, &file, status);
        }
      }
      continue;
    }

    if (streaming) {
      xyzm_send_cancel(runtime);
      return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_PROTOCOL_ERROR);
    }
    xyzm_report_retry(runtime);
    status = xyzm_send_byte(runtime, XYZM_NAK, runtime->options->block_timeout_ms);
    if (status != XYZM_STATUS_OK) {
      return xyzm_finish_receive_file(runtime, sink, &file, status);
    }
  }

  status = xyzm_recv_ymodem_terminal_header(
      runtime,
      &trailer,
      streaming);
  if (status != XYZM_STATUS_OK) {
    if (streaming &&
        (status == XYZM_STATUS_TIMEOUT ||
         status == XYZM_STATUS_IO_ERROR ||
         status == XYZM_STATUS_PROTOCOL_ERROR)) {
      /*
       * This is not specified by the X/YMODEM/YMODEM-g protocol. lrzsz's `sb`
       * keeps its no-ACK `Optiong` behavior even for the terminal empty
       * pathname block, so a tty/pty/ssh/telnet path can close after the EOT
       * ACK with the file payload complete but before the terminal block is
       * observed locally. Treat that post-EOT close/timeout as a successful
       * lrzsz YMODEM-g single-file completion instead of failing an already
       * durable payload.
       */
      xyzm_report_completed_file(runtime);
      return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_OK);
    }
    return xyzm_finish_receive_file(runtime, sink, &file, status);
  }
  status = xyzm_send_byte(runtime, XYZM_ACK, runtime->options->block_timeout_ms);
  if (status != XYZM_STATUS_OK) {
    if (streaming &&
        (status == XYZM_STATUS_TIMEOUT ||
         status == XYZM_STATUS_IO_ERROR)) {
      /*
       * lrzsz can exit immediately after writing the terminal header on a
       * YMODEM-g single-file transfer. At this point the payload and terminal
       * header have both been consumed, so a closed peer during the final ACK is
       * a successful completion rather than a transfer failure.
       */
      xyzm_report_completed_file(runtime);
      return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_OK);
    }
    return xyzm_finish_receive_file(runtime, sink, &file, status);
  }
  xyzm_report_completed_file(runtime);
  return xyzm_finish_receive_file(runtime, sink, &file, XYZM_STATUS_OK);
}
