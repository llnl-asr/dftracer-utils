#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Server stress test: generate a large trace, run the viz server under a
# concurrent load, and compare member-cache on vs off (latency, throughput,
# peak RSS). Manual harness, not part of CI.
#
# Usage:
#   benchmarks/stress/server_stress.sh [build_dir]
#
# Tunables via env (defaults target ~15 GB uncompressed across several files):
#   TRACE_DIR, NUM_PROCESSES, NUM_HOSTS, EPOCHS, STEPS, NUM_TRAIN_FILES
#   DURATION, CONCURRENCY, MIX, CACHE_SIZES, PORT
set -euo pipefail

BUILD_DIR="${1:-build/build-tests}"
GEN="$BUILD_DIR/bin/dftracer_gen_fake_trace"
SERVER="$BUILD_DIR/bin/dftracer_server"
DRIVER="$(dirname "$0")/load_driver.py"

TRACE_DIR="${TRACE_DIR:-/tmp/dft_stress_trace}"
# ~15 GB uncompressed: several rank files, deep run. Tune if your box differs.
NUM_PROCESSES="${NUM_PROCESSES:-8}"
NUM_HOSTS="${NUM_HOSTS:-2}"
EPOCHS="${EPOCHS:-200}"
STEPS="${STEPS:-4000}"
NUM_TRAIN_FILES="${NUM_TRAIN_FILES:-64}"
CHECKPOINT_SIZE="${CHECKPOINT_SIZE:-16777216}"   # 16 MB members

DURATION="${DURATION:-30}"
CONCURRENCY="${CONCURRENCY:-32}"
MIX="${MIX:-all}"
MAX_ZOOM="${MAX_ZOOM:-1.0}"
HOTSPOT="${HOTSPOT:-1.0}"
PORT="${PORT:-8091}"
# 1 GB retained vs coalesce-only (0): isolates the cache's retention benefit.
CACHE_SIZES="${CACHE_SIZES:-1073741824 0}"

for bin in "$GEN" "$SERVER"; do
  [ -x "$bin" ] || { echo "missing $bin (build with the tests preset)"; exit 1; }
done

# --- generate (skip if already populated) ----------------------------------
if [ -z "$(ls -A "$TRACE_DIR" 2>/dev/null || true)" ]; then
  echo "== generating trace into $TRACE_DIR =="
  mkdir -p "$TRACE_DIR"
  "$GEN" -o "$TRACE_DIR" -p "$NUM_PROCESSES" -H "$NUM_HOSTS" \
    -e "$EPOCHS" -s "$STEPS" --num-train-files "$NUM_TRAIN_FILES" \
    --checkpoint-size "$CHECKPOINT_SIZE"
else
  echo "== reusing existing trace in $TRACE_DIR =="
fi

files=$(find "$TRACE_DIR" -name '*.pfw.gz' | wc -l | tr -d ' ')
on_disk=$(du -sh "$TRACE_DIR" | cut -f1)
# gzip -l under-reports multi-member files (it reads only the last member's
# trailer), so estimate uncompressed from one sampled file's true ratio.
sample=$(find "$TRACE_DIR" -name '*.pfw.gz' | head -1 || true)
uncompressed="?"
if [ -n "$sample" ]; then
  sc=$(wc -c < "$sample")
  su=$(gzip -dc "$sample" | wc -c)
  tot=$(find "$TRACE_DIR" -name '*.pfw.gz' -exec cat {} + | wc -c)
  uncompressed=$(awk -v su="$su" -v sc="$sc" -v tot="$tot" \
    'BEGIN{printf "%.1f GB", (sc>0 ? tot*su/sc : 0)/1073741824}')
fi
echo "files=$files  on_disk=$on_disk  uncompressed~$uncompressed"

# --- run the load under each cache setting ----------------------------------
sample_rss() {  # $1 = pid, prints peak RSS MB while the pid lives
  local pid=$1 peak=0 rss
  while kill -0 "$pid" 2>/dev/null; do
    rss=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ' || echo 0)
    [ -n "$rss" ] && [ "$rss" -gt "$peak" ] && peak=$rss
    sleep 0.5
  done
  echo $((peak / 1024))
}

for cache in $CACHE_SIZES; do
  echo ""
  echo "=================================================================="
  echo "  member-cache-size = $cache bytes"
  echo "=================================================================="
  "$SERVER" -d "$TRACE_DIR" -p "$PORT" --member-cache-size "$cache" \
    >/tmp/dft_stress_server.log 2>&1 &
  srv=$!
  trap 'kill $srv 2>/dev/null || true' EXIT

  # Wait for readiness (info endpoint answers). Generous: the first start
  # indexes the whole trace, which is minutes for a multi-GB corpus.
  for _ in $(seq 1 "${READY_TIMEOUT:-900}"); do
    curl -sf "http://127.0.0.1:$PORT/api/v1/info" >/dev/null 2>&1 && break
    kill -0 "$srv" 2>/dev/null || { echo "server died; see log:"; \
      tail -20 /tmp/dft_stress_server.log; exit 1; }
    sleep 1
  done

  rssfile=$(mktemp)
  ( sample_rss "$srv" > "$rssfile" ) &
  sampler=$!

  python3 "$DRIVER" --host 127.0.0.1 --port "$PORT" \
    --duration "$DURATION" --concurrency "$CONCURRENCY" --mix "$MIX" \
    --max-zoom "$MAX_ZOOM" --hotspot "$HOTSPOT"

  kill "$srv" 2>/dev/null || true
  wait "$srv" 2>/dev/null || true
  wait "$sampler" 2>/dev/null || true
  echo "peak server RSS: $(cat "$rssfile") MB"
  rm -f "$rssfile"
  trap - EXIT
done

echo ""
echo "done. server log: /tmp/dft_stress_server.log"
