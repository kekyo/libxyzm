#include "../xyzm_sync_internal.h"

xyzm_status_t xyzm_xmodem_send(
    const xyzm_xmodem_send_request_t *request,
    xyzm_transfer_report_t *report) {
  xyzm_runtime_t runtime;
  xyzm_runtime_options_t options;
  xyzm_send_file_t file;
  xyzm_status_t status;

  xyzm_report_init(report);
  if ((request == NULL) || (request->options == NULL)) {
    if (report != NULL) {
      report->final_status = XYZM_STATUS_ARGUMENT_ERROR;
    }
    return XYZM_STATUS_ARGUMENT_ERROR;
  }
  status = xyzm_validate_sync_send_request(request->link, request->source, report);
  if (status != XYZM_STATUS_OK) {
    return status;
  }

  options.handshake_timeout_ms = request->options->handshake_timeout_ms;
  options.block_timeout_ms = request->options->block_timeout_ms;
  options.retry_limit = request->options->retry_limit;
  options.pad_byte = request->options->pad_byte;
  options.xmodem_checksum_mode = request->options->checksum_mode;
  options.xmodem_packet_size = request->options->packet_size;
  options.ymodem_variant = 0;
  options.zmodem_packet_len = 0u;
  options.zmodem_escape_ctrl = 0u;
  options.zmodem_use_crc32 = 0u;
  options.batch_mode = false;

  runtime.link = request->link;
  runtime.options = &options;
  runtime.report = report;
  runtime.observer = request->observer;
  runtime.protocol = XYZM_TRANSFER_PROTOCOL_XMODEM;
  runtime.direction = XYZM_TRANSFER_DIRECTION_SEND;

  xyzm_notify_transfer_started(&runtime);
  status = xyzm_send_single_file(&runtime, request->source, &file);
  if (status != XYZM_STATUS_OK) {
    return xyzm_finish_transfer(&runtime, status);
  }

  status = xyzm_xmodem_send_impl(&runtime, request->source, &file);
  status = xyzm_finish_send_file(&runtime, request->source, &file, status);
  return xyzm_finish_transfer(&runtime, status);
}
