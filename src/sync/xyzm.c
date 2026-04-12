/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "../xyzm_sync_internal.h"

#include <string.h>

/**
 * @brief Converts an absolute deadline into the remaining timeout expected by link ops.
 *
 * @param runtime Active transfer state that supplies the monotonic clock.
 * @param deadline_ms Absolute timeout deadline in milliseconds.
 * @return Remaining timeout in milliseconds.
 * @remarks Returns zero when the deadline already passed.
 */
static inline uint32_t xyzm_remaining_timeout(
    xyzm_runtime_t *runtime,
    uint64_t deadline_ms) {
  uint64_t now_ms;

  now_ms = runtime->link->now_ms(runtime->link->opaque);
  if (now_ms >= deadline_ms) {
    return 0u;
  }
  if ((deadline_ms - now_ms) > 0xffffffffu) {
    return 0xffffffffu;
  }
  return (uint32_t)(deadline_ms - now_ms);
}

void xyzm_report_init(xyzm_transfer_report_t *report) {
  if (report == NULL) {
    return;
  }
  report->final_status = XYZM_STATUS_OK;
  report->payload_bytes = 0u;
  report->completed_files = 0u;
  report->retry_count = 0u;
}

void xyzm_report_retry(xyzm_runtime_t *runtime) {
  if (runtime->report != NULL) {
    runtime->report->retry_count += 1u;
  }
  xyzm_notify_transfer_event(
      runtime,
      XYZM_TRANSFER_EVENT_RETRY,
      XYZM_STATUS_OK,
      NULL,
      0u);
}

void xyzm_report_payload(
    xyzm_runtime_t *runtime,
    uint64_t payload_delta,
    const xyzm_file_info_t *file_info) {
  if ((runtime != NULL) && (runtime->report != NULL)) {
    runtime->report->payload_bytes += payload_delta;
  }
  if (payload_delta == 0u) {
    return;
  }
  xyzm_notify_transfer_event(
      runtime,
      XYZM_TRANSFER_EVENT_PROGRESS,
      XYZM_STATUS_OK,
      file_info,
      payload_delta);
}

void xyzm_report_completed_files(
    xyzm_runtime_t *runtime,
    uint32_t completed_files) {
  if ((runtime == NULL) || (runtime->report == NULL)) {
    return;
  }
  runtime->report->completed_files = completed_files;
}

void xyzm_report_completed_file(xyzm_runtime_t *runtime) {
  if ((runtime == NULL) || (runtime->report == NULL)) {
    return;
  }
  runtime->report->completed_files += 1u;
}

void xyzm_notify_transfer_event(
    xyzm_runtime_t *runtime,
    xyzm_transfer_event_kind_t kind,
    xyzm_status_t status,
    const xyzm_file_info_t *file_info,
    uint64_t payload_delta) {
  xyzm_transfer_event_t event;

  if ((runtime == NULL) || (runtime->observer == NULL) ||
      (runtime->observer->notify == NULL)) {
    return;
  }
  event.kind = kind;
  event.protocol = runtime->protocol;
  event.direction = runtime->direction;
  event.status = status;
  event.file_info = file_info;
  event.payload_delta = payload_delta;
  event.report = runtime->report;
  runtime->observer->notify(runtime->observer->opaque, &event);
}

void xyzm_notify_transfer_started(xyzm_runtime_t *runtime) {
  xyzm_notify_transfer_event(
      runtime,
      XYZM_TRANSFER_EVENT_TRANSFER_STARTED,
      XYZM_STATUS_OK,
      NULL,
      0u);
}

void xyzm_notify_file_started(
    xyzm_runtime_t *runtime,
    const xyzm_file_info_t *file_info) {
  xyzm_notify_transfer_event(
      runtime,
      XYZM_TRANSFER_EVENT_FILE_STARTED,
      XYZM_STATUS_OK,
      file_info,
      0u);
}

void xyzm_notify_file_finished(
    xyzm_runtime_t *runtime,
    const xyzm_file_info_t *file_info,
    xyzm_status_t status) {
  xyzm_notify_transfer_event(
      runtime,
      XYZM_TRANSFER_EVENT_FILE_FINISHED,
      status,
      file_info,
      0u);
}

xyzm_status_t xyzm_finish_transfer(
    xyzm_runtime_t *runtime,
    xyzm_status_t status) {
  if ((runtime != NULL) && (runtime->report != NULL)) {
    runtime->report->final_status = status;
  }
  xyzm_notify_transfer_event(
      runtime,
      XYZM_TRANSFER_EVENT_TRANSFER_FINISHED,
      status,
      NULL,
      0u);
  return status;
}

xyzm_status_t xyzm_send_exact(
    xyzm_runtime_t *runtime,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms) {
  uint64_t deadline_ms;
  size_t sent_len;

  deadline_ms =
      runtime->link->now_ms(runtime->link->opaque) + (uint64_t)timeout_ms;
  sent_len = 0u;
  while (sent_len < len) {
    size_t written_len;
    xyzm_status_t status;
    uint32_t remaining_ms;

    remaining_ms = xyzm_remaining_timeout(runtime, deadline_ms);
    if (remaining_ms == 0u) {
      return XYZM_STATUS_TIMEOUT;
    }
    written_len = 0u;
    status = runtime->link->send(
        runtime->link->opaque,
        buf + sent_len,
        len - sent_len,
        remaining_ms,
        &written_len);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (written_len == 0u) {
      return XYZM_STATUS_IO_ERROR;
    }
    sent_len += written_len;
  }
  return XYZM_STATUS_OK;
}

xyzm_status_t xyzm_recv_exact(
    xyzm_runtime_t *runtime,
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms) {
  uint64_t deadline_ms;
  size_t read_total;

  deadline_ms =
      runtime->link->now_ms(runtime->link->opaque) + (uint64_t)timeout_ms;
  read_total = 0u;
  while (read_total < len) {
    size_t read_len;
    xyzm_status_t status;
    uint32_t remaining_ms;

    remaining_ms = xyzm_remaining_timeout(runtime, deadline_ms);
    if (remaining_ms == 0u) {
      return XYZM_STATUS_TIMEOUT;
    }
    read_len = 0u;
    status = runtime->link->recv(
        runtime->link->opaque,
        buf + read_total,
        len - read_total,
        remaining_ms,
        &read_len);
    if (status != XYZM_STATUS_OK) {
      return status;
    }
    if (read_len == 0u) {
      return XYZM_STATUS_IO_ERROR;
    }
    read_total += read_len;
  }
  return XYZM_STATUS_OK;
}

xyzm_status_t xyzm_send_byte(
    xyzm_runtime_t *runtime,
    uint8_t value,
    uint32_t timeout_ms) {
  return xyzm_send_exact(runtime, &value, 1u, timeout_ms);
}

xyzm_status_t xyzm_recv_byte(
    xyzm_runtime_t *runtime,
    uint8_t *value,
    uint32_t timeout_ms) {
  return xyzm_recv_exact(runtime, value, 1u, timeout_ms);
}

xyzm_status_t xyzm_send_cancel(xyzm_runtime_t *runtime) {
  uint8_t bytes[2];

  bytes[0] = XYZM_CAN;
  bytes[1] = XYZM_CAN;
  return xyzm_send_exact(runtime, bytes, sizeof(bytes), runtime->options->block_timeout_ms);
}

xyzm_status_t xyzm_send_single_file(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file) {
  xyzm_status_t status;

  (void)runtime;
  memset(file, 0, sizeof(*file));
  status = source->next(source->opaque, &file->info, &file->file_opaque);
  if (status != XYZM_STATUS_OK) {
    return status;
  }
  xyzm_notify_file_started(runtime, &file->info);
  return XYZM_STATUS_OK;
}

xyzm_status_t xyzm_finish_send_file(
    xyzm_runtime_t *runtime,
    const xyzm_source_ops_t *source,
    xyzm_send_file_t *file,
    xyzm_status_t result) {
  bool had_file;

  had_file = (file != NULL) && (file->file_opaque != NULL);
  if ((source == NULL) || (source->end == NULL) || (file == NULL) ||
      (file->file_opaque == NULL)) {
    if (had_file) {
      xyzm_notify_file_finished(runtime, &file->info, result);
    }
    return result;
  }
  if (source->end(source->opaque, file->file_opaque, &file->info, result) !=
      XYZM_STATUS_OK) {
    result = XYZM_STATUS_IO_ERROR;
  }
  file->file_opaque = NULL;
  if (had_file) {
    xyzm_notify_file_finished(runtime, &file->info, result);
  }
  return result;
}

xyzm_status_t xyzm_finish_receive_file(
    xyzm_runtime_t *runtime,
    const xyzm_sink_ops_t *sink,
    xyzm_receive_file_t *file,
    xyzm_status_t result) {
  bool had_file;

  had_file = (file != NULL) && (file->file_opaque != NULL);
  if ((sink == NULL) || (sink->end == NULL) || (file == NULL) ||
      (file->file_opaque == NULL)) {
    if (had_file) {
      xyzm_notify_file_finished(runtime, &file->info, result);
    }
    return result;
  }
  if (sink->end(sink->opaque, file->file_opaque, &file->info, result) !=
      XYZM_STATUS_OK) {
    result = XYZM_STATUS_IO_ERROR;
  }
  file->file_opaque = NULL;
  if (had_file) {
    xyzm_notify_file_finished(runtime, &file->info, result);
  }
  return result;
}

bool xyzm_is_valid_link(const xyzm_link_ops_t *link) {
  return (link != NULL) && (link->send != NULL) && (link->recv != NULL) &&
      (link->now_ms != NULL);
}

xyzm_status_t xyzm_validate_sync_send_request(
    const xyzm_link_ops_t *link,
    const xyzm_source_ops_t *source,
    xyzm_transfer_report_t *report) {
  if (!xyzm_is_valid_link(link) || (source == NULL) || (source->next == NULL) ||
      (source->read == NULL)) {
    if (report != NULL) {
      report->final_status = XYZM_STATUS_ARGUMENT_ERROR;
    }
    return XYZM_STATUS_ARGUMENT_ERROR;
  }
  return XYZM_STATUS_OK;
}

xyzm_status_t xyzm_validate_sync_receive_request(
    const xyzm_link_ops_t *link,
    const xyzm_sink_ops_t *sink,
    xyzm_transfer_report_t *report) {
  if (!xyzm_is_valid_link(link) || (sink == NULL) || (sink->begin == NULL) ||
      (sink->write == NULL)) {
    if (report != NULL) {
      report->final_status = XYZM_STATUS_ARGUMENT_ERROR;
    }
    return XYZM_STATUS_ARGUMENT_ERROR;
  }
  return XYZM_STATUS_OK;
}
