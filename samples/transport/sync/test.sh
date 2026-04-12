#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)

make -C "$PROJECT_ROOT" all
make -C "$SCRIPT_DIR"
node "$PROJECT_ROOT/samples/test_foreign_zmodem.ts" "$SCRIPT_DIR/minizm-sync"
