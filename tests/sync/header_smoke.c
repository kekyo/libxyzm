/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "libxyzm/sync.h"
#include "libxyzm/xyzm.h"

static xyzm_status_t noop_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  (void)opaque;
  (void)buf;
  (void)timeout_ms;
  *written_len = len;
  return len == 0 ? XYZM_STATUS_ARGUMENT_ERROR : XYZM_STATUS_OK;
}

static xyzm_status_t noop_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  (void)opaque;
  (void)buf;
  (void)timeout_ms;
  *read_len = capacity == 0 ? 0u : 1u;
  return capacity == 0 ? XYZM_STATUS_ARGUMENT_ERROR : XYZM_STATUS_OK;
}

static uint64_t noop_now_ms(void *opaque) {
  (void)opaque;
  return 0u;
}

static xyzm_status_t noop_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  (void)opaque;
  info->valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  info->name = "example.bin";
  info->size_bytes = 1u;
  info->mtime_unix_seconds = 0u;
  info->mode = 0u;
  *file_opaque = info;
  return XYZM_STATUS_OK;
}

static xyzm_status_t noop_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  (void)opaque;
  (void)file_opaque;
  if (capacity == 0u) {
    *read_len = 0u;
    return XYZM_STATUS_ARGUMENT_ERROR;
  }
  buf[0] = 0u;
  *read_len = 1u;
  return XYZM_STATUS_OK;
}

static xyzm_status_t noop_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  (void)opaque;
  (void)file_opaque;
  (void)info;
  (void)result;
  return XYZM_STATUS_OK;
}

static xyzm_status_t noop_sink_begin(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque) {
  (void)opaque;
  (void)info;
  if (resume_offset != 0) {
    *resume_offset = 0u;
  }
  *file_opaque = (void *)info;
  return XYZM_STATUS_OK;
}

static xyzm_status_t noop_sink_write(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len) {
  (void)opaque;
  (void)file_opaque;
  (void)buf;
  return len == 0u ? XYZM_STATUS_ARGUMENT_ERROR : XYZM_STATUS_OK;
}

static xyzm_status_t noop_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  (void)opaque;
  (void)file_opaque;
  (void)info;
  (void)result;
  return XYZM_STATUS_OK;
}

int main(void) {
  xyzm_link_ops_t link = {
      .opaque = 0,
      .send = noop_send,
      .recv = noop_recv,
      .now_ms = noop_now_ms,
  };
  xyzm_source_ops_t source = {
      .opaque = 0,
      .next = noop_source_next,
      .read = noop_source_read,
      .end = noop_source_end,
  };
  xyzm_sink_ops_t sink = {
      .opaque = 0,
      .begin = noop_sink_begin,
      .write = noop_sink_write,
      .end = noop_sink_end,
  };
  xyzm_xmodem_send_opts_t xmodem_send_options = {
      .handshake_timeout_ms = 1000u,
      .block_timeout_ms = 1000u,
      .retry_limit = 10u,
      .pad_byte = 0x1au,
      .checksum_mode = XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM,
      .packet_size = XYZM_XMODEM_PACKET_SIZE_128,
  };
  xyzm_ymodem_opts_t ymodem_options = {
      .handshake_timeout_ms = 1000u,
      .block_timeout_ms = 1000u,
      .retry_limit = 10u,
      .pad_byte = 0x1au,
      .variant = XYZM_YMODEM_VARIANT_STANDARD,
  };
  xyzm_xmodem_send_request_t send_request = {
      .link = &link,
      .source = &source,
      .options = &xmodem_send_options,
      .observer = 0,
  };
  xyzm_ymodem_receive_request_t receive_request = {
      .link = &link,
      .sink = &sink,
      .options = &ymodem_options,
      .observer = 0,
  };
  (void)send_request;
  (void)receive_request;

  return 0;
}
