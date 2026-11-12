# SPDX-License-Identifier: MIT
SECONDS=0

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <N> <outfile> [num_workers]" >&2
  exit 1
fi
N="$1"
outfile="$2"
num_workers="${3:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

lines_per_worker=$((N / num_workers))
remainder=$((N % num_workers))

part_files=()
for ((w=0; w<num_workers; w++)); do
  start=$((w * lines_per_worker + 1))
  end=$((start + lines_per_worker - 1))
  if ((w == num_workers - 1)); then
    end=$((end + remainder))
  fi
  part_file="$tmpdir/part${w}"
  awk -v start="$start" -v end="$end" 'BEGIN { srand(); for (i=start; i<=end; i++) { name="name_"i; cat="cat_"i; dur=int(rand()*1000); print "{\"name\":\""name"\",\"cat\":\""cat"\",\"dur\":"dur"}" } }' > "$part_file" &
  part_files+=("$part_file")
done
wait

{
  echo "["
  for pf in "${part_files[@]}"; do
    cat "$pf"
  done
  echo "]"
} > "$outfile"

# if command -v pigz >/dev/null 2>&1; then
#   pigz "$outfile"
# else
#   gzip "$outfile"
# fi

echo "Elapsed time: ${SECONDS}s"
