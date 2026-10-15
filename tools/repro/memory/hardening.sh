#!/usr/bin/env sh
# Target-side file handling, at a predictable dump path.
#
# The .phpt suite cannot cover these: meminfo_dump_process() chooses the dump id
# at random, and PHP has no sigqueue() binding to send a chosen one. The helper
# next to this script supplies it.
#
# Run inside a container built by `make docker-build`, e.g.
#   docker run --rm -v "$PWD:/src:ro" meminfo:php-8.1 sh /src/tools/repro/memory/hardening.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
EXT=${EXT:-$(php-config --extension-dir)/meminfo.so}
SIGNO=${SIGNO:-23}
DUMPS=${DUMPS:-/tmp/meminfo-hardening}

if [ ! -f "$EXT" ]; then
    echo "set EXT to a built meminfo.so (tried $EXT)" >&2
    exit 2
fi

cc -o /tmp/sigqueue "$HERE/sigqueue.c"

rm -rf "$DUMPS"
mkdir -p "$DUMPS"

cat > /tmp/repro-target.php <<'PHP'
<?php
$marker = 'hardening-marker';
file_put_contents($argv[1], getmypid());
$deadline = microtime(true) + 30;
$total = 0;
while (microtime(true) < $deadline) {
    for ($i = 0; $i < 100000; $i++) { $total += $i; }
}
PHP

start_target() {
    rm -f /tmp/repro.pid /tmp/repro.out /tmp/repro.err /tmp/repro.log
    php -n -d extension="$EXT" -d meminfo.dump_dir="$1" -d error_log=/tmp/repro.log \
        /tmp/repro-target.php /tmp/repro.pid >/tmp/repro.out 2>/tmp/repro.err &
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        [ -s /tmp/repro.pid ] && break
        sleep 0.2
    done
    sleep 0.4
    TARGET=$(cat /tmp/repro.pid)
}

report() {
    printf '  %-34s %s\n' "$1" "$2"
}

echo "=== control: an unobstructed dump ==="
start_target "$DUMPS"
/tmp/sigqueue "$TARGET" "$SIGNO" 100
sleep 1.5
report "dump created" "$([ -e "$DUMPS/meminfo-100.json" ] && echo yes || echo no)"
report "error log empty" "$([ -s /tmp/repro.log ] && echo no || echo yes)"
kill -9 "$TARGET" 2>/dev/null || true
wait 2>/dev/null || true
rm -f "$DUMPS"/*

echo "=== a symlink parked at the temporary name ==="
rm -f /tmp/repro-symlink-destination
ln -s /tmp/repro-symlink-destination "$DUMPS/meminfo-200.json.tmp"
start_target "$DUMPS"
/tmp/sigqueue "$TARGET" "$SIGNO" 200
sleep 1.5
report "written through the symlink" "$([ -e /tmp/repro-symlink-destination ] && echo YES-BAD || echo no)"
report "dump created anyway" "$([ -e "$DUMPS/meminfo-200.json" ] && echo YES-BAD || echo no)"
report "target stdout untouched" "$([ -s /tmp/repro.out ] && echo no || echo yes)"
printf '  %-34s %s\n' "logged" "$(grep -o 'meminfo: cannot create.*' /tmp/repro.log 2>/dev/null | head -1)"
kill -9 "$TARGET" 2>/dev/null || true
wait 2>/dev/null || true
rm -f "$DUMPS"/* /tmp/repro-symlink-destination

echo "=== a leftover temporary file at the same name ==="
start_target "$DUMPS"
: > "$DUMPS/meminfo-201.json.tmp"
/tmp/sigqueue "$TARGET" "$SIGNO" 201
sleep 1.5
report "leftover file still empty" "$([ -s "$DUMPS/meminfo-201.json.tmp" ] && echo no || echo yes)"
report "dump created anyway" "$([ -e "$DUMPS/meminfo-201.json" ] && echo YES-BAD || echo no)"
printf '  %-34s %s\n' "logged" "$(grep -o 'meminfo: cannot create.*' /tmp/repro.log 2>/dev/null | head -1)"
kill -9 "$TARGET" 2>/dev/null || true
wait 2>/dev/null || true
rm -f "$DUMPS"/*

echo "=== a dump that is already finished is not redone ==="
start_target "$DUMPS"
/tmp/sigqueue "$TARGET" "$SIGNO" 300
sleep 1.5
FIRST=$(stat -c %s "$DUMPS/meminfo-300.json" 2>/dev/null || echo 0)
: > "$DUMPS/marker-before-resend"
/tmp/sigqueue "$TARGET" "$SIGNO" 300
sleep 1.5
SECOND=$(stat -c %s "$DUMPS/meminfo-300.json" 2>/dev/null || echo 0)
report "resend left the dump alone" "$([ "$FIRST" = "$SECOND" ] && [ "$FIRST" != 0 ] && echo yes || echo no)"
kill -9 "$TARGET" 2>/dev/null || true
wait 2>/dev/null || true

rm -rf "$DUMPS"
echo "done"
