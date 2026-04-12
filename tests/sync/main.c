/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "support/test_sync_support.h"

#include <stdio.h>
#include <string.h>

extern const test_case_t xyzm_core_tests[];
extern const size_t xyzm_core_tests_count;
extern const test_case_t xyzm_xmodem_unit_tests[];
extern const size_t xyzm_xmodem_unit_tests_count;
extern const test_case_t xyzm_ymodem_unit_tests[];
extern const size_t xyzm_ymodem_unit_tests_count;
extern const test_case_t xyzm_zmodem_unit_tests[];
extern const size_t xyzm_zmodem_unit_tests_count;
extern const test_case_t xyzm_transfer_event_tests[];
extern const size_t xyzm_transfer_event_tests_count;
extern const test_case_t xyzm_xmodem_integration_tests[];
extern const size_t xyzm_xmodem_integration_tests_count;
extern const test_case_t xyzm_ymodem_integration_tests[];
extern const size_t xyzm_ymodem_integration_tests_count;
extern const test_case_t xyzm_zmodem_integration_tests[];
extern const size_t xyzm_zmodem_integration_tests_count;

static int run_suite(
    const char *label,
    const char *results_dir,
    const test_case_t *cases,
    size_t count,
    unsigned *passed,
    unsigned *failed) {
  fprintf(stdout, "== %s ==\n", label);
  return test_run_cases(results_dir, cases, count, passed, failed);
}

static const test_case_t *find_case_by_name(
    const char *name) {
  size_t index;

  for (index = 0u; index < xyzm_core_tests_count; ++index) {
    if (strcmp(xyzm_core_tests[index].name, name) == 0) {
      return &xyzm_core_tests[index];
    }
  }
  for (index = 0u; index < xyzm_xmodem_unit_tests_count; ++index) {
    if (strcmp(xyzm_xmodem_unit_tests[index].name, name) == 0) {
      return &xyzm_xmodem_unit_tests[index];
    }
  }
  for (index = 0u; index < xyzm_ymodem_unit_tests_count; ++index) {
    if (strcmp(xyzm_ymodem_unit_tests[index].name, name) == 0) {
      return &xyzm_ymodem_unit_tests[index];
    }
  }
  for (index = 0u; index < xyzm_zmodem_unit_tests_count; ++index) {
    if (strcmp(xyzm_zmodem_unit_tests[index].name, name) == 0) {
      return &xyzm_zmodem_unit_tests[index];
    }
  }
  for (index = 0u; index < xyzm_transfer_event_tests_count; ++index) {
    if (strcmp(xyzm_transfer_event_tests[index].name, name) == 0) {
      return &xyzm_transfer_event_tests[index];
    }
  }
  for (index = 0u; index < xyzm_xmodem_integration_tests_count; ++index) {
    if (strcmp(xyzm_xmodem_integration_tests[index].name, name) == 0) {
      return &xyzm_xmodem_integration_tests[index];
    }
  }
  for (index = 0u; index < xyzm_ymodem_integration_tests_count; ++index) {
    if (strcmp(xyzm_ymodem_integration_tests[index].name, name) == 0) {
      return &xyzm_ymodem_integration_tests[index];
    }
  }
  for (index = 0u; index < xyzm_zmodem_integration_tests_count; ++index) {
    if (strcmp(xyzm_zmodem_integration_tests[index].name, name) == 0) {
      return &xyzm_zmodem_integration_tests[index];
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  char results_dir[256];
  unsigned passed;
  unsigned failed;
  int status;
  const test_case_t *single_case;

  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  if (argc == 3 && strcmp(argv[1], "--case") == 0) {
    single_case = find_case_by_name(argv[2]);
    if (single_case == NULL) {
      fprintf(stderr, "unknown test case: %s\n", argv[2]);
      return 2;
    }
    single_case->fn();
    return 0;
  }
  passed = 0u;
  failed = 0u;
  status = 0;
  test_create_results_dir(results_dir, sizeof(results_dir));
  fprintf(stdout, "test_results_dir: %s\n", results_dir);

  status |= run_suite("core", results_dir, xyzm_core_tests, xyzm_core_tests_count, &passed, &failed);
  status |= run_suite(
      "xmodem-unit",
      results_dir,
      xyzm_xmodem_unit_tests,
      xyzm_xmodem_unit_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "ymodem-unit",
      results_dir,
      xyzm_ymodem_unit_tests,
      xyzm_ymodem_unit_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "zmodem-unit",
      results_dir,
      xyzm_zmodem_unit_tests,
      xyzm_zmodem_unit_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "transfer-events",
      results_dir,
      xyzm_transfer_event_tests,
      xyzm_transfer_event_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "xmodem-integration",
      results_dir,
      xyzm_xmodem_integration_tests,
      xyzm_xmodem_integration_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "ymodem-integration",
      results_dir,
      xyzm_ymodem_integration_tests,
      xyzm_ymodem_integration_tests_count,
      &passed,
      &failed);
  status |= run_suite(
      "zmodem-integration",
      results_dir,
      xyzm_zmodem_integration_tests,
      xyzm_zmodem_integration_tests_count,
      &passed,
      &failed);
  fprintf(stdout, "summary: passed=%u failed=%u\n", passed, failed);
  return status;
}
