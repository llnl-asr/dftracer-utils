#!/usr/bin/env bash
#
# Keep dependency installs in the sccache bucket, because the GitHub cache
# holds only 10 GB per repository.
#
#   deps_cache.sh restore <kind> <dir>
#   deps_cache.sh save <kind> <dir>
#
# The object is named by <kind>, the runner image and the content of
# Dependencies.cmake and build_rocksdb.sh. That key only picks a candidate: CMake reuses an install
# only when its own fingerprint matches, so a wrong candidate is rebuilt.
# A cache problem is never an error, since the build then makes the install.

set -uo pipefail

cmd="${1:?restore or save}"
kind="${2:?kind}"
dir="${3:?directory}"

if [ -z "${SCCACHE_BUCKET:-}" ] || [ -z "${SCCACHE_ENDPOINT:-}" ] ||
	! command -v aws >/dev/null 2>&1; then
	echo "deps cache: no bucket or no aws CLI, skipping ${cmd}"
	exit 0
fi

# R2 rejects the checksum headers that newer AWS CLI versions add.
export AWS_DEFAULT_REGION=auto
export AWS_REQUEST_CHECKSUM_CALCULATION=when_required
export AWS_RESPONSE_CHECKSUM_VALIDATION=when_required

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
hash="$(cat "$root/cmake/modules/Dependencies.cmake" "$root/scripts/ci/build_rocksdb.sh" | git hash-object --stdin | cut -c1-16)"
name="deps/${kind}-${ImageOS:-${RUNNER_OS:-local}}-${RUNNER_ARCH:-$(uname -m)}-${hash}.tar.gz"
s3() { aws --endpoint-url "$SCCACHE_ENDPOINT" "$@"; }
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

case "$cmd" in
restore)
	mkdir -p "$dir"
	# Download whole and test it first: a cut-off archive must not leave a
	# half-written library next to a fingerprint that looks complete.
	if s3 s3 cp "s3://${SCCACHE_BUCKET}/${name}" "$tmp" --only-show-errors &&
		gzip -t "$tmp" 2>/dev/null && tar -xzf "$tmp" -C "$dir"; then
		echo "deps cache: restored ${name}"
	else
		echo "deps cache: no usable ${name}"
	fi
	;;
save)
	if [ -z "$(ls -A "$dir" 2>/dev/null)" ]; then
		echo "deps cache: nothing to save in ${dir}"
	elif s3 s3api head-object --bucket "$SCCACHE_BUCKET" --key "$name" >/dev/null 2>&1; then
		echo "deps cache: ${name} is already stored"
	elif tar -czf "$tmp" -C "$dir" . &&
		s3 s3 cp "$tmp" "s3://${SCCACHE_BUCKET}/${name}" --only-show-errors; then
		echo "deps cache: stored ${name} ($(du -h "$tmp" | cut -f1))"
	else
		echo "deps cache: could not store ${name}"
	fi
	;;
*)
	echo "usage: $0 restore|save <kind> <dir>" >&2
	exit 2
	;;
esac
exit 0
