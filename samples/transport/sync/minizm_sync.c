/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "minizm_sync_transport.h"

#include "minizm_posix.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Prints the command-line usage summary for the sample CLI.
 *
 * @param stream Destination stream for the usage text.
 * @param argv0 Program name shown in the usage lines.
 */
static void print_usage(FILE *stream, const char *argv0) {
  fprintf(stream, "usage:\n");
  fprintf(stream, "  %s send <local-file>\n", argv0);
  fprintf(stream, "  %s recv <output-dir>\n", argv0);
}

/**
 * @brief Populates the transfer options used by every sample invocation.
 *
 * @param options Output option structure initialized for a basic ZMODEM run.
 */
static void fill_default_zmodem_options(xyzm_zmodem_opts_t *options) {
  /* Start from a known-zero state so every field is set explicitly below. */
  memset(options, 0, sizeof(*options));

  /* Keep the sample focused on the most feature-complete single-file path. */
  options->handshake_timeout_ms = 5000u;
  options->block_timeout_ms = 5000u;
  options->retry_limit = 8u;
  options->pad_byte = 0x1au;
  options->zmodem_packet_len = 0u;
  options->zmodem_escape_ctrl = 1u;
  options->zmodem_use_crc32 = 1u;
}

/**
 * @brief Prints a compact transfer result for manual sample runs.
 *
 * @param label Operation label such as `send` or `recv`.
 * @param report Transfer report returned by the library entry point.
 */
static void print_report(const char *label, const xyzm_transfer_report_t *report) {
  fprintf(
      stderr,
      "%s: status=%d payload=%llu files=%u retries=%u\n",
      label,
      (int)report->final_status,
      (unsigned long long)report->payload_bytes,
      report->completed_files,
      report->retry_count);
}

/**
 * @brief Sends one local file through the custom synchronous transport.
 *
 * @param local_path Path of the file that should be transmitted.
 * @return Process exit code for the sample command.
 */
static int run_send(const char *local_path) {
  /* Put the terminal into raw mode only when the sample is attached to a tty. */
  minizm_posix_tty_guard_t tty_guard;
  if (minizm_posix_tty_guard_enable_raw(&tty_guard, STDIN_FILENO) != 0) {
    perror("minizm_posix_tty_guard_enable_raw");
    return 1;
  }

  /* Swap only the transport wiring; keep the default file source helper. */
  xyzm_link_ops_t link;
  minizm_sync_transport_t link_state;
  minizm_sync_transport_init_stdio_link(&link, &link_state);
  xyzm_source_ops_t source;
  minizm_posix_file_source_t source_state;
  minizm_posix_init_file_source(&source, &source_state, local_path);

  /* Fill the fixed protocol settings before assembling the send request. */
  xyzm_zmodem_opts_t options;
  fill_default_zmodem_options(&options);

  /* Point the protocol core at the custom transport and default file source. */
  xyzm_zmodem_send_request_t request;
  request.link = &link;
  request.source = &source;
  request.options = &options;
  request.observer = NULL;

  /* Execute the transfer, then restore the terminal before reporting the result. */
  xyzm_transfer_report_t report;
  xyzm_status_t status = xyzm_zmodem_send(&request, &report);
  minizm_posix_tty_guard_restore(&tty_guard);
  print_report("send", &report);
  return status == XYZM_STATUS_OK ? 0 : 1;
}

/**
 * @brief Receives one file through the custom synchronous transport.
 *
 * @param output_dir Directory that will receive the transferred file.
 * @return Process exit code for the sample command.
 */
static int run_receive(const char *output_dir) {
  /* Put the terminal into raw mode only when the sample is attached to a tty. */
  minizm_posix_tty_guard_t tty_guard;
  if (minizm_posix_tty_guard_enable_raw(&tty_guard, STDIN_FILENO) != 0) {
    perror("minizm_posix_tty_guard_enable_raw");
    return 1;
  }

  /* Swap only the transport wiring; keep the default file sink helper. */
  xyzm_link_ops_t link;
  minizm_sync_transport_t link_state;
  minizm_sync_transport_init_stdio_link(&link, &link_state);
  xyzm_sink_ops_t sink;
  minizm_posix_file_sink_t sink_state;
  minizm_posix_init_file_sink(&sink, &sink_state, output_dir, "received.bin", true);

  /* Fill the fixed protocol settings before assembling the receive request. */
  xyzm_zmodem_opts_t options;
  fill_default_zmodem_options(&options);

  /* Point the protocol core at the custom transport and default file sink. */
  xyzm_zmodem_receive_request_t request;
  request.link = &link;
  request.sink = &sink;
  request.options = &options;
  request.observer = NULL;

  /* Execute the transfer, then restore the terminal before reporting the result. */
  xyzm_transfer_report_t report;
  xyzm_status_t status = xyzm_zmodem_receive(&request, &report);
  minizm_posix_tty_guard_restore(&tty_guard);
  print_report("recv", &report);
  return status == XYZM_STATUS_OK ? 0 : 1;
}

/**
 * @brief Dispatches the sample CLI to the send or receive path.
 *
 * @param argc Argument count received by the process.
 * @param argv Argument vector received by the process.
 * @return Process exit code for the sample command.
 */
int main(int argc, char **argv) {
  /* Reject incomplete or extra arguments before any protocol setup begins. */
  if (argc != 3) {
    print_usage(stderr, argv[0]);
    return 1;
  }

  /* Route the command to the matching single-file transfer entry point. */
  if (strcmp(argv[1], "send") == 0) {
    return run_send(argv[2]);
  }
  if (strcmp(argv[1], "recv") == 0) {
    return run_receive(argv[2]);
  }

  /* Fall back to usage text when the subcommand is not recognized. */
  print_usage(stderr, argv[0]);
  return 1;
}
