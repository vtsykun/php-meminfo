#!/usr/bin/env bash
# Run the memory, timing and deep-structure probes against a baseline git
# revision and against the working tree, inside one container, and print both
# results side by side.
#
#   compare.sh <php-version> [baseline-rev]       (baseline defaults to origin/master)
#
# Both extensions are built from source in the container, so the comparison is
# unaffected by whatever happens to be installed in the image.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

VERSION="${1:?usage: compare.sh <php-version> [baseline-rev]}"
BASE_REV="${2:-origin/master}"
IMAGE="meminfo:php-${VERSION}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "building $IMAGE" >&2
    make -C "$REPO" docker-build PHP_VERSION="$VERSION" >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/baseline-ext"

for file in meminfo.c php_meminfo.h config.m4; do
    git -C "$REPO" show "$BASE_REV:extension/$file" > "$WORK/baseline-ext/$file"
done

docker run --rm \
    -v "$REPO/extension:/repo-ext:ro" \
    -v "$WORK/baseline-ext:/baseline-ext:ro" \
    -v "$HERE:/probes:ro" \
    "$IMAGE" \
    bash -c '
set -uo pipefail
# The image has meminfo installed; remove it so only the two builds under
# comparison are ever loaded.
rm -f /usr/local/etc/php/conf.d/docker-php-ext-meminfo.ini
rm -f "$(php-config --extension-dir)/meminfo.so"

build() {
    cp -r "$1" "$2"; cd "$2"
    phpize >/dev/null 2>&1
    ./configure --enable-meminfo >/dev/null 2>&1
    make -j"$(nproc)" >/dev/null 2>&1
    cd /
    test -f "$2/modules/meminfo.so"
}

build /baseline-ext /build-baseline || { echo "baseline build failed" >&2; exit 1; }
build /repo-ext     /build-current  || { echo "current build failed" >&2; exit 1; }

php --version | head -1

probe() {   # probe <label> <php args...>
    label="$1"
    shift

    echo "--- $label ---"

    for build in baseline current; do
        printf "  %-9s " "$build"

        # A crash is a result too, so it is reported rather than aborting.
        if output=$(timeout 600 php -n -d extension=/build-$build/modules/meminfo.so "$@" 2>&1); then
            echo "$output" | tr "\n" " "
        else
            status=$?
            echo -n "FAILED (exit $status$([ $status -eq 139 ] && echo ", SIGSEGV"))"
        fi

        echo
    done
}

probe "objects: cost to the dumped process" -d memory_limit=-1 /probes/memory.php objects 100000
probe "frames: cost to the dumped process"  -d memory_limit=-1 /probes/memory.php frames 200
probe "memory_limit headroom"               /probes/memory.php limit 20000
probe "deep object chain"                   -d memory_limit=-1 /probes/deep.php 200000 object
probe "deep nested array"                   -d memory_limit=-1 /probes/deep.php 200000 array
probe "timing: 1M objects"                  -d memory_limit=-1 /probes/memory.php objects 1000000

echo "--- signal dump (current build only: the baseline has no signal path) ---"
mkdir -p /tmp/sigdumps
printf "  current   "
if output=$(timeout 600 php -n -d extension=/build-current/modules/meminfo.so -d memory_limit=-1 \
        -d meminfo.dump_dir=/tmp/sigdumps /probes/signal.php 200000 2>&1); then
    echo "$output" | tr "\n" " "
else
    echo -n "FAILED (exit $?)"
fi
echo
rm -rf /tmp/sigdumps
'
