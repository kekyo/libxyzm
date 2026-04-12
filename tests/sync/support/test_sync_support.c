/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "test_sync_support.h"
#include "../../../src/xyzm_sync_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
static void test_win32_debug_log(const char *fmt, ...) {
  const char *log_path;
  FILE *stream;
  va_list args;

  log_path = getenv("XYZM_WINE_FOREIGN_CLIENT_LOG");
  if (log_path == NULL || log_path[0] == '\0') {
    return;
  }
  stream = fopen(log_path, "a");
  if (stream == NULL) {
    return;
  }
  va_start(args, fmt);
  vfprintf(stream, fmt, args);
  va_end(args);
  fputc('\n', stream);
  fclose(stream);
}

static void test_win32_winsock_ensure_started(void) {
  static bool initialized;

  if (!initialized) {
    WSADATA wsa_data;

    ASSERT_TRUE(WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0);
    initialized = true;
  }
}

static SOCKET test_win32_socket_from_fd(intptr_t fd) {
  return (SOCKET)(uintptr_t)fd;
}

static void test_win32_close_socket_fd(intptr_t fd) {
  SOCKET socket_fd;

  if (fd == 0) {
    return;
  }
  socket_fd = test_win32_socket_from_fd(fd);
  (void)shutdown(socket_fd, SD_BOTH);
  (void)closesocket(socket_fd);
}

static void test_win32_send_all(
    SOCKET socket_fd,
    const void *buf,
    size_t len) {
  const char *cursor;
  size_t offset;

  cursor = buf;
  offset = 0u;
  while (offset < len) {
    int sent_len;

    sent_len = send(socket_fd, cursor + offset, (int)(len - offset), 0);
    ASSERT_TRUE(sent_len > 0);
    offset += (size_t)sent_len;
  }
}

static void test_win32_recv_all(
    SOCKET socket_fd,
    void *buf,
    size_t len) {
  char *cursor;
  size_t offset;

  cursor = buf;
  offset = 0u;
  while (offset < len) {
    int read_len;

    read_len = recv(socket_fd, cursor + offset, (int)(len - offset), 0);
    ASSERT_TRUE(read_len > 0);
    offset += (size_t)read_len;
  }
}

static void test_win32_send_u32(
    SOCKET socket_fd,
    uint32_t value) {
  uint32_t wire_value;

  wire_value = htonl(value);
  test_win32_send_all(socket_fd, &wire_value, sizeof(wire_value));
}

static uint32_t test_win32_recv_u32(
    SOCKET socket_fd) {
  uint32_t wire_value;

  test_win32_recv_all(socket_fd, &wire_value, sizeof(wire_value));
  return ntohl(wire_value);
}

static SOCKET test_win32_connect_bridge_socket(
    uint16_t port) {
  SOCKET socket_fd;
  struct sockaddr_in address;

  test_win32_winsock_ensure_started();
  socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_TRUE(socket_fd != INVALID_SOCKET);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_TRUE(connect(
      socket_fd,
      (const struct sockaddr *)&address,
      (int)sizeof(address)) == 0);
  return socket_fd;
}

static uint16_t test_win32_bridge_port(void) {
  const char *value;
  char *end;
  unsigned long port;

  value = getenv("XYZM_WINE_FOREIGN_BRIDGE_PORT");
  ASSERT_TRUE(value != NULL && value[0] != '\0');
  port = strtoul(value, &end, 10);
  ASSERT_TRUE(*end == '\0');
  ASSERT_TRUE(port > 0u && port <= 65535u);
  return (uint16_t)port;
}

static const char *test_win32_temp_root(void) {
  const char *value;

  value = getenv("XYZM_WINE_TMP_ROOT");
  return (value != NULL && value[0] != '\0') ? value : NULL;
}

static void format_local_timestamp(
    SYSTEMTIME timestamp,
    char *buf,
    size_t capacity) {
  ASSERT_TRUE(snprintf(
      buf,
      capacity,
      "%04u-%02u-%02u %02u:%02u:%02u.%03u",
      (unsigned)timestamp.wYear,
      (unsigned)timestamp.wMonth,
      (unsigned)timestamp.wDay,
      (unsigned)timestamp.wHour,
      (unsigned)timestamp.wMinute,
      (unsigned)timestamp.wSecond,
      (unsigned)timestamp.wMilliseconds) > 0);
}

static uint64_t test_monotonic_now_ms(void) {
  return (uint64_t)GetTickCount64();
}

static void test_get_local_time(
    SYSTEMTIME *timestamp) {
  GetLocalTime(timestamp);
}

static void append_test_verdict(
    const char *log_path,
    DWORD exit_code,
    SYSTEMTIME finished_at,
    uint64_t elapsed_ms) {
  FILE *stream;
  char finished_at_text[40];

  stream = fopen(log_path, "a");
  if (stream == NULL) {
    return;
  }
  format_local_timestamp(finished_at, finished_at_text, sizeof(finished_at_text));
  if (exit_code == 0u) {
    fprintf(stream, "result: PASS\n");
  } else {
    fprintf(stream, "result: FAIL\n");
    fprintf(stream, "exit_status: %lu\n", (unsigned long)exit_code);
  }
  fprintf(stream, "finished_at: %s\n", finished_at_text);
  fprintf(stream, "duration_ms: %llu\n", (unsigned long long)elapsed_ms);
  fclose(stream);
}
#else
static void format_local_timestamp(
    struct timespec timestamp,
    char *buf,
    size_t capacity) {
  struct tm tm_value;
  char base[32];

  ASSERT_TRUE(localtime_r(&timestamp.tv_sec, &tm_value) != NULL);
  ASSERT_TRUE(strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm_value) > 0u);
  ASSERT_TRUE(snprintf(
      buf,
      capacity,
      "%s.%03ld",
      base,
      timestamp.tv_nsec / 1000000L) > 0);
}

static void append_test_verdict(
    const char *log_path,
    int status,
    struct timespec finished_at,
    uint64_t elapsed_ms) {
  FILE *stream;
  char finished_at_text[40];

  stream = fopen(log_path, "a");
  if (stream == NULL) {
    return;
  }
  format_local_timestamp(finished_at, finished_at_text, sizeof(finished_at_text));
  if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
    fprintf(stream, "result: PASS\n");
  } else if (WIFEXITED(status)) {
    fprintf(stream, "result: FAIL\n");
    fprintf(stream, "exit_status: %d\n", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    fprintf(stream, "result: FAIL\n");
    fprintf(stream, "signal: %d\n", WTERMSIG(status));
  } else {
    fprintf(stream, "result: FAIL\n");
    fprintf(stream, "exit_status: unknown\n");
  }
  fprintf(stream, "finished_at: %s\n", finished_at_text);
  fprintf(stream, "duration_ms: %llu\n", (unsigned long long)elapsed_ms);
  fclose(stream);
}
#endif

#if !defined(_WIN32)
static uint64_t test_monotonic_now_ms(void) {
  struct timespec ts;

  ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return ((uint64_t)ts.tv_sec * 1000u) + (uint64_t)(ts.tv_nsec / 1000000u);
}
#endif

void test_logf(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

void test_create_results_dir(char *path, size_t capacity) {
#if defined(_WIN32)
  SYSTEMTIME now;
  char stamp[32];
  int rc;

  rc = _mkdir("test_results");
  ASSERT_TRUE(rc == 0 || errno == EEXIST);
  test_get_local_time(&now);
  ASSERT_TRUE(snprintf(
      stamp,
      sizeof(stamp),
      "%04u%02u%02u_%02u%02u%02u",
      (unsigned)now.wYear,
      (unsigned)now.wMonth,
      (unsigned)now.wDay,
      (unsigned)now.wHour,
      (unsigned)now.wMinute,
      (unsigned)now.wSecond) > 0);
  ASSERT_TRUE(snprintf(
      path,
      capacity,
      "test_results/%s_%03u",
      stamp,
      (unsigned)now.wMilliseconds) > 0);
  ASSERT_TRUE(CreateDirectoryA(path, NULL) != 0);
#else
  struct timespec ts;
  struct tm tm_value;
  char stamp[32];

  ASSERT_TRUE(mkdir("test_results", 0777) == 0 || errno == EEXIST);
  ASSERT_TRUE(clock_gettime(CLOCK_REALTIME, &ts) == 0);
  ASSERT_TRUE(localtime_r(&ts.tv_sec, &tm_value) != NULL);
  ASSERT_TRUE(strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_value) > 0u);
  ASSERT_TRUE(snprintf(
      path,
      capacity,
      "test_results/%s_%03ld",
      stamp,
      ts.tv_nsec / 1000000L) > 0);
  ASSERT_TRUE(mkdir(path, 0777) == 0);
#endif
}

int test_run_cases(
    const char *results_dir,
    const test_case_t *cases,
    size_t count,
    unsigned *passed,
    unsigned *failed) {
  size_t index;

  for (index = 0u; index < count; ++index) {
    char log_path[1024];
#if defined(_WIN32)
    SYSTEMTIME started_wall_at;
    SYSTEMTIME finished_wall_at;
    char started_at_text[40];
    char exe_path[1024];
    char command[2048];
    FILE *stream;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    HANDLE log_handle;
    DWORD wait_status;
    DWORD exit_code;
    uint64_t started_ms;
    uint64_t finished_ms;
#else
    struct timespec started_at;
    struct timespec started_wall_at;
    struct timespec finished_at;
    struct timespec finished_wall_at;
    char started_at_text[40];
    pid_t pid;
    int status;
#endif

    ASSERT_TRUE(snprintf(
        log_path,
        sizeof(log_path),
        "%s/%s.txt",
        results_dir,
        cases[index].name) > 0);
#if defined(_WIN32)
    started_ms = test_monotonic_now_ms();
    test_get_local_time(&started_wall_at);
    format_local_timestamp(started_wall_at, started_at_text, sizeof(started_at_text));
    stream = fopen(log_path, "w");
    ASSERT_TRUE(stream != NULL);
    fprintf(stream, "test_name: %s\n", cases[index].name);
    fprintf(stream, "description: %s\n", cases[index].description);
    fprintf(stream, "started_at: %s\n", started_at_text);
    ASSERT_TRUE(fclose(stream) == 0);

    ASSERT_TRUE(GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof(exe_path)) > 0u);
    ASSERT_TRUE(snprintf(
        command,
        sizeof(command),
        "\"%s\" --case %s",
        exe_path,
        cases[index].name) > 0);

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    log_handle = CreateFileA(
        log_path,
        FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    ASSERT_TRUE(log_handle != INVALID_HANDLE_VALUE);

    memset(&startup_info, 0, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = log_handle;
    startup_info.hStdError = log_handle;
    memset(&process_info, 0, sizeof(process_info));
    ASSERT_TRUE(CreateProcessA(
        NULL,
        command,
        NULL,
        NULL,
        TRUE,
        0u,
        NULL,
        NULL,
        &startup_info,
        &process_info) != 0);

    wait_status = WaitForSingleObject(process_info.hProcess, INFINITE);
    ASSERT_TRUE(wait_status == WAIT_OBJECT_0);
    ASSERT_TRUE(GetExitCodeProcess(process_info.hProcess, &exit_code) != 0);
    finished_ms = test_monotonic_now_ms();
    test_get_local_time(&finished_wall_at);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(log_handle);
    append_test_verdict(
        log_path,
        exit_code,
        finished_wall_at,
        finished_ms - started_ms);
    if (exit_code == 0u) {
      *passed += 1u;
      fprintf(stdout, "[PASS] %s\n", cases[index].name);
      continue;
    }
    *failed += 1u;
    fprintf(stdout, "[FAIL] %s\n", cases[index].name);
#else
    ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &started_at) == 0);
    ASSERT_TRUE(clock_gettime(CLOCK_REALTIME, &started_wall_at) == 0);
    format_local_timestamp(started_wall_at, started_at_text, sizeof(started_at_text));
    fflush(NULL);
    pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
      int fd;

      fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
      ASSERT_TRUE(fd >= 0);
      ASSERT_TRUE(dup2(fd, STDOUT_FILENO) >= 0);
      ASSERT_TRUE(dup2(fd, STDERR_FILENO) >= 0);
      close(fd);
      setvbuf(stdout, NULL, _IOLBF, 0);
      setvbuf(stderr, NULL, _IOLBF, 0);
      test_logf("test_name: %s", cases[index].name);
      test_logf("description: %s", cases[index].description);
      test_logf("started_at: %s", started_at_text);
      cases[index].fn();
      _exit(0);
    }

    ASSERT_TRUE(waitpid(pid, &status, 0) == pid);
    ASSERT_TRUE(clock_gettime(CLOCK_MONOTONIC, &finished_at) == 0);
    ASSERT_TRUE(clock_gettime(CLOCK_REALTIME, &finished_wall_at) == 0);
    append_test_verdict(
        log_path,
        status,
        finished_wall_at,
        ((uint64_t)(finished_at.tv_sec - started_at.tv_sec) * 1000u) +
            (uint64_t)((finished_at.tv_nsec - started_at.tv_nsec) / 1000000L));
    if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
      *passed += 1u;
      fprintf(stdout, "[PASS] %s\n", cases[index].name);
      continue;
    }
    *failed += 1u;
    fprintf(stdout, "[FAIL] %s\n", cases[index].name);
#endif
  }
  return *failed == 0u ? 0 : 1;
}

static xyzm_xmodem_checksum_mode_t test_xmodem_checksum_mode(test_sync_protocol_t protocol) {
  switch (protocol) {
    case TEST_SYNC_PROTOCOL_XMODEM:
    case TEST_SYNC_PROTOCOL_XMODEM_1K:
      return XYZM_XMODEM_CHECKSUM_MODE_CHECKSUM;
    case TEST_SYNC_PROTOCOL_XMODEM_CRC:
    case TEST_SYNC_PROTOCOL_XMODEM_1K_CRC:
      return XYZM_XMODEM_CHECKSUM_MODE_CRC;
    case TEST_SYNC_PROTOCOL_XMODEM_AUTO:
      return XYZM_XMODEM_CHECKSUM_MODE_AUTO;
    default:
      return 0;
  }
}

static xyzm_xmodem_packet_size_t test_xmodem_packet_size(test_sync_protocol_t protocol) {
  switch (protocol) {
    case TEST_SYNC_PROTOCOL_XMODEM:
    case TEST_SYNC_PROTOCOL_XMODEM_CRC:
    case TEST_SYNC_PROTOCOL_XMODEM_AUTO:
      return XYZM_XMODEM_PACKET_SIZE_128;
    case TEST_SYNC_PROTOCOL_XMODEM_1K:
    case TEST_SYNC_PROTOCOL_XMODEM_1K_CRC:
      return XYZM_XMODEM_PACKET_SIZE_1K;
    default:
      return 0;
  }
}

static xyzm_ymodem_variant_t test_ymodem_variant(test_sync_protocol_t protocol) {
  switch (protocol) {
    case TEST_SYNC_PROTOCOL_YMODEM:
      return XYZM_YMODEM_VARIANT_STANDARD;
    case TEST_SYNC_PROTOCOL_YMODEM_G:
      return XYZM_YMODEM_VARIANT_G;
    case TEST_SYNC_PROTOCOL_YMODEM_AUTO:
      return XYZM_YMODEM_VARIANT_AUTO;
    default:
      return 0;
  }
}

static xyzm_status_t test_sync_support_send_observed(
    const test_sync_send_request_t *request,
    const xyzm_transfer_observer_t *observer,
    xyzm_transfer_report_t *report) {
  xyzm_xmodem_send_opts_t xmodem_options;
  xyzm_ymodem_opts_t ymodem_options;
  xyzm_zmodem_opts_t zmodem_options;

  xyzm_report_init(report);
  if ((request == NULL) || (request->options == NULL)) {
    if (report != NULL) {
      report->final_status = XYZM_STATUS_ARGUMENT_ERROR;
    }
    return XYZM_STATUS_ARGUMENT_ERROR;
  }

  switch (request->options->protocol) {
    case TEST_SYNC_PROTOCOL_XMODEM:
    case TEST_SYNC_PROTOCOL_XMODEM_1K:
    case TEST_SYNC_PROTOCOL_XMODEM_CRC:
    case TEST_SYNC_PROTOCOL_XMODEM_AUTO:
    case TEST_SYNC_PROTOCOL_XMODEM_1K_CRC: {
      xyzm_xmodem_send_request_t typed_request;

      xmodem_options.handshake_timeout_ms = request->options->handshake_timeout_ms;
      xmodem_options.block_timeout_ms = request->options->block_timeout_ms;
      xmodem_options.retry_limit = request->options->retry_limit;
      xmodem_options.pad_byte = request->options->pad_byte;
      xmodem_options.checksum_mode = test_xmodem_checksum_mode(request->options->protocol);
      xmodem_options.packet_size = test_xmodem_packet_size(request->options->protocol);

      typed_request.link = request->link;
      typed_request.source = request->source;
      typed_request.options = &xmodem_options;
      typed_request.observer = observer;
      return xyzm_xmodem_send(&typed_request, report);
    }
    case TEST_SYNC_PROTOCOL_YMODEM:
    case TEST_SYNC_PROTOCOL_YMODEM_G:
    case TEST_SYNC_PROTOCOL_YMODEM_AUTO: {
      xyzm_ymodem_send_request_t typed_request;

      ymodem_options.handshake_timeout_ms = request->options->handshake_timeout_ms;
      ymodem_options.block_timeout_ms = request->options->block_timeout_ms;
      ymodem_options.retry_limit = request->options->retry_limit;
      ymodem_options.pad_byte = request->options->pad_byte;
      ymodem_options.variant = test_ymodem_variant(request->options->protocol);

      typed_request.link = request->link;
      typed_request.source = request->source;
      typed_request.options = &ymodem_options;
      typed_request.observer = observer;
      return xyzm_ymodem_send(&typed_request, report);
    }
    case TEST_SYNC_PROTOCOL_ZMODEM: {
      xyzm_zmodem_send_request_t typed_request;

      zmodem_options.handshake_timeout_ms = request->options->handshake_timeout_ms;
      zmodem_options.block_timeout_ms = request->options->block_timeout_ms;
      zmodem_options.retry_limit = request->options->retry_limit;
      zmodem_options.pad_byte = request->options->pad_byte;
      zmodem_options.zmodem_packet_len = request->options->zmodem_packet_len;
      zmodem_options.zmodem_escape_ctrl = request->options->zmodem_escape_ctrl;
      zmodem_options.zmodem_use_crc32 = request->options->zmodem_use_crc32;

      typed_request.link = request->link;
      typed_request.source = request->source;
      typed_request.options = &zmodem_options;
      typed_request.observer = observer;
      return xyzm_zmodem_send(&typed_request, report);
    }
    default:
      if (report != NULL) {
        report->final_status = XYZM_STATUS_UNSUPPORTED;
      }
      return XYZM_STATUS_UNSUPPORTED;
  }
}

static xyzm_status_t test_sync_support_receive_observed(
    const test_sync_receive_request_t *request,
    const xyzm_transfer_observer_t *observer,
    xyzm_transfer_report_t *report) {
  xyzm_xmodem_receive_opts_t xmodem_options;
  xyzm_ymodem_opts_t ymodem_options;
  xyzm_zmodem_opts_t zmodem_options;

  xyzm_report_init(report);
  if ((request == NULL) || (request->options == NULL)) {
    if (report != NULL) {
      report->final_status = XYZM_STATUS_ARGUMENT_ERROR;
    }
    return XYZM_STATUS_ARGUMENT_ERROR;
  }

  switch (request->options->protocol) {
    case TEST_SYNC_PROTOCOL_XMODEM:
    case TEST_SYNC_PROTOCOL_XMODEM_1K:
    case TEST_SYNC_PROTOCOL_XMODEM_CRC:
    case TEST_SYNC_PROTOCOL_XMODEM_1K_CRC: {
      xyzm_xmodem_receive_request_t typed_request;

      xmodem_options.handshake_timeout_ms = request->options->handshake_timeout_ms;
      xmodem_options.block_timeout_ms = request->options->block_timeout_ms;
      xmodem_options.retry_limit = request->options->retry_limit;
      xmodem_options.pad_byte = request->options->pad_byte;
      xmodem_options.checksum_mode = test_xmodem_checksum_mode(request->options->protocol);

      typed_request.link = request->link;
      typed_request.sink = request->sink;
      typed_request.options = &xmodem_options;
      typed_request.observer = observer;
      return xyzm_xmodem_receive(&typed_request, report);
    }
    case TEST_SYNC_PROTOCOL_YMODEM:
    case TEST_SYNC_PROTOCOL_YMODEM_G:
    case TEST_SYNC_PROTOCOL_YMODEM_AUTO: {
      xyzm_ymodem_receive_request_t typed_request;

      ymodem_options.handshake_timeout_ms = request->options->handshake_timeout_ms;
      ymodem_options.block_timeout_ms = request->options->block_timeout_ms;
      ymodem_options.retry_limit = request->options->retry_limit;
      ymodem_options.pad_byte = request->options->pad_byte;
      ymodem_options.variant = test_ymodem_variant(request->options->protocol);

      typed_request.link = request->link;
      typed_request.sink = request->sink;
      typed_request.options = &ymodem_options;
      typed_request.observer = observer;
      return xyzm_ymodem_receive(&typed_request, report);
    }
    case TEST_SYNC_PROTOCOL_ZMODEM: {
      xyzm_zmodem_receive_request_t typed_request;

      zmodem_options.handshake_timeout_ms = request->options->handshake_timeout_ms;
      zmodem_options.block_timeout_ms = request->options->block_timeout_ms;
      zmodem_options.retry_limit = request->options->retry_limit;
      zmodem_options.pad_byte = request->options->pad_byte;
      zmodem_options.zmodem_packet_len = request->options->zmodem_packet_len;
      zmodem_options.zmodem_escape_ctrl = request->options->zmodem_escape_ctrl;
      zmodem_options.zmodem_use_crc32 = request->options->zmodem_use_crc32;

      typed_request.link = request->link;
      typed_request.sink = request->sink;
      typed_request.options = &zmodem_options;
      typed_request.observer = observer;
      return xyzm_zmodem_receive(&typed_request, report);
    }
    default:
      if (report != NULL) {
        report->final_status = XYZM_STATUS_UNSUPPORTED;
      }
      return XYZM_STATUS_UNSUPPORTED;
  }
}

xyzm_status_t test_sync_send(
    const test_sync_send_request_t *request,
    xyzm_transfer_report_t *report) {
  return test_sync_support_send_observed(request, NULL, report);
}

xyzm_status_t test_sync_send_observed(
    const test_sync_send_request_t *request,
    const xyzm_transfer_observer_t *observer,
    xyzm_transfer_report_t *report) {
  return test_sync_support_send_observed(request, observer, report);
}

xyzm_status_t test_sync_receive(
    const test_sync_receive_request_t *request,
    xyzm_transfer_report_t *report) {
  return test_sync_support_receive_observed(request, NULL, report);
}

xyzm_status_t test_sync_receive_observed(
    const test_sync_receive_request_t *request,
    const xyzm_transfer_observer_t *observer,
    xyzm_transfer_report_t *report) {
  return test_sync_support_receive_observed(request, observer, report);
}

void fail_now(const char *file, int line, const char *fmt, ...) {
  va_list args;

  fprintf(stderr, "%s:%d: ", file, line);
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
  fflush(stderr);
  _exit(1);
}

xyzm_status_t fake_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  fake_link_t *link;

  (void)timeout_ms;
  link = opaque;
  ASSERT_TRUE((link->outgoing_len + len) <= sizeof(link->outgoing));
  memcpy(link->outgoing + link->outgoing_len, buf, len);
  link->outgoing_len += len;
  *written_len = len;
  link->now_ms += 1u;
  return XYZM_STATUS_OK;
}

xyzm_status_t fake_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  fake_link_t *link;
  size_t available_len;
  size_t copy_len;

  link = opaque;
  if (link->timeout_recv_count > 0u) {
    link->timeout_recv_count -= 1u;
    link->now_ms += timeout_ms;
    *read_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
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

uint64_t fake_now_ms(void *opaque) {
  fake_link_t *link;

  link = opaque;
  return link->now_ms;
}

xyzm_status_t timed_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
  timed_link_t *link;

  (void)timeout_ms;
  link = opaque;
  ASSERT_TRUE((link->outgoing_len + len) <= link->outgoing_capacity);
  memcpy(link->outgoing + link->outgoing_len, buf, len);
  link->outgoing_len += len;
  link->send_calls += 1u;
  link->now_us += (uint64_t)link->send_base_us + ((uint64_t)len * link->send_byte_us);
  *written_len = len;
  return XYZM_STATUS_OK;
}

xyzm_status_t timed_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
  timed_link_t *link;
  size_t available_len;
  size_t copy_len;

  link = opaque;
  if (capacity > link->max_recv_capacity) {
    link->max_recv_capacity = capacity;
  }
  available_len = link->incoming_len - link->incoming_pos;
  if (available_len == 0u) {
    link->now_us += (uint64_t)timeout_ms * 1000u;
    *read_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
  copy_len = (available_len < capacity) ? available_len : capacity;
  memcpy(buf, link->incoming + link->incoming_pos, copy_len);
  link->incoming_pos += copy_len;
  link->recv_calls += 1u;
  link->now_us += (uint64_t)link->recv_base_us + ((uint64_t)copy_len * link->recv_byte_us);
  *read_len = copy_len;
  return XYZM_STATUS_OK;
}

uint64_t timed_now_ms(void *opaque) {
  timed_link_t *link;

  link = opaque;
  return link->now_us / 1000u;
}

xyzm_status_t memory_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  memory_source_t *source;

  source = opaque;
  if (source->served) {
    return XYZM_STATUS_END;
  }
  source->served = true;
  *info = source->info;
  *file_opaque = source;
  return XYZM_STATUS_OK;
}

xyzm_status_t memory_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  memory_source_t *source;
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

xyzm_status_t memory_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset) {
  memory_source_t *source;

  (void)opaque;
  source = file_opaque;
  if (source->seek_result != XYZM_STATUS_OK) {
    return source->seek_result;
  }
  if (offset > source->data_len) {
    return XYZM_STATUS_IO_ERROR;
  }
  source->offset = (size_t)offset;
  return XYZM_STATUS_OK;
}

xyzm_status_t memory_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  memory_source_t *source;

  (void)opaque;
  (void)file_opaque;
  (void)info;
  source = file_opaque;
  source->end_result = result;
  return XYZM_STATUS_OK;
}

xyzm_status_t memory_sink_begin(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque) {
  memory_sink_t *sink;

  sink = opaque;
  sink->info = *info;
  sink->began = true;
  if (resume_offset != NULL) {
    *resume_offset = sink->resume_offset;
  }
  *file_opaque = sink;
  return XYZM_STATUS_OK;
}

xyzm_status_t memory_sink_write(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len) {
  memory_sink_t *sink;

  (void)opaque;
  sink = file_opaque;
  ASSERT_TRUE((sink->data_len + len) <= sizeof(sink->data));
  memcpy(sink->data + sink->data_len, buf, len);
  sink->data_len += len;
  return XYZM_STATUS_OK;
}

xyzm_status_t memory_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  memory_sink_t *sink;

  (void)opaque;
  sink = file_opaque;
  sink->info = *info;
  sink->end_result = result;
  return XYZM_STATUS_OK;
}

xyzm_status_t fd_send(
    void *opaque,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *written_len) {
#if defined(_WIN32)
  fd_link_t *link;
  SOCKET socket_fd;
  fd_set write_fds;
  TIMEVAL timeout;
  int ready_count;
  int sent_len;

  link = opaque;
  socket_fd = test_win32_socket_from_fd(link->fd);
  FD_ZERO(&write_fds);
  FD_SET(socket_fd, &write_fds);
  timeout.tv_sec = (long)(timeout_ms / 1000u);
  timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
  ready_count = select(0, NULL, &write_fds, NULL, &timeout);
  if (ready_count == 0) {
    *written_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
  if (ready_count < 0) {
    *written_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  sent_len = send(socket_fd, (const char *)buf, (int)len, 0);
  if (sent_len <= 0) {
    *written_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  *written_len = (size_t)sent_len;
  return XYZM_STATUS_OK;
#else
  fd_link_t *link;
  struct pollfd pfd;
  int poll_result;
  ssize_t io_result;

  link = opaque;
  pfd.fd = link->fd;
  pfd.events = POLLOUT;
  poll_result = poll(&pfd, 1, (int)timeout_ms);
  if (poll_result == 0) {
    *written_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
  if (poll_result < 0) {
    *written_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  #ifdef MSG_NOSIGNAL
  io_result = send(link->fd, buf, len, MSG_NOSIGNAL);
  #else
  io_result = write(link->fd, buf, len);
  #endif
  if (io_result <= 0) {
    *written_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  *written_len = (size_t)io_result;
  return XYZM_STATUS_OK;
#endif
}

xyzm_status_t fd_recv(
    void *opaque,
    uint8_t *buf,
    size_t capacity,
    uint32_t timeout_ms,
    size_t *read_len) {
#if defined(_WIN32)
  fd_link_t *link;
  SOCKET socket_fd;
  fd_set read_fds;
  TIMEVAL timeout;
  int ready_count;
  int received_len;

  link = opaque;
  socket_fd = test_win32_socket_from_fd(link->fd);
  FD_ZERO(&read_fds);
  FD_SET(socket_fd, &read_fds);
  timeout.tv_sec = (long)(timeout_ms / 1000u);
  timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
  ready_count = select(0, &read_fds, NULL, NULL, &timeout);
  if (ready_count == 0) {
    *read_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
  if (ready_count < 0) {
    *read_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  received_len = recv(socket_fd, (char *)buf, (int)capacity, 0);
  if (received_len <= 0) {
    *read_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  *read_len = (size_t)received_len;
  return XYZM_STATUS_OK;
#else
  fd_link_t *link;
  struct pollfd pfd;
  int poll_result;
  ssize_t io_result;

  link = opaque;
  pfd.fd = link->fd;
  pfd.events = POLLIN;
  poll_result = poll(&pfd, 1, (int)timeout_ms);
  if (poll_result == 0) {
    *read_len = 0u;
    return XYZM_STATUS_TIMEOUT;
  }
  if (poll_result < 0) {
    *read_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  io_result = read(link->fd, buf, capacity);
  if (io_result <= 0) {
    *read_len = 0u;
    return XYZM_STATUS_IO_ERROR;
  }
  *read_len = (size_t)io_result;
  return XYZM_STATUS_OK;
#endif
}

uint64_t fd_now_ms(void *opaque) {
#if defined(_WIN32)
  (void)opaque;
  return test_monotonic_now_ms();
#else
  (void)opaque;
  return test_monotonic_now_ms();
#endif
}

size_t file_size_for_path(const char *path) {
  struct stat st;

  ASSERT_TRUE(stat(path, &st) == 0);
  return (size_t)st.st_size;
}

xyzm_status_t file_source_next(
    void *opaque,
    xyzm_file_info_t *info,
    void **file_opaque) {
  file_source_t *source;

  source = opaque;
  if (source->served) {
    return XYZM_STATUS_END;
  }
  source->stream = fopen(source->path, "rb");
  if (source->stream == NULL) {
    return XYZM_STATUS_IO_ERROR;
  }
  source->size = file_size_for_path(source->path);
  memset(info, 0, sizeof(*info));
  info->valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE;
  info->name = source->name;
  info->size_bytes = source->size;
  *file_opaque = source;
  source->served = true;
  return XYZM_STATUS_OK;
}

xyzm_status_t file_source_read(
    void *opaque,
    void *file_opaque,
    uint8_t *buf,
    size_t capacity,
    size_t *read_len) {
  file_source_t *source;

  (void)opaque;
  source = file_opaque;
  *read_len = fread(buf, 1, capacity, source->stream);
  if (*read_len > 0u) {
    return XYZM_STATUS_OK;
  }
  if (ferror(source->stream) != 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  return XYZM_STATUS_END;
}

xyzm_status_t file_source_seek(
    void *opaque,
    void *file_opaque,
    uint64_t offset) {
  file_source_t *source;
  long seek_offset;

  (void)opaque;
  source = file_opaque;
  if (offset > (uint64_t)LONG_MAX) {
    return XYZM_STATUS_IO_ERROR;
  }
  seek_offset = (long)offset;
  if (fseek(source->stream, seek_offset, SEEK_SET) != 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  return XYZM_STATUS_OK;
}

xyzm_status_t file_source_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  file_source_t *source;

  (void)opaque;
  (void)info;
  (void)result;
  source = file_opaque;
  if (source == NULL) {
    return XYZM_STATUS_OK;
  }
  if (source->stream != NULL) {
    fclose(source->stream);
    source->stream = NULL;
  }
  return XYZM_STATUS_OK;
}

xyzm_status_t file_sink_begin(
    void *opaque,
    const xyzm_file_info_t *info,
    uint64_t *resume_offset,
    void **file_opaque) {
  file_sink_t *sink;
  const char *name;
  int written_len;

  sink = opaque;
  if (resume_offset != NULL) {
    *resume_offset = 0u;
  }
  name = sink->fallback_name;
  if (((info->valid_mask & XYZM_FILE_INFO_NAME) != 0u) && (info->name != NULL) &&
      (info->name[0] != '\0')) {
    name = info->name;
  }
  written_len = snprintf(sink->path, sizeof(sink->path), "%s/%s", sink->directory, name);
  if ((written_len < 0) || ((size_t)written_len >= sizeof(sink->path))) {
    return XYZM_STATUS_IO_ERROR;
  }
  sink->stream = fopen(sink->path, "wb");
  if (sink->stream == NULL) {
    return XYZM_STATUS_IO_ERROR;
  }
  *file_opaque = sink;
  return XYZM_STATUS_OK;
}

xyzm_status_t file_sink_write(
    void *opaque,
    void *file_opaque,
    const uint8_t *buf,
    size_t len) {
  file_sink_t *sink;

  (void)opaque;
  sink = file_opaque;
  if (fwrite(buf, 1, len, sink->stream) != len) {
    return XYZM_STATUS_IO_ERROR;
  }
  return XYZM_STATUS_OK;
}

xyzm_status_t file_sink_end(
    void *opaque,
    void *file_opaque,
    const xyzm_file_info_t *info,
    xyzm_status_t result) {
  file_sink_t *sink;

  (void)opaque;
  (void)file_opaque;
  (void)info;
  (void)result;
  sink = file_opaque;
  if (sink->stream != NULL) {
    fclose(sink->stream);
    sink->stream = NULL;
  }
  return XYZM_STATUS_OK;
}

void append_xmodem_block(
    uint8_t *buffer,
    size_t *offset,
    uint8_t block_number,
    const uint8_t *payload,
    size_t payload_len,
    bool use_crc,
    uint8_t header) {
  uint16_t crc;

  buffer[(*offset)++] = header;
  buffer[(*offset)++] = block_number;
  buffer[(*offset)++] = (uint8_t)(255u - block_number);
  memcpy(buffer + *offset, payload, payload_len);
  *offset += payload_len;
  if (use_crc) {
    crc = xyzm_crc16_xmodem_update_buffer(0u, payload, payload_len);
    buffer[(*offset)++] = (uint8_t)(crc >> 8);
    buffer[(*offset)++] = (uint8_t)(crc & 0xffu);
  } else {
    buffer[(*offset)++] = xyzm_checksum8_update_buffer(0u, payload, payload_len);
  }
}

void remove_tree(const char *path) {
#if defined(_WIN32)
  WIN32_FIND_DATAA find_data;
  HANDLE find_handle;
  char pattern[1024];
  DWORD attrs;

  attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
    ASSERT_TRUE(DeleteFileA(path) != 0);
    return;
  }

  ASSERT_TRUE(snprintf(pattern, sizeof(pattern), "%s\\*", path) > 0);
  find_handle = FindFirstFileA(pattern, &find_data);
  if (find_handle != INVALID_HANDLE_VALUE) {
    do {
      char child_path[1024];

      if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
        continue;
      }
      ASSERT_TRUE(snprintf(
          child_path,
          sizeof(child_path),
          "%s\\%s",
          path,
          find_data.cFileName) > 0);
      remove_tree(child_path);
    } while (FindNextFileA(find_handle, &find_data) != 0);
    FindClose(find_handle);
  }
  ASSERT_TRUE(RemoveDirectoryA(path) != 0);
#else
  char command[1024];
  int result;

  result = snprintf(command, sizeof(command), "rm -rf '%s'", path);
  ASSERT_TRUE((result > 0) && ((size_t)result < sizeof(command)));
  result = system(command);
  ASSERT_TRUE(result == 0);
#endif
}

void write_file(const char *path, const uint8_t *data, size_t len) {
  FILE *stream;

  stream = fopen(path, "wb");
  ASSERT_TRUE(stream != NULL);
  ASSERT_TRUE(fwrite(data, 1, len, stream) == len);
  ASSERT_TRUE(fclose(stream) == 0);
}

size_t read_file(const char *path, uint8_t *buf, size_t capacity) {
  FILE *stream;
  size_t read_len;

  stream = fopen(path, "rb");
  ASSERT_TRUE(stream != NULL);
  read_len = fread(buf, 1, capacity, stream);
  ASSERT_TRUE(fclose(stream) == 0);
  return read_len;
}

void fill_pattern(uint8_t *buf, size_t len, uint8_t seed) {
  size_t index;

  for (index = 0u; index < len; ++index) {
    buf[index] = (uint8_t)(((index * 29u) + seed) ^ (index >> 3));
  }
}

uint8_t *read_file_alloc(const char *path, size_t *len) {
  uint8_t *buf;
  FILE *stream;

  *len = file_size_for_path(path);
  buf = malloc((*len == 0u) ? 1u : *len);
  ASSERT_TRUE(buf != NULL);
  stream = fopen(path, "rb");
  ASSERT_TRUE(stream != NULL);
  ASSERT_TRUE(fread(buf, 1, *len, stream) == *len);
  ASSERT_TRUE(fclose(stream) == 0);
  return buf;
}

size_t xmodem_1k_padded_size(size_t data_len) {
  size_t full_blocks;
  size_t remainder;

  full_blocks = data_len / 1024u;
  remainder = data_len % 1024u;
  if (remainder == 0u) {
    return full_blocks * 1024u;
  }
  return (full_blocks * 1024u) + ((remainder <= 128u) ? 128u : 1024u);
}

child_process_t spawn_child(char *const argv[], const char *working_directory) {
#if defined(_WIN32)
  child_process_t child;
  SOCKET control_socket;
  uint32_t argc;
  uint32_t index;
  uint32_t cwd_len;
  uint16_t bridge_port;
  uint16_t transport_port;
  uint16_t status_port;

  memset(&child, 0, sizeof(child));
  test_win32_debug_log("spawn_child: begin cwd=%s", working_directory);
  bridge_port = test_win32_bridge_port();
  control_socket = test_win32_connect_bridge_socket(bridge_port);
  test_win32_debug_log("spawn_child: connected control port=%u", (unsigned)bridge_port);
  test_win32_send_all(control_socket, "SPWN", 4u);
  test_win32_debug_log("spawn_child: sent magic");
  argc = 0u;
  while (argv[argc] != NULL) {
    argc += 1u;
  }
  cwd_len = working_directory != NULL ? (uint32_t)strlen(working_directory) : 0u;
  test_win32_debug_log(
      "spawn_child: argc=%lu cwd_len=%lu",
      (unsigned long)argc,
      (unsigned long)cwd_len);
  test_win32_send_u32(control_socket, cwd_len);
  test_win32_debug_log("spawn_child: sent cwd_len");
  test_win32_send_u32(control_socket, argc);
  test_win32_debug_log("spawn_child: sent argc");
  if (cwd_len > 0u) {
    test_win32_send_all(control_socket, working_directory, cwd_len);
    test_win32_debug_log("spawn_child: sent cwd");
  }
  for (index = 0u; index < argc; ++index) {
    uint32_t arg_len;

    arg_len = (uint32_t)strlen(argv[index]);
    test_win32_send_u32(control_socket, arg_len);
    test_win32_send_all(control_socket, argv[index], arg_len);
    test_win32_debug_log(
        "spawn_child: sent argv[%lu]=%s",
        (unsigned long)index,
        argv[index]);
  }
  transport_port = (uint16_t)test_win32_recv_u32(control_socket);
  status_port = (uint16_t)test_win32_recv_u32(control_socket);
  test_win32_debug_log(
      "spawn_child: received transport=%u status=%u",
      (unsigned)transport_port,
      (unsigned)status_port);
  ASSERT_TRUE(transport_port != 0u);
  ASSERT_TRUE(status_port != 0u);
  closesocket(control_socket);
  child.fd = (intptr_t)test_win32_connect_bridge_socket(transport_port);
  child.stderr_fd = (intptr_t)test_win32_connect_bridge_socket(status_port);
  test_win32_debug_log("spawn_child: connected transport/status");
  return child;
#else
  child_process_t child;
  int socket_pair[2];
  int stderr_pipe[2];
  pid_t pid;

  ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, socket_pair) == 0);
  ASSERT_TRUE(pipe(stderr_pipe) == 0);
  pid = fork();
  ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    close(socket_pair[0]);
    close(stderr_pipe[0]);
    ASSERT_TRUE(dup2(socket_pair[1], STDIN_FILENO) >= 0);
    ASSERT_TRUE(dup2(socket_pair[1], STDOUT_FILENO) >= 0);
    ASSERT_TRUE(dup2(stderr_pipe[1], STDERR_FILENO) >= 0);
    close(socket_pair[1]);
    close(stderr_pipe[1]);
    if (working_directory != NULL) {
      ASSERT_TRUE(chdir(working_directory) == 0);
    }
    execvp(argv[0], argv);
    _exit(127);
  }

  close(socket_pair[1]);
  close(stderr_pipe[1]);
  child.pid = pid;
  child.fd = socket_pair[0];
  child.stderr_fd = stderr_pipe[0];
  return child;
#endif
}

void read_child_stderr(int fd, char *buf, size_t capacity) {
#if defined(_WIN32)
  (void)fd;
  ASSERT_TRUE(capacity > 0u);
  buf[0] = '\0';
#else
  ssize_t read_len;

  read_len = read(fd, buf, capacity - 1u);
  if (read_len < 0) {
    buf[0] = '\0';
    return;
  }
  buf[read_len] = '\0';
#endif
}

void wait_child_ok(child_process_t *child) {
#if defined(_WIN32)
  SOCKET status_socket;
  uint32_t exit_code;
  uint32_t stderr_len;
  char *stderr_log;

  status_socket = test_win32_socket_from_fd(child->stderr_fd);
  exit_code = test_win32_recv_u32(status_socket);
  stderr_len = test_win32_recv_u32(status_socket);
  stderr_log = malloc((size_t)stderr_len + 1u);
  ASSERT_TRUE(stderr_log != NULL);
  if (stderr_len > 0u) {
    test_win32_recv_all(status_socket, stderr_log, stderr_len);
  }
  stderr_log[stderr_len] = '\0';
  test_win32_close_socket_fd(child->fd);
  test_win32_close_socket_fd(child->stderr_fd);
  child->fd = 0;
  child->stderr_fd = 0;
  if (exit_code != 0u) {
    fail_now(
        __FILE__,
        __LINE__,
        "child failed: status=%lu stderr=%s",
        (unsigned long)exit_code,
        stderr_log);
  }
  free(stderr_log);
#else
  int status;
  char stderr_log[2048];

  read_child_stderr(child->stderr_fd, stderr_log, sizeof(stderr_log));
  ASSERT_TRUE(waitpid(child->pid, &status, 0) == child->pid);
  close(child->fd);
  close(child->stderr_fd);
  if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
    fail_now(__FILE__, __LINE__, "child failed: status=%d stderr=%s", status, stderr_log);
  }
#endif
}

void populate_sync_default_options(test_sync_transfer_opts_t *options, test_sync_protocol_t protocol) {
  options->protocol = protocol;
  options->handshake_timeout_ms = 5000u;
  options->block_timeout_ms = 5000u;
  options->retry_limit = 8u;
  options->pad_byte = 0x1au;
  options->zmodem_packet_len = 0u;
  options->zmodem_escape_ctrl = 1u;
  options->zmodem_use_crc32 = 1u;
}

uint64_t bytes_per_second(uint64_t payload_bytes, uint64_t elapsed_us) {
  ASSERT_TRUE(elapsed_us > 0u);
  return (payload_bytes * 1000000u) / elapsed_us;
}

void append_repeated_byte(uint8_t *buf, size_t *used_len, uint8_t value, size_t count) {
  size_t index;

  for (index = 0u; index < count; ++index) {
    buf[(*used_len)++] = value;
  }
}

void fill_xmodem_speed_incoming(
    uint8_t *buf,
    size_t *used_len,
    uint8_t start_char,
    size_t block_count) {
  *used_len = 0u;
  buf[(*used_len)++] = start_char;
  append_repeated_byte(buf, used_len, XYZM_ACK, block_count);
  buf[(*used_len)++] = XYZM_NAK;
  buf[(*used_len)++] = XYZM_ACK;
}

void fill_ymodem_speed_incoming(
    uint8_t *buf,
    size_t *used_len,
    bool streaming,
    size_t block_count) {
  *used_len = 0u;
  buf[(*used_len)++] = streaming ? XYZM_G_REQ : XYZM_CRC_REQ;
  if (!streaming) {
    buf[(*used_len)++] = XYZM_ACK;
  }
  buf[(*used_len)++] = streaming ? XYZM_G_REQ : XYZM_CRC_REQ;
  if (!streaming) {
    append_repeated_byte(buf, used_len, XYZM_ACK, block_count);
    buf[(*used_len)++] = XYZM_NAK;
    buf[(*used_len)++] = XYZM_ACK;
    buf[(*used_len)++] = XYZM_CRC_REQ;
    buf[(*used_len)++] = XYZM_ACK;
  } else {
    buf[(*used_len)++] = XYZM_ACK;
    buf[(*used_len)++] = XYZM_G_REQ;
  }
}

void fill_zmodem_speed_incoming(uint8_t *buf, size_t *used_len) {
  uint8_t hdr[4];

  *used_len = 0u;
  memset(hdr, 0, sizeof(hdr));
  append_zmodem_hex_header(buf, used_len, TEST_ZRINIT, hdr);
  append_zmodem_hex_header(buf, used_len, TEST_ZRPOS, hdr);
  append_zmodem_hex_header(buf, used_len, TEST_ZRINIT, hdr);
  append_zmodem_hex_header(buf, used_len, TEST_ZFIN, hdr);
}

uint16_t test_zmodem_crc16_entry(uint8_t index) {
  uint16_t crc;
  unsigned bit;

  crc = (uint16_t)index << 8;
  for (bit = 0u; bit < 8u; ++bit) {
    if ((crc & 0x8000u) != 0u) {
      crc = (uint16_t)((crc << 1) ^ 0x1021u);
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

uint16_t test_zmodem_crc16_update(uint16_t crc, uint8_t value) {
  return (uint16_t)(
      test_zmodem_crc16_entry((uint8_t)(crc >> 8)) ^
      (uint16_t)(crc << 8) ^
      value);
}

uint32_t test_zmodem_crc32_update(uint32_t crc, uint8_t value) {
  unsigned bit;

  crc ^= value;
  for (bit = 0u; bit < 8u; ++bit) {
    if ((crc & 1u) != 0u) {
      crc = (crc >> 1) ^ 0xedb88320u;
    } else {
      crc >>= 1;
    }
  }
  return crc;
}

void append_zmodem_escaped_byte(
    uint8_t *buf,
    size_t *used_len,
    uint8_t value,
    bool escape_ctrl) {
  bool needs_escape;

  needs_escape = false;
  if ((value == TEST_ZDLE) || (value == 0x7fu) || (value == 0xffu) ||
      (value == 0x11u) || (value == 0x13u) ||
      (value == 0x91u) || (value == 0x93u)) {
    needs_escape = true;
  }
  if (!needs_escape && escape_ctrl &&
      ((value < 0x20u) || (value == 0x80u) || (value == 0x90u))) {
    needs_escape = true;
  }
  if (!needs_escape) {
    buf[(*used_len)++] = value;
    return;
  }

  buf[(*used_len)++] = TEST_ZDLE;
  if (value == 0x7fu) {
    buf[(*used_len)++] = 'l';
  } else if (value == 0xffu) {
    buf[(*used_len)++] = 'm';
  } else {
    buf[(*used_len)++] = (uint8_t)(value ^ 0x40u);
  }
}

void append_zmodem_hex_header(
    uint8_t *buf,
    size_t *used_len,
    uint8_t type,
    const uint8_t hdr[4]) {
  uint16_t crc;
  size_t index;
  unsigned digit;

  crc = 0u;
  crc = test_zmodem_crc16_update(crc, type);
  for (index = 0u; index < 4u; ++index) {
    crc = test_zmodem_crc16_update(crc, hdr[index]);
  }
  crc = test_zmodem_crc16_update(crc, 0u);
  crc = test_zmodem_crc16_update(crc, 0u);

  buf[(*used_len)++] = TEST_ZPAD;
  buf[(*used_len)++] = TEST_ZPAD;
  buf[(*used_len)++] = TEST_ZDLE;
  buf[(*used_len)++] = TEST_ZHEX;
  digit = (type >> 4) & 0x0fu;
  buf[(*used_len)++] = (uint8_t)(digit < 10u ? ('0' + digit) : ('a' + digit - 10u));
  digit = type & 0x0fu;
  buf[(*used_len)++] = (uint8_t)(digit < 10u ? ('0' + digit) : ('a' + digit - 10u));
  for (index = 0u; index < 4u; ++index) {
    unsigned digit_hi;
    unsigned digit_lo;

    digit_hi = (hdr[index] >> 4) & 0x0fu;
    digit_lo = hdr[index] & 0x0fu;
    buf[(*used_len)++] = (uint8_t)(digit_hi < 10u ? ('0' + digit_hi) : ('a' + digit_hi - 10u));
    buf[(*used_len)++] = (uint8_t)(digit_lo < 10u ? ('0' + digit_lo) : ('a' + digit_lo - 10u));
  }
  for (index = 0u; index < 4u; ++index) {
    unsigned shift;
    unsigned crc_digit;

    shift = (unsigned)(12u - index * 4u);
    crc_digit = (crc >> shift) & 0x0fu;
    buf[(*used_len)++] = (uint8_t)(crc_digit < 10u ? ('0' + crc_digit) : ('a' + crc_digit - 10u));
  }
  buf[(*used_len)++] = '\r';
  buf[(*used_len)++] = 0x8au;
  if ((type != TEST_ZFIN) && (type != TEST_ZACK)) {
    buf[(*used_len)++] = 0x11u;
  }
}

void append_zmodem_binary_header(
    uint8_t *buf,
    size_t *used_len,
    uint8_t type,
    const uint8_t hdr[4],
    bool use_crc32,
    bool escape_ctrl) {
  size_t index;

  buf[(*used_len)++] = TEST_ZPAD;
  buf[(*used_len)++] = TEST_ZDLE;
  buf[(*used_len)++] = use_crc32 ? TEST_ZBIN32 : TEST_ZBIN;
  if (use_crc32) {
    uint32_t crc;

    crc = 0xffffffffu;
    crc = test_zmodem_crc32_update(crc, type);
    append_zmodem_escaped_byte(buf, used_len, type, escape_ctrl);
    for (index = 0u; index < 4u; ++index) {
      crc = test_zmodem_crc32_update(crc, hdr[index]);
      append_zmodem_escaped_byte(buf, used_len, hdr[index], escape_ctrl);
    }
    crc = ~crc;
    for (index = 0u; index < 4u; ++index) {
      append_zmodem_escaped_byte(
          buf,
          used_len,
          (uint8_t)((crc >> (index * 8u)) & 0xffu),
          escape_ctrl);
    }
  } else {
    uint16_t crc;

    crc = 0u;
    crc = test_zmodem_crc16_update(crc, type);
    append_zmodem_escaped_byte(buf, used_len, type, escape_ctrl);
    for (index = 0u; index < 4u; ++index) {
      crc = test_zmodem_crc16_update(crc, hdr[index]);
      append_zmodem_escaped_byte(buf, used_len, hdr[index], escape_ctrl);
    }
    crc = test_zmodem_crc16_update(crc, 0u);
    crc = test_zmodem_crc16_update(crc, 0u);
    append_zmodem_escaped_byte(buf, used_len, (uint8_t)(crc >> 8), escape_ctrl);
    append_zmodem_escaped_byte(buf, used_len, (uint8_t)crc, escape_ctrl);
  }
}

void append_zmodem_data_subpacket(
    uint8_t *buf,
    size_t *used_len,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t frame_end,
    bool use_crc32,
    bool escape_ctrl) {
  size_t index;

  if (use_crc32) {
    uint32_t crc;

    crc = 0xffffffffu;
    for (index = 0u; index < payload_len; ++index) {
      crc = test_zmodem_crc32_update(crc, payload[index]);
      append_zmodem_escaped_byte(buf, used_len, payload[index], escape_ctrl);
    }
    crc = test_zmodem_crc32_update(crc, frame_end);
    buf[(*used_len)++] = TEST_ZDLE;
    buf[(*used_len)++] = frame_end;
    crc = ~crc;
    for (index = 0u; index < 4u; ++index) {
      append_zmodem_escaped_byte(
          buf,
          used_len,
          (uint8_t)((crc >> (index * 8u)) & 0xffu),
          escape_ctrl);
    }
  } else {
    uint16_t crc;

    crc = 0u;
    for (index = 0u; index < payload_len; ++index) {
      crc = test_zmodem_crc16_update(crc, payload[index]);
      append_zmodem_escaped_byte(buf, used_len, payload[index], escape_ctrl);
    }
    crc = test_zmodem_crc16_update(crc, frame_end);
    crc = test_zmodem_crc16_update(crc, 0u);
    crc = test_zmodem_crc16_update(crc, 0u);
    buf[(*used_len)++] = TEST_ZDLE;
    buf[(*used_len)++] = frame_end;
    append_zmodem_escaped_byte(buf, used_len, (uint8_t)(crc >> 8), escape_ctrl);
    append_zmodem_escaped_byte(buf, used_len, (uint8_t)crc, escape_ctrl);
  }
}

size_t find_bytes(
    const uint8_t *haystack,
    size_t haystack_len,
    const uint8_t *needle,
    size_t needle_len) {
  size_t offset;

  if (needle_len == 0u) {
    return 0u;
  }
  for (offset = 0u; (offset + needle_len) <= haystack_len; ++offset) {
    if (memcmp(haystack + offset, needle, needle_len) == 0) {
      return offset;
    }
  }
  return haystack_len;
}

size_t count_bytes_pattern(
    const uint8_t *haystack,
    size_t haystack_len,
    const uint8_t *needle,
    size_t needle_len) {
  size_t count;
  size_t offset;

  count = 0u;
  if (needle_len == 0u) {
    return 0u;
  }
  for (offset = 0u; (offset + needle_len) <= haystack_len; ++offset) {
    if (memcmp(haystack + offset, needle, needle_len) == 0) {
      count += 1u;
    }
  }
  return count;
}

void make_temp_dir(char *path, size_t capacity, const char *prefix) {
#if defined(_WIN32)
  const char *temp_root_override;
  char temp_root[MAX_PATH];
  DWORD attempt;

  temp_root_override = test_win32_temp_root();
  if (temp_root_override != NULL) {
    ASSERT_TRUE(snprintf(temp_root, sizeof(temp_root), "%s", temp_root_override) > 0);
  } else {
    DWORD temp_len;

    temp_len = GetTempPathA((DWORD)sizeof(temp_root), temp_root);
    ASSERT_TRUE(temp_len > 0u && temp_len < sizeof(temp_root));
  }
  for (attempt = 0u; attempt < 128u; ++attempt) {
    ASSERT_TRUE(snprintf(
        path,
        capacity,
        "%s%s-%lu-%lu",
        temp_root,
        prefix,
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)(GetTickCount() + attempt)) > 0);
    if (CreateDirectoryA(path, NULL) != 0) {
      return;
    }
    ASSERT_TRUE(GetLastError() == ERROR_ALREADY_EXISTS);
  }
  fail_now(__FILE__, __LINE__, "failed to create temporary directory");
#else
  int written_len;

  written_len = snprintf(path, capacity, "/tmp/%s-XXXXXX", prefix);
  ASSERT_TRUE((written_len > 0) && ((size_t)written_len < capacity));
  ASSERT_TRUE(mkdtemp(path) == path);
#endif
}
