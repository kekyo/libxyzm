/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

typedef struct zmodem_batch_memory_file {
  xyzm_file_info_t info;
  const uint8_t *data;
  size_t data_len;
  size_t offset;
  xyzm_status_t end_result;
} zmodem_batch_memory_file_t;

typedef struct zmodem_batch_memory_source {
  zmodem_batch_memory_file_t *files;
  size_t file_count;
  size_t next_index;
} zmodem_batch_memory_source_t;

typedef struct zmodem_midstream_link {
  uint8_t incoming[2048];
  size_t incoming_len;
  size_t incoming_pos;
  uint8_t outgoing[8192];
  size_t outgoing_len;
  uint64_t now_ms;
  size_t max_write_chunk;
  size_t first_subpacket_wire_len;
  uint8_t zdata_zero_header[32];
  size_t zdata_zero_header_len;
  uint8_t zdata_resume_header[32];
  size_t zdata_resume_header_len;
  uint8_t zeof_prefix[4];
  uint64_t resume_offset;
  bool zrpos_injected;
  bool zrpos_injected_before_zeof;
  bool zeof_seen_before_zrpos;
  bool final_handshake_injected;
} zmodem_midstream_link_t;

typedef struct zmodem_midstream_source {
  xyzm_file_info_t info;
  const uint8_t *data;
  size_t data_len;
  size_t offset;
  bool served;
  size_t seek_count;
  uint64_t last_seek_offset;
  bool seek_after_zeof;
  xyzm_status_t end_result;
  zmodem_midstream_link_t *link;
} zmodem_midstream_source_t;

static xyzm_status_t zmodem_batch_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  zmodem_batch_memory_source_t *source = opaque;
  zmodem_batch_memory_file_t *file;

  if (source->next_index >= source->file_count) {
    return XYZM_STATUS_END;
  }
  file = &source->files[source->next_index];
  source->next_index += 1u;
  file->offset = 0u;
  *info = file->info;
  *file_opaque = file;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_batch_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  zmodem_batch_memory_file_t *file = file_opaque;
  size_t available_len;
  size_t copy_len;

  (void)opaque;
  available_len = file->data_len - file->offset;
  if (available_len == 0u) {
    *read_len = 0u;
    return XYZM_STATUS_END;
  }
  copy_len = (available_len < capacity) ? available_len : capacity;
  memcpy(buf, file->data + file->offset, copy_len);
  file->offset += copy_len;
  *read_len = copy_len;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_batch_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset) {
  zmodem_batch_memory_file_t *file = file_opaque;

  (void)opaque;
  if (offset > file->data_len) {
    return XYZM_STATUS_IO_ERROR;
  }
  file->offset = (size_t)offset;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_batch_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  zmodem_batch_memory_file_t *file = file_opaque;

  (void)opaque;
  (void)info;
  file->end_result = result;
  return XYZM_STATUS_OK;
}

static void insert_bytes(
    uint8_t *buf,
    size_t *used_len,
    size_t position,
    const uint8_t *insert,
    size_t insert_len) {
  ASSERT_TRUE(position <= *used_len);
  memmove(buf + position + insert_len, buf + position, *used_len - position);
  memcpy(buf + position, insert, insert_len);
  *used_len += insert_len;
}

static void store_zmodem_offset(uint8_t hdr[4], uint64_t offset) {
  hdr[0] = (uint8_t)(offset & 0xffu);
  hdr[1] = (uint8_t)((offset >> 8) & 0xffu);
  hdr[2] = (uint8_t)((offset >> 16) & 0xffu);
  hdr[3] = (uint8_t)((offset >> 24) & 0xffu);
}

static size_t make_zmodem_metadata(
    uint8_t *metadata,
    size_t capacity,
    const char *name,
    uint64_t size_bytes) {
  char size_text[32];
  size_t name_len;
  size_t size_len;
  int written_len;

  name_len = strlen(name);
  written_len = snprintf(
      size_text,
      sizeof(size_text),
      "%llu",
      (unsigned long long)size_bytes);
  ASSERT_TRUE(written_len > 0);
  size_len = (size_t)written_len;
  ASSERT_TRUE(name_len + 1u + size_len + 1u <= capacity);
  memcpy(metadata, name, name_len);
  metadata[name_len] = '\0';
  memcpy(metadata + name_len + 1u, size_text, size_len);
  metadata[name_len + 1u + size_len] = '\0';
  return name_len + 1u + size_len + 1u;
}

static void zmodem_midstream_append_incoming(
    zmodem_midstream_link_t *link,
    const uint8_t *buf,
    size_t len) {
  ASSERT_TRUE((link->incoming_len + len) <= sizeof(link->incoming));
  memcpy(link->incoming + link->incoming_len, buf, len);
  link->incoming_len += len;
}

static void zmodem_midstream_append_incoming_hex_header(
    zmodem_midstream_link_t *link,
    uint8_t type,
    const uint8_t hdr[4]) {
  uint8_t frame[32];
  size_t frame_len;

  frame_len = 0u;
  append_zmodem_hex_header(frame, &frame_len, type, hdr);
  zmodem_midstream_append_incoming(link, frame, frame_len);
}

static bool zmodem_midstream_has_zeof(
    const zmodem_midstream_link_t *link) {
  return find_bytes(
      link->outgoing,
      link->outgoing_len,
      link->zeof_prefix,
      sizeof(link->zeof_prefix)) < link->outgoing_len;
}

static void zmodem_midstream_append_final_handshake(
    zmodem_midstream_link_t *link) {
  uint8_t zero_hdr[4];

  if (link->final_handshake_injected ||
      !zmodem_midstream_has_zeof(link)) {
    return;
  }
  memset(zero_hdr, 0, sizeof(zero_hdr));
  zmodem_midstream_append_incoming_hex_header(link, TEST_ZRINIT, zero_hdr);
  zmodem_midstream_append_incoming_hex_header(link, TEST_ZFIN, zero_hdr);
  link->final_handshake_injected = true;
}

static void zmodem_midstream_inject_zrpos_if_payload_is_active(
    zmodem_midstream_link_t *link) {
  uint8_t resume_hdr[4];
  size_t zdata_pos;
  size_t payload_begin;

  if (link->zrpos_injected) {
    return;
  }

  zdata_pos = find_bytes(
      link->outgoing,
      link->outgoing_len,
      link->zdata_zero_header,
      link->zdata_zero_header_len);
  if (zdata_pos >= link->outgoing_len) {
    return;
  }

  payload_begin = zdata_pos + link->zdata_zero_header_len;
  if ((link->outgoing_len <= payload_begin) ||
      (link->outgoing_len >= (payload_begin + link->first_subpacket_wire_len))) {
    return;
  }

  link->zeof_seen_before_zrpos = zmodem_midstream_has_zeof(link);
  memset(resume_hdr, 0, sizeof(resume_hdr));
  store_zmodem_offset(resume_hdr, link->resume_offset);
  zmodem_midstream_append_incoming_hex_header(link, TEST_ZRPOS, resume_hdr);
  link->zrpos_injected = true;
  link->zrpos_injected_before_zeof = !link->zeof_seen_before_zrpos;
}

static void zmodem_midstream_link_init(
    zmodem_midstream_link_t *link,
    const uint8_t *payload,
    size_t packet_len,
    uint64_t resume_offset) {
  uint8_t zero_hdr[4];
  uint8_t resume_hdr[4];
  uint8_t first_subpacket[128];
  size_t first_subpacket_len;

  memset(link, 0, sizeof(*link));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  memset(resume_hdr, 0, sizeof(resume_hdr));
  store_zmodem_offset(resume_hdr, resume_offset);

  link->max_write_chunk = 8u;
  link->resume_offset = resume_offset;
  link->zeof_prefix[0] = TEST_ZPAD;
  link->zeof_prefix[1] = TEST_ZDLE;
  link->zeof_prefix[2] = TEST_ZBIN;
  link->zeof_prefix[3] = TEST_ZEOF;

  append_zmodem_binary_header(
      link->zdata_zero_header,
      &link->zdata_zero_header_len,
      TEST_ZDATA,
      zero_hdr,
      false,
      false);
  append_zmodem_binary_header(
      link->zdata_resume_header,
      &link->zdata_resume_header_len,
      TEST_ZDATA,
      resume_hdr,
      false,
      false);

  first_subpacket_len = 0u;
  append_zmodem_data_subpacket(
      first_subpacket,
      &first_subpacket_len,
      payload,
      packet_len,
      TEST_ZCRCG,
      false,
      false);
  link->first_subpacket_wire_len = first_subpacket_len;

  zmodem_midstream_append_incoming_hex_header(link, TEST_ZRINIT, zero_hdr);
  zmodem_midstream_append_incoming_hex_header(link, TEST_ZRPOS, zero_hdr);
}

static xyzm_status_t zmodem_midstream_link_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  zmodem_midstream_link_t *link;
  size_t copy_len;

  (void)timeout_ms;
  link = opaque;
  copy_len = (len < link->max_write_chunk) ? len : link->max_write_chunk;
  ASSERT_TRUE(copy_len > 0u);
  ASSERT_TRUE((link->outgoing_len + copy_len) <= sizeof(link->outgoing));
  memcpy(link->outgoing + link->outgoing_len, buf, copy_len);
  link->outgoing_len += copy_len;
  *written_len = copy_len;
  link->now_ms += 1u;

  zmodem_midstream_inject_zrpos_if_payload_is_active(link);
  zmodem_midstream_append_final_handshake(link);
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_midstream_link_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  zmodem_midstream_link_t *link;
  size_t available_len;
  size_t copy_len;

  link = opaque;
  available_len = link->incoming_len - link->incoming_pos;
  if (available_len == 0u) {
    link->now_ms += timeout_ms;
    *read_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
  copy_len = (available_len < capacity) ? available_len : capacity;
  memcpy(buf, link->incoming + link->incoming_pos, copy_len);
  link->incoming_pos += copy_len;
  *read_len = copy_len;
  link->now_ms += 1u;
  return XYZM_STATUS_OK;
}

static uint64_t zmodem_midstream_link_now_ms(void *opaque) {
  zmodem_midstream_link_t *link;

  link = opaque;
  return link->now_ms;
}

static xyzm_status_t zmodem_midstream_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  zmodem_midstream_source_t *source;

  source = opaque;
  if (source->served) {
    return XYZM_STATUS_END;
  }
  source->served = true;
  *info = source->info;
  *file_opaque = source;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_midstream_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  zmodem_midstream_source_t *source;
  size_t available_len;
  size_t copy_len;

  (void)opaque;
  source = file_opaque;
  available_len = source->data_len - source->offset;
  if (available_len == 0u) {
    *read_len = 0u;
    return XYZM_STATUS_END;
  }
  copy_len = (available_len < capacity) ? available_len : capacity;
  memcpy(buf, source->data + source->offset, copy_len);
  source->offset += copy_len;
  *read_len = copy_len;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_midstream_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset) {
  zmodem_midstream_source_t *source;

  (void)opaque;
  source = file_opaque;
  if (offset > source->data_len) {
    return XYZM_STATUS_IO_ERROR;
  }
  source->seek_count += 1u;
  source->last_seek_offset = offset;
  source->seek_after_zeof =
      source->seek_after_zeof ||
      ((source->link != NULL) && source->link->final_handshake_injected);
  source->offset = (size_t)offset;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_midstream_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  zmodem_midstream_source_t *source;

  (void)opaque;
  (void)info;
  source = file_opaque;
  source->end_result = result;
  return XYZM_STATUS_OK;
}

static xyzm_status_t zmodem_midstream_send(
    zmodem_midstream_link_t *link,
    zmodem_midstream_source_t *source,
    const test_sync_transfer_opts_t *options,
    xyzm_transfer_report_t *report,
    size_t *completion_calls) {
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_send_request_t request;

  *completion_calls = 0u;

  link_ops.opaque = link;
  link_ops.send = zmodem_midstream_link_send;
  link_ops.recv = zmodem_midstream_link_recv;
  link_ops.now_ms = zmodem_midstream_link_now_ms;

  source_ops.opaque = source;
  source_ops.next = zmodem_midstream_source_next;
  source_ops.read = zmodem_midstream_source_read;
  source_ops.seek = zmodem_midstream_source_seek;
  source_ops.end = zmodem_midstream_source_end;

  request.link = &link_ops;
  request.source = &source_ops;
  request.options = options;
  return test_sync_send(&request, report);
}

static void test_zmodem_send_crc16_packet_len(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t zero_hdr[4];
  uint8_t data[80];
  uint8_t zfile_prefix[] = {'*', 0x18u, 'A', TEST_ZFILE};
  uint8_t zdata_prefix[] = {'*', 0x18u, 'A', TEST_ZDATA};
  size_t zdata_pos;
  size_t index;

  memset(&link, 0, sizeof(link));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "packet.bin";
  source.info.size_bytes = sizeof(data);
  source.data = data;
  source.data_len = sizeof(data);
  memset(data, 'A', sizeof(data));

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  options.zmodem_packet_len = 64u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, zfile_prefix, sizeof(zfile_prefix)) <
      link.outgoing_len);
  zdata_pos = find_bytes(link.outgoing, link.outgoing_len, zdata_prefix, sizeof(zdata_prefix));
  ASSERT_TRUE(zdata_pos < link.outgoing_len);
  zdata_pos += 10u;
  for (index = 0u; index < 64u; ++index) {
    ASSERT_EQ_U8(link.outgoing[zdata_pos + index], 'A');
  }
  ASSERT_EQ_U8(link.outgoing[zdata_pos + 64u], 0x18u);
  ASSERT_EQ_U8(link.outgoing[zdata_pos + 65u], 'i');
}

static void test_zmodem_send_escape_option(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t zero_hdr[4];
  static const uint8_t data[] = {0x01u, 'A', 'B'};
  static const uint8_t raw_pattern[] = {0x01u, 'A', 'B'};
  static const uint8_t escaped_pattern[] = {0x18u, 0x41u, 'A', 'B'};
  size_t raw_pos;
  size_t escaped_pos;

  memset(&link, 0, sizeof(link));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  zrinit_hdr[3] = 0x20u;
  memset(zero_hdr, 0, sizeof(zero_hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "escape.bin";
  source.info.size_bytes = sizeof(data);
  source.data = data;
  source.data_len = sizeof(data);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_escape_ctrl = 1u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  raw_pos = find_bytes(link.outgoing, link.outgoing_len, raw_pattern, sizeof(raw_pattern));
  escaped_pos = find_bytes(
      link.outgoing,
      link.outgoing_len,
      escaped_pattern,
      sizeof(escaped_pattern));
  ASSERT_TRUE(raw_pos == link.outgoing_len);
  ASSERT_TRUE(escaped_pos < link.outgoing_len);
}

static void test_zmodem_send_telnet_escape(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t zero_hdr[4];
  static const uint8_t data[] = {'@', '\r', 'A'};
  static const uint8_t raw_pattern[] = {'@', '\r', 'A'};
  static const uint8_t escaped_pattern[] = {'@', TEST_ZDLE, (uint8_t)('\r' ^ 0x40u), 'A'};
  size_t raw_pos;
  size_t escaped_pos;

  memset(&link, 0, sizeof(link));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "telnet.bin";
  source.info.size_bytes = sizeof(data);
  source.data = data;
  source.data_len = sizeof(data);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  raw_pos = find_bytes(link.outgoing, link.outgoing_len, raw_pattern, sizeof(raw_pattern));
  escaped_pos = find_bytes(
      link.outgoing,
      link.outgoing_len,
      escaped_pattern,
      sizeof(escaped_pattern));
  ASSERT_TRUE(raw_pos == link.outgoing_len);
  ASSERT_TRUE(escaped_pos < link.outgoing_len);
}

static void test_zmodem_packet_boundary(void) {
  struct {
    size_t data_len;
    size_t expected_crcg_count;
  } cases[] = {
      {31u, 0u},
      {32u, 1u},
      {33u, 1u},
  };
  static const uint8_t zcrcg_pattern[] = {TEST_ZDLE, TEST_ZCRCG};
  static const uint8_t zcrce_pattern[] = {TEST_ZDLE, TEST_ZCRCE};
  size_t case_index;

  for (case_index = 0u; case_index < (sizeof(cases) / sizeof(cases[0])); ++case_index) {
    fake_link_t link;
    memory_source_t source;
    xyzm_link_ops_t link_ops;
    xyzm_source_ops_t source_ops;
    test_sync_transfer_opts_t options;
    test_sync_send_request_t request;
    xyzm_transfer_report_t report;
    xyzm_status_t status;
    uint8_t zrinit_hdr[4];
    uint8_t zero_hdr[4];
    uint8_t eof_hdr[4];
    uint8_t data[33];
    uint8_t expected[64];
    uint8_t final_payload[32];
    size_t expected_len;
    size_t index;

    memset(&link, 0, sizeof(link));
    memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
    memset(zero_hdr, 0, sizeof(zero_hdr));
    memset(eof_hdr, 0, sizeof(eof_hdr));
    append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
    append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
    append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
    append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

    for (index = 0u; index < sizeof(data); ++index) {
      data[index] = (uint8_t)('A' + (index % 3u));
    }

    memset(&source, 0, sizeof(source));
    source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
    source.info.name = "packet.bin";
    source.info.size_bytes = cases[case_index].data_len;
    source.data = data;
    source.data_len = cases[case_index].data_len;

    link_ops.opaque = &link;
    link_ops.send = fake_send;
    link_ops.recv = fake_recv;
    link_ops.now_ms = fake_now_ms;

    source_ops.opaque = &source;
    source_ops.next = memory_source_next;
    source_ops.read = memory_source_read;
    source_ops.end = memory_source_end;

    populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
    options.zmodem_use_crc32 = 0u;
    options.zmodem_escape_ctrl = 0u;
    options.zmodem_packet_len = 32u;
    request.link = &link_ops;
    request.source = &source_ops;
    request.options = &options;

    status = test_sync_send(&request, &report);
    ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
    ASSERT_TRUE(report.payload_bytes == cases[case_index].data_len);
    ASSERT_EQ_SIZE(
        count_bytes_pattern(link.outgoing, link.outgoing_len, zcrcg_pattern, sizeof(zcrcg_pattern)),
        cases[case_index].expected_crcg_count);
    ASSERT_TRUE(
        count_bytes_pattern(link.outgoing, link.outgoing_len, zcrce_pattern, sizeof(zcrce_pattern)) >= 1u);

    expected_len = 0u;
    eof_hdr[0] = (uint8_t)(cases[case_index].data_len & 0xffu);
    eof_hdr[1] = (uint8_t)((cases[case_index].data_len >> 8) & 0xffu);
    eof_hdr[2] = (uint8_t)((cases[case_index].data_len >> 16) & 0xffu);
    eof_hdr[3] = (uint8_t)((cases[case_index].data_len >> 24) & 0xffu);
    append_zmodem_binary_header(
        expected,
        &expected_len,
        TEST_ZEOF,
        eof_hdr,
        false,
        false);
    ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);

    if (cases[case_index].data_len < 32u) {
      memcpy(final_payload, data, cases[case_index].data_len);
      expected_len = 0u;
      append_zmodem_data_subpacket(
          expected,
          &expected_len,
          final_payload,
          cases[case_index].data_len,
          TEST_ZCRCE,
          false,
          false);
      ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);
    } else if (cases[case_index].data_len == 32u) {
      expected_len = 0u;
      append_zmodem_data_subpacket(
          expected,
          &expected_len,
          NULL,
          0u,
          TEST_ZCRCE,
          false,
          false);
      ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);
    } else {
      final_payload[0] = data[32];
      expected_len = 0u;
      append_zmodem_data_subpacket(
          expected,
          &expected_len,
          final_payload,
          1u,
          TEST_ZCRCE,
          false,
          false);
      ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);
    }
  }
}

static void test_zmodem_send_single_byte(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t zero_hdr[4];
  uint8_t expected[32];
  uint8_t eof_hdr[4];
  size_t expected_len;

  memset(&link, 0, sizeof(link));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  memset(eof_hdr, 0, sizeof(eof_hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "one.bin";
  source.info.size_bytes = 1u;
  source.data = (const uint8_t *)"Z";
  source.data_len = 1u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.payload_bytes == 1u);
  ASSERT_TRUE(report.completed_files == 1u);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_OK);

  expected_len = 0u;
  append_zmodem_data_subpacket(
      expected,
      &expected_len,
      (const uint8_t *)"Z",
      1u,
      TEST_ZCRCE,
      false,
      false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);

  eof_hdr[0] = 1u;
  expected_len = 0u;
  append_zmodem_binary_header(
      expected,
      &expected_len,
      TEST_ZEOF,
      eof_hdr,
      false,
      false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);
}

static void test_zmodem_send_single_byte_without_report(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t zero_hdr[4];
  uint8_t expected[32];
  uint8_t eof_hdr[4];
  size_t expected_len;

  memset(&link, 0, sizeof(link));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  memset(eof_hdr, 0, sizeof(eof_hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "one.bin";
  source.info.size_bytes = 1u;
  source.data = (const uint8_t *)"Z";
  source.data_len = 1u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, NULL);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_OK);

  expected_len = 0u;
  append_zmodem_data_subpacket(
      expected,
      &expected_len,
      (const uint8_t *)"Z",
      1u,
      TEST_ZCRCE,
      false,
      false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);

  eof_hdr[0] = 1u;
  expected_len = 0u;
  append_zmodem_binary_header(
      expected,
      &expected_len,
      TEST_ZEOF,
      eof_hdr,
      false,
      false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);
}

static void test_zmodem_send_remote_skip(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t zero_hdr[4];
  static const uint8_t payload[] = {'s', 'k', 'i', 'p'};
  static const uint8_t zdata_prefix[] = {'*', 0x18u, 'C', TEST_ZDATA};

  memset(&link, 0, sizeof(link));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  zrinit_hdr[3] = 0x20u;
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZSKIP, zero_hdr);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "skip.bin";
  source.info.size_bytes = sizeof(payload);
  source.data = payload;
  source.data_len = sizeof(payload);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_REMOTE_CANCELLED);
  ASSERT_EQ_STATUS(report.final_status, XYZM_STATUS_REMOTE_CANCELLED);
  ASSERT_TRUE(report.payload_bytes == 0u);
  ASSERT_TRUE(report.completed_files == 0u);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_REMOTE_CANCELLED);
  ASSERT_TRUE(source.offset == 0u);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, zdata_prefix, sizeof(zdata_prefix)) ==
      link.outgoing_len);
}

static void test_zmodem_send_resume_from_requested_offset(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t zrinit_hdr[4];
  uint8_t resume_hdr[4];
  uint8_t zero_hdr[4];
  uint8_t expected[128];
  uint8_t eof_hdr[4];
  size_t expected_len;
  static const uint8_t payload[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'};

  memset(&link, 0, sizeof(link));
  memset(&source, 0, sizeof(source));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  memset(resume_hdr, 0, sizeof(resume_hdr));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  store_zmodem_offset(resume_hdr, 4u);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, resume_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);

  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "resume-send.bin";
  source.info.size_bytes = sizeof(payload);
  source.data = payload;
  source.data_len = sizeof(payload);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.seek = memory_source_seek;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(source.offset, sizeof(payload));
  ASSERT_TRUE(report.payload_bytes == sizeof(payload) - 4u);
  ASSERT_TRUE(report.completed_files == 1u);

  expected_len = 0u;
  append_zmodem_binary_header(expected, &expected_len, TEST_ZDATA, resume_hdr, false, false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);

  expected_len = 0u;
  append_zmodem_data_subpacket(
      expected,
      &expected_len,
      payload + 4u,
      sizeof(payload) - 4u,
      TEST_ZCRCE,
      false,
      false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);

  memset(eof_hdr, 0, sizeof(eof_hdr));
  store_zmodem_offset(eof_hdr, sizeof(payload));
  expected_len = 0u;
  append_zmodem_binary_header(expected, &expected_len, TEST_ZEOF, eof_hdr, false, false);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected, expected_len) < link.outgoing_len);
}

static void test_zmodem_send_midstream_zrpos(void) {
  zmodem_midstream_link_t link;
  zmodem_midstream_source_t source;
  test_sync_transfer_opts_t options;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t payload[160];
  size_t completion_calls;
  size_t index;

  for (index = 0u; index < sizeof(payload); ++index) {
    payload[index] = (uint8_t)('A' + (index % 26u));
  }
  zmodem_midstream_link_init(&link, payload, 64u, 32u);

  memset(&source, 0, sizeof(source));
  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "midstream-zrpos.bin";
  source.info.size_bytes = sizeof(payload);
  source.data = payload;
  source.data_len = sizeof(payload);
  source.link = &link;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  options.zmodem_packet_len = 64u;

  status = zmodem_midstream_send(
      &link,
      &source,
      &options,
      &report,
      &completion_calls);

  ASSERT_TRUE(link.zrpos_injected);
  ASSERT_TRUE(link.zrpos_injected_before_zeof);
  ASSERT_TRUE(!link.zeof_seen_before_zrpos);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(source.seek_count > 0u);
  ASSERT_TRUE(!source.seek_after_zeof);
  ASSERT_TRUE(source.last_seek_offset == 32u);
  ASSERT_TRUE(find_bytes(
      link.outgoing,
      link.outgoing_len,
      link.zdata_resume_header,
      link.zdata_resume_header_len) < link.outgoing_len);
  ASSERT_TRUE(report.completed_files == 1u);
}

static void test_zmodem_send_resume_requires_seek(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t zrinit_hdr[4];
  static const uint8_t payload[] = {'A', 'B', 'C', 'D'};

  memset(&link, 0, sizeof(link));
  memset(&source, 0, sizeof(source));
  memset(hdr, 0, sizeof(hdr));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  store_zmodem_offset(hdr, 2u);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, hdr);

  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "no-seek.bin";
  source.info.size_bytes = sizeof(payload);
  source.data = payload;
  source.data_len = sizeof(payload);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.seek = NULL;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_UNSUPPORTED);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_UNSUPPORTED);
  ASSERT_TRUE(report.payload_bytes == 0u);
}

static void test_zmodem_send_resume_seek_failure(void) {
  fake_link_t link;
  memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  test_sync_transfer_opts_t options;
  test_sync_send_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t zrinit_hdr[4];
  static const uint8_t payload[] = {'A', 'B', 'C', 'D'};

  memset(&link, 0, sizeof(link));
  memset(&source, 0, sizeof(source));
  memset(hdr, 0, sizeof(hdr));
  memset(zrinit_hdr, 0, sizeof(zrinit_hdr));
  store_zmodem_offset(hdr, 2u);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zrinit_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, hdr);

  source.info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  source.info.name = "seek-fail.bin";
  source.info.size_bytes = sizeof(payload);
  source.data = payload;
  source.data_len = sizeof(payload);
  source.seek_result = XYZM_STATUS_IO_ERROR;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  source_ops.opaque = &source;
  source_ops.next = memory_source_next;
  source_ops.read = memory_source_read;
  source_ops.seek = memory_source_seek;
  source_ops.end = memory_source_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;

  status = test_sync_send(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_IO_ERROR);
  ASSERT_EQ_STATUS(source.end_result, XYZM_STATUS_IO_ERROR);
  ASSERT_TRUE(report.payload_bytes == 0u);
}

static void test_zmodem_receive_bad_header_crc(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr);
  link.incoming[17] = (link.incoming[17] == '0') ? '1' : '0';

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_PROTOCOL_ERROR);
  ASSERT_TRUE(!sink.began);
}

static void test_zmodem_receive_size_mismatch(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[16];
  uint8_t eof_hdr[4];

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  memset(metadata, 0, sizeof(metadata));
  memset(eof_hdr, 0, sizeof(eof_hdr));
  memcpy(metadata, "size.bin", 8u);
  metadata[8] = '\0';
  metadata[9] = '5';
  metadata[10] = '\0';

  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      metadata,
      11u,
      TEST_ZCRCW,
      false,
      false);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      (const uint8_t *)"abc",
      3u,
      TEST_ZCRCE,
      false,
      false);
  eof_hdr[0] = 3u;
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZEOF, eof_hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, hdr);
  link.incoming[link.incoming_len++] = 'O';
  link.incoming[link.incoming_len++] = 'O';

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_PROTOCOL_ERROR);
  ASSERT_EQ_SIZE(sink.data_len, 3u);
  ASSERT_TRUE(report.payload_bytes == 3u);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_PROTOCOL_ERROR);
}

static void test_zmodem_receive_extra_data_after_zero_size(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[16];

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "zero.bin", 8u);
  metadata[8] = '\0';
  metadata[9] = '0';
  metadata[10] = '\0';

  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      metadata,
      11u,
      TEST_ZCRCW,
      false,
      false);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      (const uint8_t *)"X",
      1u,
      TEST_ZCRCE,
      false,
      false);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_PROTOCOL_ERROR);
  ASSERT_EQ_SIZE(sink.data_len, 0u);
  ASSERT_TRUE(report.payload_bytes == 0u);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_PROTOCOL_ERROR);
}

static void test_zmodem_receive_ignores_flow_control(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[128];
  static const uint8_t payload[] = "ok";
  static const uint8_t flow_control[] = {0x11u, 0x13u, 0x91u, 0x93u};
  size_t data_subpacket_pos;
  size_t offset;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "flow.bin", 8u);
  memcpy(metadata + 9u, "2", 1u);

  offset = 0u;
  append_zmodem_binary_header(link.incoming, &offset, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(link.incoming, &offset, metadata, 11u, TEST_ZCRCW, false, false);
  append_zmodem_binary_header(link.incoming, &offset, TEST_ZDATA, hdr, false, false);
  data_subpacket_pos = offset;
  append_zmodem_data_subpacket(
      link.incoming,
      &offset,
      payload,
      sizeof(payload) - 1u,
      TEST_ZCRCE,
      false,
      false);
  insert_bytes(link.incoming, &offset, data_subpacket_pos, flow_control, sizeof(flow_control));
  hdr[0] = (uint8_t)(sizeof(payload) - 1u);
  append_zmodem_hex_header(link.incoming, &offset, TEST_ZEOF, hdr);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &offset, TEST_ZFIN, hdr);
  link.incoming[offset++] = 'O';
  link.incoming[offset++] = 'O';
  link.incoming_len = offset;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, sizeof(payload) - 1u);
  ASSERT_TRUE(memcmp(sink.data, payload, sizeof(payload) - 1u) == 0);
  ASSERT_TRUE(report.payload_bytes == (sizeof(payload) - 1u));
}

static void test_zmodem_receive_multiple_data_subpackets(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[32];
  static const uint8_t payload_part1[] = {'a', 'b'};
  static const uint8_t payload_part2[] = {'c', 'd'};
  static const uint8_t expected_payload[] = {'a', 'b', 'c', 'd'};

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  memset(metadata, 0, sizeof(metadata));
  memcpy(metadata, "multi.bin", 9u);
  metadata[9] = '\0';
  metadata[10] = '4';
  metadata[11] = '\0';

  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      metadata,
      12u,
      TEST_ZCRCW,
      false,
      false);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      payload_part1,
      sizeof(payload_part1),
      TEST_ZCRCG,
      false,
      false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      payload_part2,
      sizeof(payload_part2),
      TEST_ZCRCE,
      false,
      false);
  hdr[0] = (uint8_t)sizeof(expected_payload);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZEOF, hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, hdr);
  link.incoming[link.incoming_len++] = 'O';
  link.incoming[link.incoming_len++] = 'O';

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, sizeof(expected_payload));
  ASSERT_TRUE(memcmp(sink.data, expected_payload, sizeof(expected_payload)) == 0);
  ASSERT_TRUE(report.payload_bytes == sizeof(expected_payload));
}

static void test_zmodem_receive_resume_from_sink_offset(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[64];
  uint8_t expected_zrpos[32];
  size_t metadata_len;
  size_t expected_len;
  static const uint8_t existing_prefix[] = {'A', 'B', 'C', 'D'};
  static const uint8_t remaining_payload[] = {'E', 'F', 'G', 'H'};
  static const uint8_t expected_payload[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  metadata_len = make_zmodem_metadata(metadata, sizeof(metadata), "resume.bin", sizeof(expected_payload));

  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      metadata,
      metadata_len,
      TEST_ZCRCW,
      false,
      false);
  store_zmodem_offset(hdr, sizeof(existing_prefix));
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      remaining_payload,
      sizeof(remaining_payload),
      TEST_ZCRCE,
      false,
      false);
  store_zmodem_offset(hdr, sizeof(expected_payload));
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZEOF, hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, hdr);
  link.incoming[link.incoming_len++] = 'O';
  link.incoming[link.incoming_len++] = 'O';

  memcpy(sink.data, existing_prefix, sizeof(existing_prefix));
  sink.data_len = sizeof(existing_prefix);
  sink.resume_offset = sizeof(existing_prefix);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, sizeof(expected_payload));
  ASSERT_TRUE(memcmp(sink.data, expected_payload, sizeof(expected_payload)) == 0);
  ASSERT_TRUE(report.payload_bytes == sizeof(remaining_payload));
  ASSERT_TRUE(report.completed_files == 1u);

  store_zmodem_offset(hdr, sizeof(existing_prefix));
  expected_len = 0u;
  append_zmodem_hex_header(expected_zrpos, &expected_len, TEST_ZRPOS, hdr);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected_zrpos, expected_len) < link.outgoing_len);
}

static void test_zmodem_receive_resume_already_complete(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[64];
  uint8_t expected_zrpos[32];
  size_t metadata_len;
  size_t expected_len;
  static const uint8_t complete_payload[] = {'d', 'o', 'n', 'e'};

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  metadata_len = make_zmodem_metadata(metadata, sizeof(metadata), "complete.bin", sizeof(complete_payload));

  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      metadata,
      metadata_len,
      TEST_ZCRCW,
      false,
      false);
  store_zmodem_offset(hdr, sizeof(complete_payload));
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZEOF, hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, hdr);
  link.incoming[link.incoming_len++] = 'O';
  link.incoming[link.incoming_len++] = 'O';

  memcpy(sink.data, complete_payload, sizeof(complete_payload));
  sink.data_len = sizeof(complete_payload);
  sink.resume_offset = sizeof(complete_payload);

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_EQ_SIZE(sink.data_len, sizeof(complete_payload));
  ASSERT_TRUE(memcmp(sink.data, complete_payload, sizeof(complete_payload)) == 0);
  ASSERT_TRUE(report.payload_bytes == 0u);
  ASSERT_TRUE(report.completed_files == 1u);

  store_zmodem_offset(hdr, sizeof(complete_payload));
  expected_len = 0u;
  append_zmodem_hex_header(expected_zrpos, &expected_len, TEST_ZRPOS, hdr);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, expected_zrpos, expected_len) < link.outgoing_len);
}

static void test_zmodem_receive_resume_rejects_oversized_offset(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;
  uint8_t hdr[4];
  uint8_t metadata[64];
  size_t metadata_len;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  metadata_len = make_zmodem_metadata(metadata, sizeof(metadata), "oversize.bin", 4u);

  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(
      link.incoming,
      &link.incoming_len,
      metadata,
      metadata_len,
      TEST_ZCRCW,
      false,
      false);
  sink.resume_offset = 5u;

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.zmodem_use_crc32 = 0u;
  options.zmodem_escape_ctrl = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_TRUE(sink.began);
  ASSERT_EQ_STATUS(sink.end_result, XYZM_STATUS_ARGUMENT_ERROR);
  ASSERT_TRUE(report.payload_bytes == 0u);
}

static void test_zmodem_receive_timeout(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  test_sync_transfer_opts_t options;
  test_sync_receive_request_t request;
  xyzm_transfer_report_t report;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));

  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;

  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;

  populate_sync_default_options(&options, TEST_SYNC_PROTOCOL_ZMODEM);
  options.retry_limit = 2u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;

  status = test_sync_receive(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_TIMEOUT);
  ASSERT_TRUE(report.retry_count == 2u);
  ASSERT_TRUE(!sink.began);
}

static void test_zmodem_send_batch_sync_unit(void) {
  static const uint8_t first_data[] = {'A', 'B', 'C'};
  static const uint8_t second_data[] = {'D', 'E'};
  fake_link_t link;
  zmodem_batch_memory_file_t files[2];
  zmodem_batch_memory_source_t source;
  xyzm_link_ops_t link_ops;
  xyzm_source_ops_t source_ops;
  xyzm_zmodem_opts_t options;
  xyzm_zmodem_send_request_t request;
  xyzm_transfer_report_t report;
  uint8_t zero_hdr[4];
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memset(zero_hdr, 0, sizeof(zero_hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRPOS, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZRINIT, zero_hdr);
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, zero_hdr);
  memset(files, 0, sizeof(files));
  files[0].info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  files[0].info.name = "first.bin";
  files[0].info.size_bytes = sizeof(first_data);
  files[0].data = first_data;
  files[0].data_len = sizeof(first_data);
  files[1].info.valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  files[1].info.name = "second.bin";
  files[1].info.size_bytes = sizeof(second_data);
  files[1].data = second_data;
  files[1].data_len = sizeof(second_data);
  source.files = files;
  source.file_count = 2u;
  source.next_index = 0u;
  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;
  source_ops.opaque = &source;
  source_ops.next = zmodem_batch_source_next;
  source_ops.read = zmodem_batch_source_read;
  source_ops.seek = zmodem_batch_source_seek;
  source_ops.end = zmodem_batch_source_end;
  options.handshake_timeout_ms = 1000u;
  options.block_timeout_ms = 1000u;
  options.retry_limit = 10u;
  options.pad_byte = 0x1au;
  options.zmodem_packet_len = 0u;
  options.zmodem_escape_ctrl = 0u;
  options.zmodem_use_crc32 = 0u;
  request.link = &link_ops;
  request.source = &source_ops;
  request.options = &options;
  request.observer = NULL;

  status = xyzm_zmodem_send_batch(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.completed_files == 2u);
  ASSERT_TRUE(report.payload_bytes == sizeof(first_data) + sizeof(second_data));
  ASSERT_EQ_STATUS(files[0].end_result, XYZM_STATUS_OK);
  ASSERT_EQ_STATUS(files[1].end_result, XYZM_STATUS_OK);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, (const uint8_t *)"first.bin", 9u) < link.outgoing_len);
  ASSERT_TRUE(find_bytes(link.outgoing, link.outgoing_len, (const uint8_t *)"second.bin", 10u) < link.outgoing_len);
}

static void test_zmodem_receive_batch_sync_unit(void) {
  fake_link_t link;
  memory_sink_t sink;
  xyzm_link_ops_t link_ops;
  xyzm_sink_ops_t sink_ops;
  xyzm_zmodem_opts_t options;
  xyzm_zmodem_receive_request_t request;
  xyzm_transfer_report_t report;
  uint8_t hdr[4];
  uint8_t metadata[128];
  size_t metadata_len;
  xyzm_status_t status;

  memset(&link, 0, sizeof(link));
  memset(&sink, 0, sizeof(sink));
  memset(hdr, 0, sizeof(hdr));
  metadata_len = make_zmodem_metadata(metadata, sizeof(metadata), "first.bin", 3u);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(link.incoming, &link.incoming_len, metadata, metadata_len, TEST_ZCRCW, false, false);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(link.incoming, &link.incoming_len, (const uint8_t *)"ABC", 3u, TEST_ZCRCE, false, false);
  store_zmodem_offset(hdr, 3u);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZEOF, hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  metadata_len = make_zmodem_metadata(metadata, sizeof(metadata), "second.bin", 2u);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZFILE, hdr, false, false);
  append_zmodem_data_subpacket(link.incoming, &link.incoming_len, metadata, metadata_len, TEST_ZCRCW, false, false);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZDATA, hdr, false, false);
  append_zmodem_data_subpacket(link.incoming, &link.incoming_len, (const uint8_t *)"DE", 2u, TEST_ZCRCE, false, false);
  store_zmodem_offset(hdr, 2u);
  append_zmodem_binary_header(link.incoming, &link.incoming_len, TEST_ZEOF, hdr, false, false);
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(link.incoming, &link.incoming_len, TEST_ZFIN, hdr);
  link.incoming[link.incoming_len++] = 'O';
  link.incoming[link.incoming_len++] = 'O';
  link_ops.opaque = &link;
  link_ops.send = fake_send;
  link_ops.recv = fake_recv;
  link_ops.now_ms = fake_now_ms;
  sink_ops.opaque = &sink;
  sink_ops.begin = memory_sink_begin;
  sink_ops.write = memory_sink_write;
  sink_ops.end = memory_sink_end;
  options.handshake_timeout_ms = 1000u;
  options.block_timeout_ms = 1000u;
  options.retry_limit = 10u;
  options.pad_byte = 0x1au;
  options.zmodem_packet_len = 0u;
  options.zmodem_escape_ctrl = 0u;
  options.zmodem_use_crc32 = 0u;
  request.link = &link_ops;
  request.sink = &sink_ops;
  request.options = &options;
  request.observer = NULL;

  status = xyzm_zmodem_receive_batch(&request, &report);
  ASSERT_EQ_STATUS(status, XYZM_STATUS_OK);
  ASSERT_TRUE(report.completed_files == 2u);
  ASSERT_TRUE(report.payload_bytes == 5u);
  ASSERT_EQ_SIZE(sink.data_len, 5u);
  ASSERT_TRUE(memcmp(sink.data, "ABCDE", 5u) == 0);
}

static void test_zmodem_send_crc16_packet_len_sync_unit(void) {
  test_zmodem_send_crc16_packet_len();
}

static void test_zmodem_send_escape_option_sync_unit(void) {
  test_zmodem_send_escape_option();
}

static void test_zmodem_send_telnet_escape_sync_unit(void) {
  test_zmodem_send_telnet_escape();
}

static void test_zmodem_packet_boundary_sync_unit(void) {
  test_zmodem_packet_boundary();
}

static void test_zmodem_send_single_byte_sync_unit(void) {
  test_zmodem_send_single_byte();
}

static void test_zmodem_send_single_byte_without_report_sync_unit(void) {
  test_zmodem_send_single_byte_without_report();
}

static void test_zmodem_send_remote_skip_sync_unit(void) {
  test_zmodem_send_remote_skip();
}

static void test_zmodem_send_resume_from_requested_offset_sync_unit(void) {
  test_zmodem_send_resume_from_requested_offset();
}

static void test_zmodem_send_midstream_zrpos_sync_unit(void) {
  test_zmodem_send_midstream_zrpos();
}

static void test_zmodem_send_resume_requires_seek_sync_unit(void) {
  test_zmodem_send_resume_requires_seek();
}

static void test_zmodem_send_resume_seek_failure_sync_unit(void) {
  test_zmodem_send_resume_seek_failure();
}

static void test_zmodem_receive_bad_header_crc_sync_unit(void) {
  test_zmodem_receive_bad_header_crc();
}

static void test_zmodem_receive_size_mismatch_sync_unit(void) {
  test_zmodem_receive_size_mismatch();
}

static void test_zmodem_receive_extra_data_after_zero_size_sync_unit(void) {
  test_zmodem_receive_extra_data_after_zero_size();
}

static void test_zmodem_receive_ignores_flow_control_sync_unit(void) {
  test_zmodem_receive_ignores_flow_control();
}

static void test_zmodem_receive_multiple_data_subpackets_sync_unit(void) {
  test_zmodem_receive_multiple_data_subpackets();
}

static void test_zmodem_receive_resume_from_sink_offset_sync_unit(void) {
  test_zmodem_receive_resume_from_sink_offset();
}

static void test_zmodem_receive_resume_already_complete_sync_unit(void) {
  test_zmodem_receive_resume_already_complete();
}

static void test_zmodem_receive_resume_rejects_oversized_offset_sync_unit(void) {
  test_zmodem_receive_resume_rejects_oversized_offset();
}

static void test_zmodem_receive_timeout_sync_unit(void) {
  test_zmodem_receive_timeout();
}

const test_case_t xyzm_zmodem_unit_tests[] = {
  TEST_CASE(test_zmodem_send_crc16_packet_len_sync_unit, "Synchronous ZMODEM send respects CRC16 mode and negotiated packet length."),
  TEST_CASE(test_zmodem_send_escape_option_sync_unit, "Synchronous ZMODEM send escapes extra control bytes when ESCCTL is enabled."),
  TEST_CASE(test_zmodem_send_telnet_escape_sync_unit, "Synchronous ZMODEM send escapes carriage return after '@' for telnet-sensitive paths."),
  TEST_CASE(test_zmodem_packet_boundary_sync_unit, "Synchronous ZMODEM subpacket framing changes at the configured packet boundary."),
  TEST_CASE(test_zmodem_send_single_byte_sync_unit, "Synchronous ZMODEM send completes a one-byte payload and emits matching ZEOF."),
  TEST_CASE(test_zmodem_send_single_byte_without_report_sync_unit, "Synchronous ZMODEM send remains correct even when transfer report is NULL."),
  TEST_CASE(test_zmodem_send_remote_skip_sync_unit, "Synchronous ZMODEM send stops when the peer skips the offered file."),
  TEST_CASE(test_zmodem_send_resume_from_requested_offset_sync_unit, "Synchronous ZMODEM send resumes from the receiver-requested offset."),
  TEST_CASE(test_zmodem_send_midstream_zrpos_sync_unit, "Synchronous ZMODEM send handles ZRPOS while a payload subpacket is still being written."),
  TEST_CASE(test_zmodem_send_resume_requires_seek_sync_unit, "Synchronous ZMODEM send rejects non-zero ZRPOS when the source cannot seek."),
  TEST_CASE(test_zmodem_send_resume_seek_failure_sync_unit, "Synchronous ZMODEM send reports source seek failures during resume."),
  TEST_CASE(test_zmodem_receive_bad_header_crc_sync_unit, "Synchronous ZMODEM receive rejects a header with a corrupted CRC."),
  TEST_CASE(test_zmodem_receive_size_mismatch_sync_unit, "Synchronous ZMODEM receive reports protocol error when ZEOF arrives before the declared size."),
  TEST_CASE(test_zmodem_receive_extra_data_after_zero_size_sync_unit, "Synchronous ZMODEM receive rejects payload that arrives after a declared zero size."),
  TEST_CASE(test_zmodem_receive_ignores_flow_control_sync_unit, "Synchronous ZMODEM receive ignores injected XON/XOFF bytes on the wire."),
  TEST_CASE(test_zmodem_receive_multiple_data_subpackets_sync_unit, "Synchronous ZMODEM receive preserves consecutive subpackets within one ZDATA frame."),
  TEST_CASE(test_zmodem_receive_resume_from_sink_offset_sync_unit, "Synchronous ZMODEM receive resumes into an existing partial file."),
  TEST_CASE(test_zmodem_receive_resume_already_complete_sync_unit, "Synchronous ZMODEM receive accepts a resume offset that already completes the file."),
  TEST_CASE(test_zmodem_receive_resume_rejects_oversized_offset_sync_unit, "Synchronous ZMODEM receive rejects resume offsets beyond the announced file size."),
  TEST_CASE(test_zmodem_receive_timeout_sync_unit, "Synchronous ZMODEM receive times out during initial header exchange after retry exhaustion."),
  TEST_CASE(test_zmodem_send_batch_sync_unit, "Synchronous ZMODEM batch send enumerates multiple source files."),
  TEST_CASE(test_zmodem_receive_batch_sync_unit, "Synchronous ZMODEM batch receive accepts multiple ZFILE entries before ZFIN."),
};

const size_t xyzm_zmodem_unit_tests_count = sizeof(xyzm_zmodem_unit_tests) / sizeof(xyzm_zmodem_unit_tests[0]);
