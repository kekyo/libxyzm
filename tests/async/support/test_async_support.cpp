/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "test_async_support.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
#include <liburing.h>
#include <linux/stat.h>
#endif

namespace {

class test_async_failure : public std::runtime_error {
 public:
  explicit test_async_failure(const std::string &message)
      : std::runtime_error(message) {}
};

static uint64_t async_now_ms(void) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

#if defined(_WIN32)
static SOCKET win32_socket_from_fd(intptr_t fd) {
  return static_cast<SOCKET>(static_cast<uintptr_t>(fd));
}

static void win32_ensure_winsock(void) {
  static bool initialized = false;
  if (!initialized) {
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
      throw xyzm_async_io_error("WSAStartup failed");
    }
    initialized = true;
  }
}

static cardio::promise<void> wait_socket_event(
    SOCKET socket_fd,
    long network_events,
    uint32_t timeout_ms,
    cardio::cancellation cancellation) {
  WSAEVENT event = WSACreateEvent();
  if (event == WSA_INVALID_EVENT) {
    throw xyzm_async_io_error("WSACreateEvent failed");
  }
  if (WSAEventSelect(socket_fd, event, network_events | FD_CLOSE) != 0) {
    WSACloseEvent(event);
    throw xyzm_async_io_error("WSAEventSelect failed");
  }
  try {
    auto timeout_source = cardio::cancellations::timeout(timeout_ms);
    auto wait_source = cardio::cancellations::any(
        cancellation,
        timeout_source.get_cancellation());
    co_await cardio::from_win32_handle(
        reinterpret_cast<HANDLE>(event),
        wait_source.get_cancellation());
    WSANETWORKEVENTS ready_events{};
    if (WSAEnumNetworkEvents(socket_fd, event, &ready_events) != 0) {
      throw xyzm_async_io_error("WSAEnumNetworkEvents failed");
    }
    if ((ready_events.lNetworkEvents & network_events) == 0) {
      throw xyzm_async_io_error("socket event mismatch");
    }
  } catch (const cardio::canceled_exception &) {
    WSAEventSelect(socket_fd, nullptr, 0);
    WSACloseEvent(event);
    if (cancellation.is_cancellation_requested()) {
      throw xyzm_async_cancelled_error("socket cancelled");
    }
    throw xyzm_async_timeout_error("socket timeout");
  } catch (...) {
    WSAEventSelect(socket_fd, nullptr, 0);
    WSACloseEvent(event);
    throw;
  }
  WSAEventSelect(socket_fd, nullptr, 0);
  WSACloseEvent(event);
}
#else
static bool has_fd_event(cardio::fd_event events, cardio::fd_event expected) {
  return (events & expected) != cardio::fd_event::none;
}

static cardio::promise<void> wait_fd_event(
    int fd,
    cardio::fd_event event,
    uint32_t timeout_ms,
    cardio::cancellation cancellation) {
  try {
    auto timeout_source = cardio::cancellations::timeout(timeout_ms);
    auto wait_source = cardio::cancellations::any(
        cancellation,
        timeout_source.get_cancellation());
    const auto events = co_await cardio::from_fd(
        fd,
        event,
        wait_source.get_cancellation());
    if (!has_fd_event(events, event)) {
      throw xyzm_async_io_error("fd event mismatch");
    }
  } catch (const cardio::canceled_exception &) {
    if (cancellation.is_cancellation_requested()) {
      throw xyzm_async_cancelled_error("fd cancelled");
    }
    throw xyzm_async_timeout_error("fd timeout");
  } catch (const xyzm_async_error &) {
    throw;
  } catch (...) {
    throw xyzm_async_io_error("fd wait failed");
  }
}
#endif

static void increment(std::size_t test_async_io_counters_t::*member, test_async_io_counters_t *counters) {
  if (counters != nullptr) {
    counters->*member += 1u;
  }
}

static void throw_file_io(const char *message) {
  throw xyzm_async_io_error(message);
}

#if defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
static void assert_io_uring_success(int result, const char *message) {
  if (result < 0) {
    throw xyzm_async_io_error(message);
  }
}

static cardio::promise<std::size_t> io_uring_read_at(
    cardio::io_uring &io,
    int fd,
    std::span<uint8_t> buffer,
    uint64_t offset,
    cardio::cancellation cancellation) {
  auto completion = co_await cardio::io_urings::submit(
      io,
      [fd, data = buffer.data(), size = buffer.size(), offset](::io_uring_sqe *sqe) {
        ::io_uring_prep_read(
            sqe,
            fd,
            data,
            static_cast<unsigned>(size),
            offset);
      },
      cancellation);
  assert_io_uring_success(completion.result, "io_uring read failed");
  co_return static_cast<std::size_t>(completion.result);
}

static cardio::promise<std::size_t> io_uring_write_at(
    cardio::io_uring &io,
    int fd,
    std::span<const uint8_t> buffer,
    uint64_t offset,
    cardio::cancellation cancellation) {
  auto completion = co_await cardio::io_urings::submit(
      io,
      [fd, data = buffer.data(), size = buffer.size(), offset](::io_uring_sqe *sqe) {
        ::io_uring_prep_write(
            sqe,
            fd,
            data,
            static_cast<unsigned>(size),
            offset);
      },
      cancellation);
  assert_io_uring_success(completion.result, "io_uring write failed");
  co_return static_cast<std::size_t>(completion.result);
}
#endif

}  // namespace

[[noreturn]] void test_async_fail_now(
    const char *file,
    int line,
    const char *fmt,
    ...) {
  char body[1024];
  char message[1200];
  va_list args;

  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  snprintf(message, sizeof(message), "%s:%d: %s", file, line, body);
  throw test_async_failure(message);
}

void test_async_logf(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

void test_async_create_results_dir(char *path, std::size_t capacity) {
  test_async_make_temp_dir(path, capacity, "libxyzm-async-results");
}

int test_async_run_cases(
    const char *results_dir,
    const test_async_case_t *cases,
    std::size_t count,
    unsigned *passed,
    unsigned *failed) {
  for (std::size_t index = 0; index < count; ++index) {
    const auto log_path =
        std::filesystem::path(results_dir) / (std::string(cases[index].name) + ".txt");
    std::ofstream log(log_path, std::ios::binary | std::ios::trunc);
    log << "test_name: " << cases[index].name << '\n';
    log << "description: " << cases[index].description << '\n';
    try {
      cases[index].fn();
      ++*passed;
      log << "result: PASS\n";
      std::fprintf(stdout, "[PASS] %s\n", cases[index].name);
    } catch (const std::exception &ex) {
      ++*failed;
      log << "result: FAIL\n";
      log << "error: " << ex.what() << '\n';
      std::fprintf(stdout, "[FAIL] %s\n", cases[index].name);
      std::fprintf(stdout, "       %s\n", ex.what());
    } catch (...) {
      ++*failed;
      log << "result: FAIL\n";
      log << "error: unknown exception\n";
      std::fprintf(stdout, "[FAIL] %s\n", cases[index].name);
      std::fprintf(stdout, "       unknown exception\n");
    }
  }
  return *failed == 0u ? 0 : 1;
}

void test_async_build_fake_link_ops(
    test_async_fake_link_t *link,
    xyzm_async_link_ops_t *ops) {
  ops->send =
      [link](
          std::span<const uint8_t> buf,
          uint32_t,
          std::size_t &written_len,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    link->send_count += 1u;
    link->max_send_len = std::max(link->max_send_len, buf.size());
    link->outgoing.insert(link->outgoing.end(), buf.begin(), buf.end());
    written_len = buf.size();
    link->now_ms += 1u;
    co_return;
  };
  ops->recv =
      [link](
          std::span<uint8_t> buf,
          uint32_t timeout_ms,
          std::size_t &read_len,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    link->recv_count += 1u;
    link->max_recv_len = std::max(link->max_recv_len, buf.size());
    if (link->timeout_recv_count > 0u) {
      link->timeout_recv_count -= 1u;
      link->now_ms += timeout_ms;
      read_len = 0u;
      throw xyzm_async_timeout_error("fake timeout");
    }
    const auto available_len = link->incoming.size() - link->incoming_pos;
    if (available_len == 0u) {
      link->now_ms += timeout_ms;
      read_len = 0u;
      throw xyzm_async_timeout_error("fake timeout");
    }
    const auto copy_len = std::min(available_len, buf.size());
    std::copy_n(link->incoming.data() + link->incoming_pos, copy_len, buf.data());
    link->incoming_pos += copy_len;
    read_len = copy_len;
    link->now_ms += 1u;
    co_return;
  };
  ops->now_ms = [link]() -> uint64_t {
    return link->now_ms;
  };
}

void test_async_build_memory_source_ops(
    test_async_memory_source_t *source,
    xyzm_async_source_ops_t *ops) {
  ops->next =
      [source](
          xyzm_async_file_info_t &info,
          cardio::cancellation cancellation) -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    if (source->served) {
      co_return xyzm_async_status_t::end;
    }
    source->served = true;
    info = source->info;
    co_return xyzm_async_status_t::ok;
  };
  ops->read =
      [source](
          std::span<uint8_t> buf,
          std::size_t &read_len,
          cardio::cancellation cancellation) -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    const auto available_len = source->data.size() - source->offset;
    if (available_len == 0u) {
      read_len = 0u;
      co_return xyzm_async_status_t::end;
    }
    const auto copy_len = std::min(available_len, buf.size());
    std::copy_n(source->data.data() + source->offset, copy_len, buf.data());
    source->offset += copy_len;
    read_len = copy_len;
    co_return xyzm_async_status_t::ok;
  };
  ops->seek =
      [source](uint64_t offset, cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    if (offset > source->data.size()) {
      throw xyzm_async_io_error("memory seek failed");
    }
    source->offset = static_cast<std::size_t>(offset);
    co_return;
  };
  ops->end =
      [source](
          const xyzm_async_file_info_t &,
          std::exception_ptr error,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    source->end_error = error;
    co_return;
  };
}

void test_async_build_memory_sink_ops(
    test_async_memory_sink_t *sink,
    xyzm_async_sink_ops_t *ops) {
  ops->begin =
      [sink](
          const xyzm_async_file_info_t &info,
          uint64_t &resume_offset,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    sink->info = info;
    sink->began = true;
    resume_offset = sink->resume_offset;
    co_return;
  };
	  ops->write =
	      [sink](
	          std::span<const uint8_t> buf,
	          cardio::cancellation cancellation) -> cardio::promise<void> {
	    cancellation.throw_if_cancellation_requested();
	    sink->write_count += 1u;
	    sink->max_write_len = std::max(sink->max_write_len, buf.size());
	    sink->data.insert(sink->data.end(), buf.begin(), buf.end());
	    co_return;
	  };
  ops->end =
      [sink](
          const xyzm_async_file_info_t &info,
          std::exception_ptr error,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    sink->info = info;
    sink->end_error = error;
    co_return;
  };
}

void test_async_build_fd_link_ops(
    test_async_fd_link_t *link,
    xyzm_async_link_ops_t *ops) {
  ops->send =
      [link](
          std::span<const uint8_t> buf,
          uint32_t timeout_ms,
          std::size_t &written_len,
          cardio::cancellation cancellation) -> cardio::promise<void> {
#if defined(_WIN32)
    win32_ensure_winsock();
    const auto socket_fd = win32_socket_from_fd(link->fd);
    co_await wait_socket_event(socket_fd, FD_WRITE, timeout_ms, cancellation);
    increment(&test_async_io_counters_t::transport_wait_write_count, link->counters);
    const auto result = send(
        socket_fd,
        reinterpret_cast<const char *>(buf.data()),
        static_cast<int>(buf.size()),
        0);
    if (result <= 0) {
      written_len = 0u;
      throw xyzm_async_io_error("socket send failed");
    }
    written_len = static_cast<std::size_t>(result);
#else
    co_await wait_fd_event(link->fd, cardio::fd_event::write, timeout_ms, cancellation);
    increment(&test_async_io_counters_t::transport_wait_write_count, link->counters);
#ifdef MSG_NOSIGNAL
    const auto result = ::send(link->fd, buf.data(), buf.size(), MSG_NOSIGNAL);
#else
    const auto result = ::write(link->fd, buf.data(), buf.size());
#endif
    if (result <= 0) {
      written_len = 0u;
      throw xyzm_async_io_error("fd send failed");
    }
    written_len = static_cast<std::size_t>(result);
#endif
    increment(&test_async_io_counters_t::transport_write_count, link->counters);
    co_return;
  };
  ops->recv =
      [link](
          std::span<uint8_t> buf,
          uint32_t timeout_ms,
          std::size_t &read_len,
          cardio::cancellation cancellation) -> cardio::promise<void> {
#if defined(_WIN32)
    win32_ensure_winsock();
    const auto socket_fd = win32_socket_from_fd(link->fd);
    co_await wait_socket_event(socket_fd, FD_READ, timeout_ms, cancellation);
    increment(&test_async_io_counters_t::transport_wait_read_count, link->counters);
    const auto result = recv(
        socket_fd,
        reinterpret_cast<char *>(buf.data()),
        static_cast<int>(buf.size()),
        0);
    if (result <= 0) {
      read_len = 0u;
      throw xyzm_async_io_error("socket recv failed");
    }
    read_len = static_cast<std::size_t>(result);
#else
    co_await wait_fd_event(link->fd, cardio::fd_event::read, timeout_ms, cancellation);
    increment(&test_async_io_counters_t::transport_wait_read_count, link->counters);
    const auto result = ::read(link->fd, buf.data(), buf.size());
    if (result <= 0) {
      read_len = 0u;
      throw xyzm_async_io_error("fd recv failed");
    }
    read_len = static_cast<std::size_t>(result);
#endif
    increment(&test_async_io_counters_t::transport_read_count, link->counters);
    co_return;
  };
  ops->now_ms = []() -> uint64_t {
    return async_now_ms();
  };
}

void test_async_build_file_source_ops(
    test_async_file_source_t *source,
    xyzm_async_source_ops_t *ops) {
  ops->next =
      [source](
          xyzm_async_file_info_t &info,
          cardio::cancellation cancellation) -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    if (source->served) {
      co_return xyzm_async_status_t::end;
    }
#if defined(_WIN32)
    HANDLE handle = CreateFileA(
        source->path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      throw_file_io("CreateFile read failed");
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0) {
      CloseHandle(handle);
      throw_file_io("GetFileSizeEx failed");
    }
    source->handle = handle;
    source->size = static_cast<uint64_t>(size.QuadPart);
    increment(&test_async_io_counters_t::file_open_count, source->counters);
    increment(&test_async_io_counters_t::file_stat_count, source->counters);
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
    if (source->io == nullptr) {
      throw_file_io("missing io_uring");
    }
    struct statx stat_value {};
    increment(&test_async_io_counters_t::file_stat_count, source->counters);
    co_await cardio::io_urings::statx(
        *source->io,
        AT_FDCWD,
        source->path,
        0,
        STATX_SIZE,
        stat_value,
        cancellation);
    source->size = stat_value.stx_size;
    increment(&test_async_io_counters_t::file_open_count, source->counters);
    source->fd = co_await cardio::io_urings::open(
        *source->io,
        source->path,
        O_RDONLY | O_CLOEXEC,
        0,
        cancellation);
#else
    (void)cancellation;
    throw xyzm_async_unsupported_error("async file source requires io_uring or overlapped I/O");
#endif
    source->served = true;
    source->offset = 0u;
    info = {};
    info.name = source->name;
    info.size_bytes = source->size;
    co_return xyzm_async_status_t::ok;
  };
  ops->read =
      [source](
          std::span<uint8_t> buf,
          std::size_t &read_len,
          cardio::cancellation cancellation) -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    if (source->offset >= source->size) {
      read_len = 0u;
      co_return xyzm_async_status_t::end;
    }
    const auto remaining = source->size - source->offset;
    const auto wanted = std::min<uint64_t>(remaining, buf.size());
    auto chunk = buf.first(static_cast<std::size_t>(wanted));
#if defined(_WIN32)
    auto bytes = std::span<std::byte>(
        reinterpret_cast<std::byte *>(chunk.data()),
        chunk.size());
    const auto result = co_await cardio::win32::read(
        static_cast<HANDLE>(source->handle),
        bytes,
        source->offset,
        cancellation);
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
    const auto result = co_await io_uring_read_at(
        *source->io,
        source->fd,
        chunk,
        source->offset,
        cancellation);
#else
    (void)chunk;
    throw xyzm_async_unsupported_error("async file read requires io_uring or overlapped I/O");
#endif
    if (result == 0u) {
      read_len = 0u;
      co_return xyzm_async_status_t::end;
    }
    source->offset += result;
    read_len = result;
    increment(&test_async_io_counters_t::file_read_count, source->counters);
    co_return xyzm_async_status_t::ok;
  };
  ops->seek =
      [source](uint64_t offset, cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    if (offset > source->size) {
      throw_file_io("file source seek failed");
    }
    source->offset = offset;
    co_return;
  };
  ops->end =
      [source](
          const xyzm_async_file_info_t &,
          std::exception_ptr,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
#if defined(_WIN32)
    if (source->handle != nullptr) {
      CloseHandle(static_cast<HANDLE>(source->handle));
      source->handle = nullptr;
      increment(&test_async_io_counters_t::file_close_count, source->counters);
    }
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
    if (source->fd >= 0) {
      const auto fd = source->fd;
      source->fd = -1;
      increment(&test_async_io_counters_t::file_close_count, source->counters);
      co_await cardio::io_urings::close(*source->io, fd, cancellation);
    }
#else
    (void)cancellation;
#endif
    co_return;
  };
}

void test_async_build_file_sink_ops(
    test_async_file_sink_t *sink,
    xyzm_async_sink_ops_t *ops) {
  ops->begin =
      [sink](
          const xyzm_async_file_info_t &info,
          uint64_t &resume_offset,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    const auto name = info.name.has_value() && !info.name->empty()
        ? *info.name
        : sink->fallback_name;
    sink->path = (std::filesystem::path(sink->directory) / name).string();
    sink->offset = 0u;
    resume_offset = 0u;
#if defined(_WIN32)
    HANDLE handle = CreateFileA(
        sink->path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      throw_file_io("CreateFile write failed");
    }
    sink->handle = handle;
    increment(&test_async_io_counters_t::file_open_count, sink->counters);
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
    if (sink->io == nullptr) {
      throw_file_io("missing io_uring");
    }
    increment(&test_async_io_counters_t::file_open_count, sink->counters);
    sink->fd = co_await cardio::io_urings::open(
        *sink->io,
        sink->path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0666,
        cancellation);
#else
    throw xyzm_async_unsupported_error("async file sink requires io_uring or overlapped I/O");
#endif
    co_return;
  };
  ops->write =
      [sink](
          std::span<const uint8_t> buf,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    if (buf.empty()) {
      co_return;
    }
#if defined(_WIN32)
    auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(buf.data()),
        buf.size());
    const auto result = co_await cardio::win32::write(
        static_cast<HANDLE>(sink->handle),
        bytes,
        sink->offset,
        cancellation);
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
    const auto result = co_await io_uring_write_at(
        *sink->io,
        sink->fd,
        buf,
        sink->offset,
        cancellation);
#else
    throw xyzm_async_unsupported_error("async file write requires io_uring or overlapped I/O");
#endif
    if (result != buf.size()) {
      throw_file_io("short file write");
    }
    sink->offset += result;
    increment(&test_async_io_counters_t::file_write_count, sink->counters);
    co_return;
  };
  ops->end =
      [sink](
          const xyzm_async_file_info_t &,
          std::exception_ptr,
          cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
#if defined(_WIN32)
    if (sink->handle != nullptr) {
      CloseHandle(static_cast<HANDLE>(sink->handle));
      sink->handle = nullptr;
      increment(&test_async_io_counters_t::file_close_count, sink->counters);
    }
#elif defined(__linux__) && CARDIO_WITH_LINUX_IO_URING
    if (sink->fd >= 0) {
      const auto fd = sink->fd;
      sink->fd = -1;
      increment(&test_async_io_counters_t::file_close_count, sink->counters);
      co_await cardio::io_urings::close(*sink->io, fd, cancellation);
    }
#else
    (void)cancellation;
#endif
    co_return;
  };
}

xyzm_xmodem_send_opts_t test_async_xmodem_send_options(
    xyzm_xmodem_checksum_mode_t checksum_mode,
    xyzm_xmodem_packet_size_t packet_size) {
  return xyzm_xmodem_send_opts_t{
      5000u,
      5000u,
      8u,
      0x1au,
      checksum_mode,
      packet_size};
}

xyzm_xmodem_receive_opts_t test_async_xmodem_receive_options(
    xyzm_xmodem_checksum_mode_t checksum_mode) {
  return xyzm_xmodem_receive_opts_t{
      5000u,
      5000u,
      8u,
      0x1au,
      checksum_mode};
}

xyzm_ymodem_opts_t test_async_ymodem_options(xyzm_ymodem_variant_t variant) {
  return xyzm_ymodem_opts_t{5000u, 5000u, 8u, 0x1au, variant};
}

xyzm_zmodem_opts_t test_async_zmodem_options(void) {
  return xyzm_zmodem_opts_t{5000u, 5000u, 8u, 0x1au, 0u, 1u, 1u};
}

void test_async_append_xmodem_block(
    std::vector<uint8_t> *buffer,
    uint8_t block_number,
    std::span<const uint8_t> payload,
    bool use_crc,
    uint8_t header) {
  buffer->push_back(header);
  buffer->push_back(block_number);
  buffer->push_back(static_cast<uint8_t>(255u - block_number));
  buffer->insert(buffer->end(), payload.begin(), payload.end());
  if (use_crc) {
    const auto crc = xyzm_crc16_xmodem_update_buffer(0u, payload.data(), payload.size());
    buffer->push_back(static_cast<uint8_t>(crc >> 8));
    buffer->push_back(static_cast<uint8_t>(crc & 0xffu));
  } else {
    buffer->push_back(xyzm_checksum8_update_buffer(0u, payload.data(), payload.size()));
  }
}

void test_async_fill_pattern(uint8_t *buf, std::size_t len, uint8_t seed) {
  for (std::size_t index = 0; index < len; ++index) {
    buf[index] = static_cast<uint8_t>(seed + ((index * 31u) ^ (index >> 3u)));
  }
}

std::size_t test_async_xmodem_1k_padded_size(std::size_t data_len) {
  if (data_len == 0u) {
    return 0u;
  }
  if (data_len <= 128u) {
    return 128u;
  }
  if ((data_len % 1024u) == 0u) {
    return data_len;
  }
  if ((data_len % 1024u) <= 128u) {
    return ((data_len / 1024u) * 1024u) + 128u;
  }
  return ((data_len / 1024u) + 1u) * 1024u;
}

void test_async_make_temp_dir(char *path, std::size_t capacity, const char *prefix) {
  const auto base = std::filesystem::temp_directory_path();
  const auto stamp = static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  for (unsigned attempt = 0; attempt < 1000u; ++attempt) {
    const auto candidate = base /
        (std::string(prefix) + "-" + std::to_string(stamp) + "-" + std::to_string(attempt));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      const auto text = candidate.string();
      if (text.size() + 1u > capacity) {
        throw std::runtime_error("temporary directory path is too long");
      }
      std::memcpy(path, text.c_str(), text.size() + 1u);
      return;
    }
  }
  throw std::runtime_error("temporary directory creation failed");
}

void test_async_remove_tree(const char *path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    throw std::runtime_error("remove_tree failed: " + error.message());
  }
}

void test_async_write_file(const char *path, std::span<const uint8_t> data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("open write file failed");
  }
  output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!output) {
    throw std::runtime_error("write file failed");
  }
}

std::vector<uint8_t> test_async_read_file(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("open read file failed");
  }
  return std::vector<uint8_t>(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
}

test_async_child_process_t test_async_spawn_child(
    char *const argv[],
    const char *working_directory) {
  test_async_child_process_t child{};
#if defined(_WIN32)
  (void)argv;
  (void)working_directory;
  throw std::runtime_error("async child process is not implemented for Win32");
#else
  int sockets[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    throw std::system_error(errno, std::generic_category(), "socketpair failed");
  }
  if (pipe(stderr_pipe) != 0) {
    const auto error = errno;
    close(sockets[0]);
    close(sockets[1]);
    throw std::system_error(error, std::generic_category(), "pipe failed");
  }
  const auto pid = fork();
  if (pid < 0) {
    const auto error = errno;
    close(sockets[0]);
    close(sockets[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    throw std::system_error(error, std::generic_category(), "fork failed");
  }
  if (pid == 0) {
    close(sockets[0]);
    close(stderr_pipe[0]);
    if (working_directory != nullptr) {
      if (chdir(working_directory) != 0) {
        _exit(126);
      }
    }
    (void)dup2(sockets[1], STDIN_FILENO);
    (void)dup2(sockets[1], STDOUT_FILENO);
    (void)dup2(stderr_pipe[1], STDERR_FILENO);
    close(sockets[1]);
    close(stderr_pipe[1]);
    execv(argv[0], argv);
    _exit(127);
  }
  close(sockets[1]);
  close(stderr_pipe[1]);
  child.pid = pid;
  child.fd = sockets[0];
  child.stderr_fd = stderr_pipe[0];
#endif
  return child;
}

void test_async_wait_child_ok(test_async_child_process_t *child) {
#if defined(_WIN32)
  (void)child;
  throw std::runtime_error("async child process is not implemented for Win32");
#else
  int status = 0;
  if (child->fd >= 0) {
    close(child->fd);
    child->fd = -1;
  }
  if (waitpid(child->pid, &status, 0) != child->pid) {
    throw std::system_error(errno, std::generic_category(), "waitpid failed");
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    if (child->stderr_fd >= 0) {
      close(child->stderr_fd);
      child->stderr_fd = -1;
    }
    return;
  }
  std::string stderr_text;
  if (child->stderr_fd >= 0) {
    char buf[256];
    ssize_t read_len = 0;
    while ((read_len = read(child->stderr_fd, buf, sizeof(buf))) > 0) {
      stderr_text.append(buf, static_cast<std::size_t>(read_len));
    }
    close(child->stderr_fd);
    child->stderr_fd = -1;
  }
  throw std::runtime_error("child process failed: " + stderr_text);
#endif
}
