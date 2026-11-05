#!/usr/bin/env bash
# Benchmark meminfo_dump() of a baseline revision against the working tree with
# the host's PHP, and fail when the working tree is slower than the threshold.
#
#   compare.sh [baseline-rev] [threshold-percent]   (defaults: origin/master, 15)
#
# BENCH_ITEMS and BENCH_RUNS are passed to bench-dump.php (defaults 2000000, 5);
# BENCH_ROUNDS is how many interleaved base/head rounds to run (default 2).
# Both extensions are built from source in a temporary directory with the same
# phpize; the runs are interleaved base, head, base, head so that a noisy
# runner affects both sides alike.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

BASE_REV="${1:-origin/master}"
THRESHOLD="${2:-15}"
PHP="${PHP:-php}"
PHPIZE="${PHPIZE:-phpize}"
PHP_CONFIG="${PHP_CONFIG:-php-config}"
BENCH_ARGS=(--items="${BENCH_ITEMS:-2000000}" --runs="${BENCH_RUNS:-5}")

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

build() {   # build <extension-source-dir> <build-dir>
    cp -R "$1" "$2"
    (
        cd "$2"
        make distclean >/dev/null 2>&1 || true
        "$PHPIZE" >/dev/null
        ./configure --enable-meminfo --with-meminfo-s3=no --with-php-config="$PHP_CONFIG" >/dev/null
        make -j"$(getconf _NPROCESSORS_ONLN)" >/dev/null
    ) || { echo "error: building $1 failed" >&2; exit 1; }
    test -f "$2/modules/meminfo.so"
}

echo "bench: building $BASE_REV" >&2
git -C "$REPO" archive "$BASE_REV" extension | tar -x -C "$WORK"
mv "$WORK/extension" "$WORK/base-src"
build "$WORK/base-src" "$WORK/base"

echo "bench: building the working tree" >&2
build "$REPO/extension" "$WORK/head"

run() {   # run <side> <round>
    echo "bench: $1, round $2" >&2
    "$PHP" -n -d extension="$WORK/$1/modules/meminfo.so" -d memory_limit=-1 \
        "$HERE/bench-dump.php" "${BENCH_ARGS[@]}" > "$WORK/$1-$2.json"
}

for round in $(seq 1 "${BENCH_ROUNDS:-2}"); do
    run base "$round"
    run head "$round"
done

"$PHP" "$HERE/compare.php" "$WORK"/base-*.json -- "$WORK"/head-*.json --threshold="$THRESHOLD"
