#include "../xyzm_async_internal.h"

static cardio::promise<void> xyzm_zmodem_receive_common_async(
    const xyzm_zmodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    bool batch_mode,
    cardio::cancellation cancellation) {
  xyzm_async_runtime_t runtime;
  xyzm_runtime_options_t options;

  xyzm_report_init_async(report);
  if ((request == NULL) || (request->options == NULL)) {
    xyzm_throw_argument_async();
  }
  xyzm_validate_async_receive_request(request->link, request->sink);

  options.handshake_timeout_ms = request->options->handshake_timeout_ms;
  options.block_timeout_ms = request->options->block_timeout_ms;
  options.retry_limit = request->options->retry_limit;
  options.pad_byte = request->options->pad_byte;
  options.xmodem_checksum_mode = (xyzm_xmodem_checksum_mode_t)0;
  options.xmodem_packet_size = (xyzm_xmodem_packet_size_t)0;
  options.ymodem_variant = (xyzm_ymodem_variant_t)0;
  options.zmodem_packet_len = request->options->zmodem_packet_len;
  options.zmodem_escape_ctrl = request->options->zmodem_escape_ctrl;
  options.zmodem_use_crc32 = request->options->zmodem_use_crc32;
  options.batch_mode = batch_mode;

  runtime.link = request->link;
  runtime.options = &options;
  runtime.report = report;
  runtime.observer = request->observer;
  runtime.cancellation = cancellation;
  runtime.protocol = XYZM_TRANSFER_PROTOCOL_ZMODEM;
  runtime.direction = XYZM_TRANSFER_DIRECTION_RECEIVE;

  xyzm_check_cancelled_async(runtime);
  xyzm_notify_transfer_started_async(runtime);
  try {
    co_await xyzm_zmodem_receive_impl_async(runtime, request->sink);
    xyzm_finish_transfer_success_async(runtime);
    co_return;
  } catch (...) {
    std::exception_ptr error = xyzm_normalize_error_async(std::current_exception());
    xyzm_finish_transfer_failure_async(runtime, error);
    std::rethrow_exception(error);
  }
}

cardio::promise<void> xyzm_zmodem_receive_async(
    const xyzm_zmodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation) {
  co_await xyzm_zmodem_receive_common_async(request, report, false, cancellation);
  co_return;
}

cardio::promise<void> xyzm_zmodem_receive_batch_async(
    const xyzm_zmodem_receive_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation) {
  co_await xyzm_zmodem_receive_common_async(request, report, true, cancellation);
  co_return;
}
