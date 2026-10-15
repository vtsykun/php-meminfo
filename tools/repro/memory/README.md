# Memory and performance probes for the dump writer

These scripts measure what a dump costs the process it is taken from, and how
long it takes. They exist because the writer's contract is not something a
`.phpt` can express well: it is about *not* disturbing a process that is
already in trouble.

Unlike `tools/repro/protobuf/`, nothing here needs a special image — the
standard `meminfo:php-X.Y` image built by `make docker-build` is enough.

## Comparing against a baseline

```bash
make docker-build PHP_VERSION=8.1
tools/repro/memory/compare.sh 8.1              # vs origin/master
tools/repro/memory/compare.sh 7.4 some-tag     # vs any revision
```

`compare.sh` builds the baseline revision and the working tree inside one
container and runs every probe against both, so the two columns differ only by
the extension. The installed extension is removed inside the container first,
so neither column can accidentally measure it.

## The probes

| Script | What it answers |
|---|---|
| `memory.php objects N` | How much memory does dumping N objects leave behind, and what was the peak? |
| `memory.php frames N` | The same for N nested call frames. |
| `memory.php limit N` | Can a dump still complete with 8 MB left of `memory_limit`? |
| `deep.php DEPTH object\|array` | Does a DEPTH-level structure dump without overflowing the C stack? |
| `signal.php N` | Does a signal-triggered dump land in a 0600 file, without spending the request's CPU budget? |
| `hardening.sh` | Does the target refuse to write through a symlink or over a leftover file at the dump path? |

`signal.php` and `hardening.sh` need a target that can serve a dump: PHP 7.1 or
later, non-ZTS, POSIX.

Each script takes an extension path the usual way:

```bash
php -d extension=$PWD/extension/modules/meminfo.so \
    -d memory_limit=-1 tools/repro/memory/memory.php objects 100000
```

## Why dumps go to a real file here

Never to `php://memory`: that stream holds its buffer in the Zend allocator, so
it would be counted as memory the dump itself allocated, and it would push the
process towards `memory_limit` — the two things being measured.

## Why `hardening.sh` is a shell script and not a test

The dump file is named after an id that `meminfo_dump_process()` picks at
random, and PHP has no `sigqueue()` binding to send a chosen one. The small C
helper beside the script supplies it, which is what makes a predictable path —
and therefore these checks — possible.

## Checking for leaks

The allocator has to be out of the way for valgrind to see anything:

```bash
docker run --rm -v "$PWD:/src:ro" meminfo:php-8.1 sh -c '
  apk add --no-cache valgrind >/dev/null &&
  cp -r /src/extension /b && cd /b &&
  phpize >/dev/null && ./configure --enable-meminfo >/dev/null && make -j4 >/dev/null &&
  USE_ZEND_ALLOC=0 valgrind --leak-check=full --error-exitcode=9 \
    php -n -d extension=/b/modules/meminfo.so \
    /src/tools/repro/memory/memory.php objects 2000'
```

Expect a handful of still-reachable blocks from PHP's own startup; what matters
is that repeating the dump does not add to them.
