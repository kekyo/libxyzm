/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_async_support.h"

#include <cstdio>
#include <cstring>

extern const test_async_case_t xyzm_async_core_tests[];
extern const std::size_t xyzm_async_core_tests_count;
extern const test_async_case_t xyzm_async_transfer_event_tests[];
extern const std::size_t xyzm_async_transfer_event_tests_count;
extern const test_async_case_t xyzm_async_zmodem_integration_tests[];
extern const std::size_t xyzm_async_zmodem_integration_tests_count;
#if !defined(_WIN32)
extern const test_async_case_t xyzm_async_xmodem_integration_tests[];
extern const std::size_t xyzm_async_xmodem_integration_tests_count;
#endif

static int run_suite(
    const char *label,
    const char *results_dir,
    const test_async_case_t *cases,
    std::size_t count,
    unsigned *passed,
    unsigned *failed) {
  std::fprintf(stdout, "== %s ==\n", label);
  return test_async_run_cases(results_dir, cases, count, passed, failed);
}

static const test_async_case_t *find_case_by_name(const char *name) {
  for (std::size_t index = 0; index < xyzm_async_core_tests_count; ++index) {
    if (std::strcmp(xyzm_async_core_tests[index].name, name) == 0) {
      return &xyzm_async_core_tests[index];
    }
  }
  for (std::size_t index = 0; index < xyzm_async_transfer_event_tests_count; ++index) {
    if (std::strcmp(xyzm_async_transfer_event_tests[index].name, name) == 0) {
      return &xyzm_async_transfer_event_tests[index];
    }
  }
  for (std::size_t index = 0; index < xyzm_async_zmodem_integration_tests_count; ++index) {
    if (std::strcmp(xyzm_async_zmodem_integration_tests[index].name, name) == 0) {
      return &xyzm_async_zmodem_integration_tests[index];
    }
  }
#if !defined(_WIN32)
  for (std::size_t index = 0; index < xyzm_async_xmodem_integration_tests_count; ++index) {
    if (std::strcmp(xyzm_async_xmodem_integration_tests[index].name, name) == 0) {
      return &xyzm_async_xmodem_integration_tests[index];
    }
  }
#endif
  return nullptr;
}

int main(int argc, char **argv) {
  char results_dir[512];
  unsigned passed = 0;
  unsigned failed = 0;
  int status = 0;

  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::setvbuf(stderr, nullptr, _IOLBF, 0);
  if (argc == 3 && std::strcmp(argv[1], "--case") == 0) {
    const auto *single_case = find_case_by_name(argv[2]);
    if (single_case == nullptr) {
      std::fprintf(stderr, "unknown async test case: %s\n", argv[2]);
      return 2;
    }
    single_case->fn();
    return 0;
  }

  test_async_create_results_dir(results_dir, sizeof(results_dir));
  std::fprintf(stdout, "test_results_dir: %s\n", results_dir);
  status |= run_suite(
      "async-core",
      results_dir,
      xyzm_async_core_tests,
      xyzm_async_core_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "async-transfer-events",
      results_dir,
      xyzm_async_transfer_event_tests,
      xyzm_async_transfer_event_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "async-zmodem-integration",
      results_dir,
      xyzm_async_zmodem_integration_tests,
      xyzm_async_zmodem_integration_tests_count,
      &passed,
      &failed);
#if !defined(_WIN32)
  status |= run_suite(
      "async-xmodem-integration",
      results_dir,
      xyzm_async_xmodem_integration_tests,
      xyzm_async_xmodem_integration_tests_count,
      &passed,
      &failed);
#endif
  std::fprintf(stdout, "summary: passed=%u failed=%u\n", passed, failed);
  return status;
}
