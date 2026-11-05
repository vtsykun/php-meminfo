Dumping a running process
=========================

Calling `meminfo_dump()` requires changing the application. When a worker is already running and
leaking, a dump can instead be requested from outside, with the `dump-meminfo` command or
`meminfo_dump_process()` from PHP. This page is the full reference; the [README](../README.md#dumping-a-running-process)
has the short version.

How it works
------------

```bash
$ dump-meminfo <pid> [--dir=DIR] [--signal=SIG] [--timeout=SEC] [--force] [--count=N] [--interval=TIME] [--wait-upload]
$ dump-meminfo 12345
/tmp/meminfo-1834201933.json
```

| Option | Meaning |
|---|---|
| `--dir=DIR` | Directory to write the dump to (default: the target's own `meminfo.dump_dir`) |
| `--signal=SIG` | Signal to send, by name or number (default: this process's `meminfo.dump_signal`, normally `SIGURG`) |
| `--timeout=SEC` | Seconds to wait for the dump to start (default: 30); once started, the wait continues while it grows |
| `--force` | Send the signal even when the target does not appear to handle it |
| `--count=N` | Take N dumps in a row and print one path per line, in order (default: 1) |
| `--interval=TIME` | Wait between two dumps of a series: seconds, or a number with `s` or `m` (default: `60s`) |
| `--wait-upload` | Wait until the target has [uploaded the dump](#uploading-dumps-to-s3) and print its `s3://` location instead of the local path |

The only requirement on the target is that the extension is loaded. On receiving the signal it
records the request and raises the engine's interrupt flag; the dump itself is written at the next
safe point between two opcodes, so the application is never interrupted mid-operation.

On systems without `sigqueue()`, such as macOS, the request cannot carry an id, so the target names
the file after its pid and the time the dump started (`meminfo-12345-1792181893-19785.json`) and the
trigger looks for a file of that shape written after it asked.

Without `--dir` the target writes to its own `meminfo.dump_dir`. With it, the dump is written to
`DIR` (relative paths are resolved by the caller): the trigger leaves the directory in a request
file, `/tmp/meminfo-request-<pid>…`, which the target reads when it serves the signal and the trigger
removes afterwards. If the target cannot write there, the trigger fails at once with the target's
reason instead of waiting out `--timeout`. A target running a meminfo that predates this cannot be
told where to write; look in its `meminfo.dump_dir`.

The command prints the path of the finished dump on stdout and nothing else, so it composes with
other commands (`cp "$(dump-meminfo 12345)" ~/dumps/`). Open the dump in the
[browser UI](hunting_down_memory_leaks.md#opening-a-dump).

The same thing is available from PHP; it returns the path of the finished dump:

```php
$path = meminfo_dump_process(12345, ['dir' => '/var/tmp', 'timeout' => 60]);
```

Install the command with `make install-cli` (honours `PREFIX`, default `/usr/local`).

Errors
------

Nothing is reported by a return value — every failure throws, so a dump cannot be mistaken for a
success that happens to have produced no file:

```php
try {
    $path = meminfo_dump_process($pid);
} catch (InvalidArgumentException $e) {
    // The call itself was wrong: a pid that cannot be signalled, an unknown
    // signal name, no signal configured.
} catch (RuntimeException $e) {
    // The target or the environment: not handling the signal, no longer
    // running, timed out, a result that is not the file it should be.
}
```

`meminfo_dump()` throws `RuntimeException` as well, when the dump cannot be written to the stream it
was given, or when it is incomplete because the extension ran out of memory for its own bookkeeping
(the stream then holds valid JSON with items missing). Note that the stream holds a partial dump
either way: the bytes written before the failure are not rolled back, unlike the signal path, which
writes to a temporary file and removes it.

`dump-meminfo` maps the same distinction onto its exit code:

| Exit | Meaning |
|---|---|
| 0 | The dump was written. Its path is on stdout, and nothing else is. |
| 1 | A runtime failure — the target or the environment. |
| 2 | Bad usage, from either the command's own argument parsing or the call. |

The message always goes to stderr, so `$(dump-meminfo PID)` stays usable.

Settings
--------

| Setting | Default | Meaning |
|---|---|---|
| `meminfo.dump_signal` | `SIGURG` | Signal the process dumps on, by name (with or without the `SIG` prefix) or number. `0` or empty disables it. |
| `meminfo.dump_dir` | system temp dir | Where dump files are written unless the trigger passes `--dir` / `'dir'`. Without it, the trigger looks in its own `meminfo.dump_dir`, so both sides must agree. On macOS the system temp dir (`$TMPDIR`) differs per user — set this explicitly there, or pass `--dir`. |

Both are `PHP_INI_SYSTEM`, so they belong in `php.ini`. The other settings are described below:
[quiet points](#dumping-at-a-quiet-point-of-a-worker), [memory pressure](#dumping-on-memory-pressure),
and the [README](../README.md#settings) for dump options, generations and allocation sites.

```ini
extension=meminfo.so
meminfo.dump_signal=SIGURG
meminfo.dump_dir=/var/tmp/meminfo
```

`php -i | grep meminfo` reports the resolved signal, the directory, and whether this build can dump
on a signal at all.

### Why `SIGURG` is the default

It is the only common signal whose default action is to *ignore* it, so sending it to a process that
happens not to be listening does nothing. `SIGUSR1` and `SIGUSR2` look like the natural choice but are
a poor one: php-fpm resets them to `SIG_DFL` in every worker, where their default action is to
**terminate**, so a mistargeted signal would kill an idle worker. They remain available through
`meminfo.dump_signal` for anyone who needs them.

Finding an FPM worker
---------------------

Enable the status page in the pool configuration:

```ini
pm.status_path = /status
```

Then pick the worker that is actually serving the request you care about — `?full&json` lists one
entry per worker with its `pid`, `state` and `request uri`:

```bash
$ curl -s 'http://127.0.0.1/status?full&json' | jq -r \
    '.processes[] | select(.state == "Running") | "\(.pid) \(."request uri")"'
```

**Aim at a worker, never at the FPM master.** The master interprets `SIGUSR1` as "reopen logs" and
`SIGUSR2` as "reload", and it reports both as handled, so the safety check below cannot refuse them —
sending `SIGUSR2` to the master restarts the whole pool. Note that `pgrep -f 'php-fpm: pool www'`
matches the master as well; take the pid from the status page.

When it refuses
---------------

Before signalling anything, the trigger checks `/proc/<pid>/status` (on macOS, the kernel's process
record) to see whether the target actually handles the signal. The handler exists only for the
duration of a request, so this single check catches all of: the extension is not loaded, the target
is a build that cannot serve a dump, or the worker is idle.

```
$ dump-meminfo 12345
meminfo_dump_process(): process 12345 does not handle signal 23: ...
$ echo $?
1
```

`--force` skips the check. On a system where the check is impossible (no `/proc`, and not macOS),
only the default `SIGURG` is sent without it.

`--force` does not, however, make the trigger send a signal whose default action would terminate or
stop a target that is known not to handle it — there is no dump to be had that way, only a dead
process.

Limitations
-----------

- **The target must be PHP 7.1 or later, non-ZTS, on POSIX.** The interrupt hook the dump relies on
  does not exist before 7.1, and under ZTS a signal arrives on an arbitrary thread. `dump-meminfo` and
  `meminfo_dump_process()` themselves work from any build, including PHP 7.0 — a process that cannot
  serve a dump can still ask another one for its.
- **Under FPM, only a worker that is handling a request can be dumped.** Between requests there is no
  PHP memory worth dumping and no handler installed.
- **The dump happens at the next opcode.** A target blocked in a long internal call or in I/O only
  reaches that point when the call returns. Asking for a dump interrupts the wait, which is visible to
  the application: `sleep()` returns early, `stream_select()` reports an interrupted system call, and
  `pcntl` dispatches any signals it had pending.
- **An idle event loop never answers.** Swoole, ReactPHP or AMPHP waiting in `epoll_wait()` with
  nothing to do runs no opcode, so the request waits until a timer or a request wakes it, and
  `dump-meminfo` times out otherwise. Ask while it is busy, or keep a cheap timer ticking in workers
  you may want to dump (`Swoole\Timer::tick(1000, function () {})`). In Swoole's PROCESS mode the
  master process never runs PHP code: dump a worker instead.
- **Swoole coroutines need `swoole.enable_fiber_mock=1` (PHP 8.1+).** A dump walks the running
  coroutine and the globals; the locals of coroutines that are suspended (in `Co::sleep()`, on a
  channel, waiting for I/O) live on stacks of Swoole's own and are only reachable when Swoole reports
  its switches to the engine, which this setting enables. The parent of the dumping coroutine, while
  it is still inside `go()`, stays out of reach even then. OpenSwoole does not report its switches,
  and neither does PHP before 8.1, so there suspended coroutines are not in the dump.
- **Registered callbacks are dumped under the frame `<CALLBACK>`.** Autoloaders and `pcntl_signal()`
  handlers are kept in tables that SPL and pcntl keep private, so the dump lists them by calling
  `spl_autoload_functions()` and `pcntl_signal_get_handler()`. These internal functions run no PHP
  code, and they are not called within 256 KB of `memory_limit`. Not dumped: the context of
  `libxml_set_streams_context()` and stream notification callbacks, which have no reader.
- **`max_execution_time` is paused for the duration of the dump**, so dumping a large heap cannot time
  out the request it interrupts. FPM's `request_terminate_timeout` and the web server's
  `fastcgi_read_timeout` keep running, though — a dump big enough to outlast them will lose the worker.
- **FPM's slowlog can swallow the signal.** It uses `ptrace` on the worker to collect a backtrace; a
  signal arriving in that window may be lost. Retry.
- **Without `sigqueue()` (macOS), concurrent requests are not told apart.** Two triggers asking the
  same process at the same moment may be handed the same file, and a second one passing `--dir` while
  the first is still waiting is refused ("another dump request is pending"). Requests made one after
  the other always get separate dumps. The signal is resent at most three times, so a target writing
  somewhere the trigger cannot see produces a bounded number of dumps rather than one per second.

Security
--------

A dump contains array keys, class and property names, string contents and heap addresses —
everything the request had in memory. Treat it as sensitive:

- Dump files are created with mode `0600`, owned by the user the target runs as.
- The temporary file is opened with `O_EXCL | O_NOFOLLOW`, so a symlink or a leftover file at that
  path is never written through or into; the attempt is recorded in the target's error log.
- The returned path is verified to be a regular file with a single link, owned by the user the
  target runs as, before it is handed back. The owner comes from `/proc` or, on macOS, from the
  kernel's process record; where it cannot be determined only files of the calling user are accepted,
  and none when the caller is root. Without `sigqueue()`, files of the expected shape that fail this
  check, or are timestamped before the request or in the future, are skipped rather than returned.
- Requesting a dump needs permission to signal the target: the same user, or root. Reading the result
  across mount namespaces additionally needs `CAP_SYS_PTRACE`.
- A signal whose default action would terminate or stop the target is never sent to a process that is
  known not to handle it, not even with `--force`. `--force` only covers the case where that cannot be
  determined.
- A request file from `--dir` is honoured only if it is a regular file with a single link, not a
  symlink, and owned by the target's own user or by root — the users that may signal it anyway — and
  names an absolute directory. Anything else at that name is ignored. Whoever may ask for a dump can
  thus choose where it goes; nobody else can.
- **`meminfo.dump_dir` must not be writable by anyone you do not trust.** The returned path is
  verified before it is handed back, but anything that can write to that directory can replace the
  file afterwards. The default system temporary directory is shared; prefer a dedicated directory
  owned by the user the target runs as.
- Nothing from the dump path ever reaches the target's own output. Failures go to its error log, so a
  dump cannot corrupt a response or trigger a user error handler.

A series of dumps
-----------------

One dump shows what is in memory; three show what keeps growing. With `--count`, the command takes N
dumps of the same process, `--interval` apart, and prints their paths in order. A failure stops the
series; the dumps already taken stay.

```bash
$ dump-meminfo 12345 --count=3 --interval=5m
```

Open the files together in the browser UI. The newest is the dump; the others become its baselines.
The Growth view then shows two things:

- what grew at every step;
- the objects created between the first two dumps that are still alive in the last: leak candidates,
  grouped by where they are held.

Dumping at a quiet point of a worker
------------------------------------

A signal is served at the next safe point. That is usually in the middle of a request or a job,
so the dump also holds that job's temporaries, which blur a comparison of several dumps. With
`meminfo.dump_defer=1`, the signal only records the request. The dump is written when the worker
calls `meminfo_dump_if_requested()` between two jobs. The function returns the dump's path, or `null`
when nothing is pending. The trigger waits as usual, within its `--timeout`. A request nobody served
before the PHP request ends is logged.

```ini
meminfo.dump_defer = 1
```

| Runtime | Where to call `meminfo_dump_if_requested()` |
|---|---|
| RoadRunner | in the worker loop, after `$worker->respond(...)` |
| FrankenPHP worker mode | after the handler in the `frankenphp_handle_request()` loop, next to `gc_collect_cycles()` |
| Symfony Messenger | a listener on `WorkerRunningEvent` (fired between two messages) |
| Laravel Octane | a listener on `RequestTerminated` (or `TaskTerminated` / `TickTerminated`) |
| Plain CLI loop | at the end of each iteration |

```php
// RoadRunner
while ($request = $worker->waitRequest()) {
    $worker->respond($handler->handle($request));
    meminfo_dump_if_requested();
}
```

Dumping on memory pressure
--------------------------

A leak is often noticed only when the process dies on `memory_limit`. Two settings make the process
dump itself:

| Setting | When the dump is written |
|---|---|
| `meminfo.dump_on_limit=1` | When "Allowed memory size … exhausted" is raised. The dump is written before the fatal error ends the request, with the frames still in place. |
| `meminfo.dump_threshold=512M` (or `80%`) | Once per request, at the next safe point after memory usage passes it. With `meminfo.dump_defer`, at the next `meminfo_dump_if_requested()`. |

Both write to `meminfo.dump_dir`, under the same names and with the same `0600` permissions as signal
dumps, and log the dump's path to the error log. The memory limit is lifted while the dump is
written, as for a signal.

The dump on `memory_limit` does not wait for `meminfo.dump_defer`: the fatal error ends the request
right after it, so there is no quiet point left. It therefore holds the job that was running. Its
header says `"trigger": "memory_limit"`, and the log line says so when `dump_defer` is on. The UI warns
when such a dump, or one taken on a signal or a threshold without `dump_defer`, is part of a
comparison. The threshold has no such limit: with `dump_defer` it waits for the next
`meminfo_dump_if_requested()` (`"trigger": "threshold (deferred)"`).

The threshold compares used PHP memory (`memory_get_usage(false)`), not reserved pages or RSS.
On PHP 8.0+ it is checked around user-function calls and after completed internal calls, including
the allocating calls in a `str_repeat()` loop. The internal execution hook is installed at module
startup so traced JIT code can use it; Zend's allocator and GC remain unchanged. The hook only
requests a dump: traversal runs later through the existing VM interrupt. Operations inlined by
the VM/JIT and allocation loops with no function calls can still miss the threshold.

Empty or `0` disables threshold observation. Values are integer bytes, a K/M/G suffix, or 1..100%
of a finite positive `memory_limit`; an absolute threshold also works with an unlimited limit.
Invalid/overflowing startup settings are diagnosed and disabled. A crossing attempts capture once
per request, including a failed attempt; there is no allocation-by-allocation retry. Percentage
thresholds are recomputed when `memory_limit` changes. Before PHP 8.0 the setting is ignored.
`dump_on_limit` needs PHP 7.1 or later,
like signal dumps, and an engine that reports the memory error to Zend observers/error callbacks.

Some security-backported EOL PHP builds terminate memory-limit failures through `_exit(1)`
without those callbacks or PHP shutdown. On those builds `dump_on_limit` cannot produce the
fatal-time dump, and pending S3 uploads are not drained at shutdown. A separately configured
`dump_threshold` can capture an earlier state, but it is best effort: the observer can miss
allocations between calls, and a single large allocation may terminate before a safe point.
An early dump has trigger `threshold`, not `memory_limit`. PHP's version number alone does not
identify downstream backports. See [OOM compatibility](development.md#oom-engine-compatibility).

For early evidence on an immediate-exit engine, explicitly enable a threshold with headroom,
for example `meminfo.dump_threshold=80%`. This does not turn `dump_on_limit` into an early capture
setting. If restoring the application's memory limit after traversal fails, meminfo logs the
failure and aborts the request instead of letting it continue with a lifted limit. Ordinary
capture/write failures are logged, remove the partial file, and let the application continue.

Uploading dumps to S3
---------------------

A dump the process writes on its own — on a signal, a threshold or `memory_limit` — is a local file
first. In an ephemeral pod that file is gone with the pod, often before anybody has looked at it. With
`meminfo.dump_upload` the process also ships each finished dump to S3 or an S3-compatible store
(MinIO, Ceph, Yandex Object Storage…):

```ini
meminfo.dump_upload = s3://my-dumps/payments-api
meminfo.s3_region   = eu-central-1
```

```
s3://my-dumps/payments-api/<hostname>/meminfo-1834201933.json
```

The object key is the prefix, the host name (the pod name in Kubernetes) and the dump's file name.
The object is `application/json` and carries `x-amz-meta-meminfo-trigger`, `-pid`, `-hostname`,
`-php-version` and `-version`.

### Building with S3

The transport is built by default on native glibc Linux and macOS x86_64/arm64. The package
prepares [vcpkg](https://vcpkg.io) at the baseline in `extension/deps/vcpkg.json`, builds
[aws-c-s3](https://github.com/awslabs/aws-c-s3) (AWS Common Runtime, plain C), and links its static
libraries into `meminfo.so`. You do not need to clone vcpkg or pass a library directory:

```bash
$ pie install vanta/meminfo
$ php --ri meminfo | grep 'S3 transport'
S3 transport => yes (aws-c-s3)
```

This is the behavior of the current development revision. The published `0.2.0` package still
requires the [advanced explicit-prefix build](#advanced-explicit-prefix-builds) until a release
containing automatic preparation is available.

#### Native build prerequisites

PIE needs PHP 8.1+ to run and the target PHP's `phpize`/`php-config`. In addition to PHP development
tools, automatic S3 preparation checks git, curl, zip, unzip, tar, make, C/C++ compilers, CMake
3.21+, Ninja, pkg-config, Perl with `IPC::Cmd`, SHA512 verification support and Linux kernel headers.
For Debian/Ubuntu (install the target PHP development package separately if needed):

```bash
$ sudo apt-get update
$ sudo apt-get install -y build-essential autoconf automake libtool cmake ninja-build \
    pkg-config linux-libc-dev git curl ca-certificates zip unzip tar perl
```

On macOS, install Xcode Command Line Tools and the additional tools with Homebrew:

```bash
$ xcode-select --install
$ brew install autoconf automake libtool cmake ninja pkg-config
$ perl -MIPC::Cmd -e 1
```

Install a target PHP distribution providing development tools as well. The helper checks system
CMake/Ninja instead of downloading a replacement toolchain. It never runs apt, brew or sudo.
The initial source build needs network access and can take several minutes; vcpkg and the native
libraries are pinned by the package manifest. A prerequisite/download/build failure stops
installation; it never silently produces an extension without S3.

#### Build modes

| Mode | PIE option | Make equivalent | Native dependency downloads |
|---|---|---|---|
| Automatic (default) | omitted, or `--with-meminfo-s3=auto` | `make build` / `MEMINFO_S3=auto` | When preparation needs them |
| No S3 | `--with-meminfo-s3=no` | `make build MEMINFO_S3=no` | None |
| Existing static libraries | `--with-meminfo-s3=/absolute/prefix` | `MEMINFO_S3=/absolute/prefix` | None |

`yes` is also accepted by configure as an alias for automatic preparation. Explicit empty values
are rejected. With phpize, use the same `./configure --with-meminfo-s3=...` values. Offline explicit
prefix/no-S3 builds do not invoke vcpkg; PIE itself still needs its package source available locally.

Automatic preparation currently rejects musl/Alpine and implicit cross-compilation. Use no-S3 mode,
or supply separately prepared compatible libraries. The compiler target must match the native
architecture. Use paths without whitespace or shell metacharacters for dependency storage/prefixes;
phpize itself also rejects whitespace in build paths.

S3 upload still requires PHP 7.1+, non-ZTS and POSIX. Building S3 into the extension does not enable
uploads by itself: `meminfo.dump_upload` remains empty by default. On Linux the CRT's OpenSSL
symbols stay private to `meminfo.so`, avoiding clashes with ext/openssl and ext/curl.

#### Cache and build diagnostics

By default, workspace, pinned checkout, logs and binary archives live in `.meminfo-deps/` inside
the extension build directory. Set `MEMINFO_DEPS_CACHE` to an absolute writable directory to reuse
storage across builds; never point it at the shipped `extension/deps/` directory. Its `checkouts/`,
`archives/` and `logs/` directories serve different purposes. Installed libraries always stay in
the individual build directory's `.meminfo-deps/installed/`, so another build sharing the cache
cannot replace archives while this extension is linking them. Existing
`VCPKG_DEFAULT_BINARY_CACHE` and `VCPKG_BINARY_SOURCES` settings take precedence for binary caching.
Every build still runs vcpkg's ABI evaluation, so compiler/SDK changes are checked before reuse.
If PHP's `make clean` removed nested static libraries, the helper repairs the incomplete installed
tree and restores packages from the binary cache on the next configure.

```bash
$ MEMINFO_DEPS_CACHE=/tmp/meminfo-native-cache pie install vanta/meminfo
$ MEMINFO_DEPS_CACHE=/tmp/meminfo-native-cache MEMINFO_BUILD_VERBOSE=1 pie -v install vanta/meminfo
```

PIE hides configure progress at normal verbosity; use both `-v` and `MEMINFO_BUILD_VERBOSE=1` for
live native build output. Errors include the failing stage and full log path when storage could
be created. Logs use `logs/build.*` and are retained after failures; remove old logs when no longer
needed. A lock serializes preparation on a shared vcpkg root and reports its owner after a bounded
wait. After an interrupted build, retry; remove a stale lock only after confirming its owner stopped.
An external `VCPKG_ROOT` must be a clean checkout at the manifest baseline; the helper will not
reset it. No-S3 and explicit-prefix modes bypass automatic preparation and its prerequisite checks.

#### Advanced explicit-prefix builds

For custom/offline native dependencies, prepare a prefix explicitly from a checkout:

```bash
$ git clone https://github.com/microsoft/vcpkg ~/vcpkg
$ baseline=$(php -r 'echo json_decode(file_get_contents("extension/deps/vcpkg.json"), true)["builtin-baseline"];')
$ git -C ~/vcpkg checkout "$baseline"
$ ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
$ make vcpkg-install VCPKG_ROOT=~/vcpkg
$ make build MEMINFO_S3=vcpkg_installed/arm64-linux
```

Use the matching native triplet (`x64-linux`, `arm64-linux`, `x64-osx`, `arm64-osx`). For PIE,
pass the resulting absolute prefix: `pie install vanta/meminfo --with-meminfo-s3=/absolute/prefix`.
The prefix must contain `include/aws/s3/s3_client.h` and the static AWS CRT libraries in `lib/`.
`make s3-build` retains the explicit-prefix Debian development image.

### Settings

| Setting | Default | Meaning |
|---|---|---|
| `meminfo.dump_upload` | empty | `s3://bucket[/prefix]`. Empty keeps dumps local only. The prefix may hold letters, digits and `/._-`. |
| `meminfo.dump_upload_keep_local` | `1` | `0` removes the local file once it is uploaded. |
| `meminfo.dump_upload_timeout` | `120` | Seconds an exiting process waits for uploads still running. |
| `meminfo.s3_region` | empty | Region; falls back to `AWS_REGION`, `AWS_DEFAULT_REGION`, then `us-east-1`. |
| `meminfo.s3_endpoint` | empty | `https://host[:port]` or `http://…` of an S3-compatible store; empty is AWS. |
| `meminfo.s3_path_style` | `0` | Path-style addresses (`host/bucket/key`), which MinIO and Ceph usually need. |

All are `PHP_INI_SYSTEM`. In PHP-FPM, `meminfo.dump_upload` and `meminfo.s3_*` can differ per pool
with `php_admin_value`; `meminfo.dump_on_limit` and `meminfo.dump_threshold`, on the other hand, hook
into the engine at startup, so they have to be on in `php.ini` (a pool can only switch them off).

Credentials come from the environment: `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and, for
temporary credentials, `AWS_SESSION_TOKEN`. PHP-FPM clears the environment of its workers by default,
so pass them through the pool:

```ini
; php-fpm pool
env[AWS_ACCESS_KEY_ID] = $AWS_ACCESS_KEY_ID
env[AWS_SECRET_ACCESS_KEY] = $AWS_SECRET_ACCESS_KEY
```

Instance profiles, IRSA (web identity) and `~/.aws` profiles are not read yet. The bucket should have
default encryption on and no public access: a dump holds keys, class and property names and string
contents of the process.

### What happens

1. The dump is written to `meminfo.dump_dir` as usual, then handed to the upload.
2. The upload runs in the background, on threads of its own. **No request waits for it**: the
   request that produced the dump carries on, and in PHP-FPM the response is not held up.
3. Once it is over, a record is written next to the dump: `<dump>.json.uploaded` holding the
   `s3://` location, or `<dump>.json.upload-error` holding the reason. The outcome is also logged,
   at the next request start or end, dump, `meminfo_dump_if_requested()` or exit.
4. A process that reaches module shutdown waits up to `meminfo.dump_upload_timeout` for uploads still running, then
   gives up; their dumps stay local, with an `.upload-error`. A process killed outright (SIGKILL, the
   OOM killer) loses an upload in flight, never the local file.

A dump on `memory_limit` is uploaded too when the engine reports the fatal error and follows
normal shutdown: an FPM worker can finish the upload in the background, and the CLI waits for it
on its way out. Immediate `_exit` paths skip that drain; see the memory-pressure limitations above.

`dump-meminfo --wait-upload` (`'wait_upload' => true` for `meminfo_dump_process()`) waits for the
record after the dump and prints the `s3://` location, or fails with the target's reason. With
`meminfo.dump_upload_keep_local=0` use it: without it, the trigger prints the local path, which goes
away once the upload is done.

```bash
$ kubectl exec payments-7d9f-x2x -- dump-meminfo 1 --wait-upload
s3://my-dumps/payments-api/payments-7d9f-x2x/meminfo-1834201933.json
$ aws s3 cp s3://my-dumps/payments-api/payments-7d9f-x2x/meminfo-1834201933.json .
```

### Limits

- Uploads need a build that dumps on its own (PHP 7.1+, NTS, POSIX), like signal dumps.
- A process forked after it started uploading (`pcntl_fork()` in a CLI daemon) keeps the dumps of the
  child local and logs it once: the upload threads do not survive `fork()`, and restarting them in
  the child is not safe. Workers forked before their parent uploaded anything, as PHP-FPM's are,
  upload normally.
- The client is sized for a PHP worker: one thread, two connections, 8 MiB parts.

Containers
----------

Signals do not cross PID namespaces, so the trigger has to run in the target's:

```bash
$ docker exec <container> dump-meminfo <pid>
$ kubectl exec <pod> -c <container> -- dump-meminfo <pid>
```

An ephemeral debug container shares the PID namespace but not the filesystem, so it will not see the
dump; run inside the target container instead. With `readOnlyRootFilesystem`, point
`meminfo.dump_dir` at a writable volume (an `emptyDir` is enough) and make sure it is large enough —
a dump is roughly 800 bytes per item. To keep dumps past the pod, [upload them to S3](#uploading-dumps-to-s3).

Cleaning up
-----------

A dump is written to `<name>.json.tmp` and renamed into place only once it is complete, so a `.json`
file is always whole. A target killed mid-dump leaves its `.tmp` behind; those are safe to delete:

```bash
$ find /tmp -maxdepth 1 -name 'meminfo-*.json.tmp' -mtime +1 -delete
```

With uploads, the `.json.uploaded` and `.json.upload-error` records next to the dumps are small and
safe to delete together with them.
