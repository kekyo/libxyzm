#!/bin/sh

set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cd "$SCRIPT_DIR"

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

validate_positive_integer() {
  value_name=$1
  value=$2

  case $value in
    '' | *[!0-9]*)
      fail "$value_name must be a positive integer: $value"
      ;;
  esac

  [ "$value" -gt 0 ] || fail "$value_name must be a positive integer: $value"
}

detect_processor_count() {
  detected_count=''

  if command -v getconf >/dev/null 2>&1; then
    detected_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
  fi
  if [ -z "$detected_count" ] && command -v nproc >/dev/null 2>&1; then
    detected_count=$(nproc 2>/dev/null || true)
  fi
  if [ -z "$detected_count" ] && command -v sysctl >/dev/null 2>&1; then
    detected_count=$(sysctl -n hw.ncpu 2>/dev/null || true)
  fi

  case $detected_count in
    '' | *[!0-9]*)
      detected_count=1
      ;;
  esac

  if [ "$detected_count" -lt 1 ]; then
    detected_count=1
  fi

  printf '%s\n' "$detected_count"
}

if [ -z "${LIBXYZM_BUILD_JOBS:-}" ]; then
  LIBXYZM_BUILD_JOBS=$(detect_processor_count)
fi
validate_positive_integer LIBXYZM_BUILD_JOBS "$LIBXYZM_BUILD_JOBS"

WIN64_CC=x86_64-w64-mingw32-gcc
WIN64_CXX=x86_64-w64-mingw32-g++
WIN32_CC=i686-w64-mingw32-gcc
WIN32_CXX=i686-w64-mingw32-g++

run_with_retry() {
  max_attempts=$1
  shift
  attempt=1

  while :
  do
    if "$@"; then
      return 0
    fi
    if [ "$attempt" -ge "$max_attempts" ]; then
      return 1
    fi
    attempt=$((attempt + 1))
    printf '%s\n' "Retrying ($attempt/$max_attempts): $*"
  done
}

make clean

make -j "$LIBXYZM_BUILD_JOBS" all
make -j "$LIBXYZM_BUILD_JOBS" all TARGET_OS=win32 CC="$WIN64_CC" CXX="$WIN64_CXX"
make -j "$LIBXYZM_BUILD_JOBS" all TARGET_OS=win32 CC="$WIN32_CC" CXX="$WIN32_CXX"

run_with_retry 2 make -j "$LIBXYZM_BUILD_JOBS" test
run_with_retry 2 make -j "$LIBXYZM_BUILD_JOBS" test TARGET_OS=win32 CC="$WIN64_CC" CXX="$WIN64_CXX"
run_with_retry 2 make -j "$LIBXYZM_BUILD_JOBS" test TARGET_OS=win32 CC="$WIN32_CC" CXX="$WIN32_CXX"
