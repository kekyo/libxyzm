/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#pragma once

#ifndef XYZM_SYNC_INTERNAL_H
#define XYZM_SYNC_INTERNAL_H

#include "libxyzm/xyzm.h"
#include "xyzm_common_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xyzm_runtime {
  const xyzm_link_ops_t *link;
  const xyzm_runtime_options_t *options;
  xyzm_transfer_report_t *report;
  const xyzm_transfer_observer_t *observer;
  xyzm_transfer_protocol_t protocol;
  xyzm_transfer_direction_t direction;
} xyzm_runtime_t;

typedef struct xyzm_send_file {
  xyzm_file_info_t info;
  void *file_opaque;
} xyzm_send_file_t;

typedef struct xyzm_receive_file {
  xyzm_file_info_t info;
  char name_storage[256];
  void *file_opaque;
} xyzm_receive_file_t;

void xyzm_report_init(xyzm_transfer_report_t *report);
void xyzm_report_retry(xyzm_runtime_t *runtime);
void xyzm_report_payload(
    xyzm_runtime_t *runtime,
    uint64_t payload_delta,
    const xyzm_file_info_t *file_info);
void xyzm_report_completed_files(
    xyzm_runtime_t *runtime,
    uint32_t completed_files);
void xyzm_report_completed_file(xyzm_runtime_t *runtime);
void xyzm_notify_transfer_event(
    xyzm_runtime_t *runtime,
    xyzm_transfer_event_kind_t kind,
    xyzm_status_t status,
    const xyzm_file_info_t *file_info,
    uint64_t payload_delta);
void xyzm_notify_transfer_started(xyzm_runtime_t *runtime);
void xyzm_notify_file_started(
    xyzm_runtime_t *runtime,
    const xyzm_file_info_t *file_info);
void xyzm_notify_file_finished(
    xyzm_runtime_t *runtime,
    const xyzm_file_info_t *file_info,
    xyzm_status_t status);
xyzm_status_t xyzm_finish_transfer(
    xyzm_runtime_t *runtime,
    xyzm_status_t status);
bool xyzm_is_valid_link(const xyzm_link_ops_t *link);
xyzm_status_t xyzm_validate_sync_send_request(
    const xyzm_link_ops_t *link,
    const xyzm_source_ops_t *source,
    xyzm_transfer_report_t *report);
xyzm_status_t xyzm_validate_sync_receive_request(
    const xyzm_link_ops_t *link,
    const xyzm_sink_ops_t *sink,
    xyzm_transfer_report_t *report);

xyzm_status_t xyzm_send_exact(
    xyzm_runtime_t *runtime,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms);

xyzm_status_t xyzm_recv_exact(
    xyzm_runtime_t *runtime,
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms);

xyzm_status_t xyzm_send_byte(
    xyzm_runtime_t *runtime,
    uint8_t value,
    uint32_t timeout_ms);

xyzm_status_t xyzm_recv_byte(
    xyzm_runtime_t *runtime,
    uint8_t *value,
    uint32_t timeout_ms);

xyzm_status_t xyzm_send_cancel(xyzm_runtime_t *runtime);

xyzm_status_t xyzm_send_single_file(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file);

xyzm_status_t xyzm_finish_send_file(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    xyzm_status_t result);

xyzm_status_t xyzm_finish_receive_file(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink,
    xyzm_receive_file_t *file,
    xyzm_status_t result);

xyzm_status_t xyzm_xmodem_send_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file);

xyzm_status_t xyzm_xmodem_receive_impl(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink);

xyzm_status_t xyzm_ymodem_send_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file);
xyzm_status_t xyzm_ymodem_send_batch_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file);

xyzm_status_t xyzm_ymodem_receive_impl(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink);

xyzm_status_t xyzm_zmodem_send_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file);
xyzm_status_t xyzm_zmodem_send_batch_impl(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file);

xyzm_status_t xyzm_zmodem_receive_impl(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink);

#ifdef __cplusplus
}
#endif

#endif
