#!/bin/bash
# Runs McMini's TSan example targets through a DMTCP record cycle
# (`mcmini -i <interval> ./target`) and checks for anything beyond the
# accepted "thread leak" warning (see mc_pthread_join_impl()'s RECORD-mode
# FIXME comment in src/lib/wrappers.c for why that one is expected).
#
# Usage: check-tsan.sh <build-dir> [dmtcp-bin-dir]
#
# <build-dir> must contain libmcmini.so, mcmini, and the *-tsan targets.
# [dmtcp-bin-dir], if given, is prepended to PATH and its ../lib/dmtcp to
# LD_LIBRARY_PATH (needed if dmtcp_launch/dmtcp_restart aren't already on
# PATH -- e.g. a local DMTCP checkout not installed system-wide).
set -u

BUILD_DIR="${1:?usage: check-tsan.sh <build-dir> [dmtcp-bin-dir]}"
DMTCP_BIN_DIR="${2:-}"

if [ -n "$DMTCP_BIN_DIR" ]; then
  export PATH="$DMTCP_BIN_DIR:$PATH"
  export LD_LIBRARY_PATH="$DMTCP_BIN_DIR/../lib/dmtcp:${LD_LIBRARY_PATH:-}"
fi

if ! command -v dmtcp_launch >/dev/null 2>&1; then
  echo "check-tsan: dmtcp_launch not found on PATH (pass dmtcp-bin-dir?)" >&2
  exit 2
fi

TARGETS="cv-producer-consumer-multi-tsan"
CKPT_INTERVAL=3
TIMEOUT_SECS=90
FAIL=0

cd "$BUILD_DIR" || exit 2

for target in $TARGETS; do
  echo "=== check-tsan: $target ==="
  pkill -9 -f dmtcp_co >/dev/null 2>&1
  rm -f ./*.dmtcp ./*.temp ./dmtcp_restart_script*.sh ./ckpt* 2>/dev/null
  rm -f /dev/shm/mcmini* /tmp/mcmini-fifo 2>/dev/null

  LOG=$(mktemp)
  timeout -s KILL "$TIMEOUT_SECS" ./mcmini -i "$CKPT_INTERVAL" "./$target" \
    >"$LOG" 2>&1
  rc=$?

  # Anything other than a plain "thread leak" summary is a real failure.
  # (See mc_pthread_join_impl()'s RECORD-mode comment for why leaks alone
  # are an accepted, documented trade-off, not a bug.)
  bad_summaries=$(grep "^SUMMARY: ThreadSanitizer:" "$LOG" \
    | grep -v "thread leak")

  if [ "$rc" != "0" ] && [ "$rc" != "66" ]; then
    echo "FAIL: $target exited $rc (expected 0 or 66-with-leaks-only)"
    cat "$LOG"
    FAIL=1
  elif [ -n "$bad_summaries" ]; then
    echo "FAIL: $target reported unexpected TSan issues:"
    echo "$bad_summaries"
    cat "$LOG"
    FAIL=1
  else
    echo "PASS: $target"
  fi
  rm -f "$LOG"
done

pkill -9 -f dmtcp_co >/dev/null 2>&1
rm -f ./*.dmtcp ./*.temp ./dmtcp_restart_script*.sh ./ckpt* 2>/dev/null
rm -f /dev/shm/mcmini* /tmp/mcmini-fifo 2>/dev/null

exit $FAIL
