#include "../xyzm_async_internal.h"

cardio::promise<void> xyzm_xmodem_send_async(
    const xyzm_xmodem_send_async_request_t *request,
    xyzm_async_transfer_report_t *report,
    cardio::cancellation cancellation) {
  xyzm_async_runtime_t runtime;
  xyzm_runtime_options_t options;
  xyzm_async_send_file_t file{};

  xyzm_report_init_async(report);
  if ((request == NULL) || (request->options == NULL)) {
    xyzm_throw_argument_async();
  }
  xyzm_validate_async_send_request(request->link, request->source);

  options.handshake_timeout_ms = request->options->handshake_timeout_ms;
  options.block_timeout_ms = request->options->block_timeout_ms;
  options.retry_limit = request->options->retry_limit;
  options.pad_byte = request->options->pad_byte;
  options.xmodem_checksum_mode = request->options->checksum_mode;
  options.xmodem_packet_size = request->options->packet_size;
  options.ymodem_variant = (xyzm_ymodem_variant_t)0;
  options.zmodem_packet_len = 0u;
  options.zmodem_escape_ctrl = 0u;
  options.zmodem_use_crc32 = 0u;
  options.batch_mode = false;

  runtime.link = request->link;
  runtime.options = &options;
  runtime.report = report;
  runtime.observer = request->observer;
  runtime.cancellation = cancellation;
  runtime.protocol = XYZM_TRANSFER_PROTOCOL_XMODEM;
  runtime.direction = XYZM_TRANSFER_DIRECTION_SEND;

  xyzm_check_cancelled_async(runtime);
  xyzm_notify_transfer_started_async(runtime);
  std::exception_ptr error;
  try {
    if ((co_await xyzm_send_single_file_async(runtime, request->source, &file)) ==
        xyzm_async_status_t::end) {
      xyzm_finish_transfer_success_async(runtime);
      co_return;
    }

    co_await xyzm_xmodem_send_impl_async(runtime, request->source, &file);
    co_await xyzm_finish_send_file_async(runtime, request->source, &file, nullptr);
    xyzm_finish_transfer_success_async(runtime);
    co_return;
  } catch (...) {
    error = xyzm_normalize_error_async(std::current_exception());
  }
  if (file.active) {
    co_await xyzm_finish_send_file_async(runtime, request->source, &file, error);
  }
  xyzm_finish_transfer_failure_async(runtime, error);
  std::rethrow_exception(error);
}
