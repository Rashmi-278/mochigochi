#!/usr/bin/env bash
# Run the firmware Makefile targets sequentially, stopping at the first failure.
#
# Usage:
#   ./run-make.sh              # CI-safe pipeline: format-check, build, db
#   ./run-make.sh --flash      # also flash the board after a successful build
#   ./run-make.sh --clean      # prepend a clean before building
#   ./run-make.sh PORT=/dev/cu.usbmodemXXXX --flash
#
# Interactive targets (monitor, flash-monitor) are intentionally excluded
# because they block and never return.
set -euo pipefail

cd "$(dirname "$0")"

# Split args into make-style VAR=value overrides and our own flags.
MAKE_ARGS=()
DO_CLEAN=0
DO_FLASH=0
DO_FORMAT=1
for arg in "$@"; do
  case "$arg" in
    --clean)     DO_CLEAN=1 ;;
    --flash)     DO_FLASH=1 ;;
    --no-format) DO_FORMAT=0 ;;
    *=*)         MAKE_ARGS+=("$arg") ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

# Build the ordered list of targets.
TARGETS=()
[ "$DO_CLEAN" -eq 1 ] && TARGETS+=(clean)
[ "$DO_FORMAT" -eq 1 ] && TARGETS+=(format-check)
TARGETS+=(build db)
[ "$DO_FLASH" -eq 1 ] && TARGETS+=(flash)

for target in "${TARGETS[@]}"; do
  echo "==> make $target ${MAKE_ARGS[*]:-}"
  make "$target" "${MAKE_ARGS[@]}"
done

echo "==> done: ${TARGETS[*]}"
