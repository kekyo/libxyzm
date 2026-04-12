#!/bin/sh

set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cd "$SCRIPT_DIR"

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

make all -j
make all TARGET_OS=win32 CC="$WIN64_CC" CXX="$WIN64_CXX" -j
make all TARGET_OS=win32 CC="$WIN32_CC" CXX="$WIN32_CXX" -j

run_with_retry 2 make test -j
run_with_retry 2 make test TARGET_OS=win32 CC="$WIN64_CC" CXX="$WIN64_CXX" -j
run_with_retry 2 make test TARGET_OS=win32 CC="$WIN32_CC" CXX="$WIN32_CXX" -j
