MEMINFO
=======
PHP Meminfo is a PHP extension that gives you insights on the PHP memory content.

Its main goal is to help you understand memory leaks: by looking at data present in memory, you can better understand your application behaviour.

One of the main sources of inspiration for this tool is the Java jmap tool with the -histo option (see `man jmap`).

![Build](https://github.com/BitOne/php-meminfo/workflows/Build%20the%20extension%20and%20test%20the%20analyzers/badge.svg)

Compatibility
-------------
PHP 7.x, 8.0.

For older versions of PHP, you can use the following releases:
 - 5.6: PHP Meminfo v1.1
 - 5.5: PHP Meminfo v1.0.5 (may work with PHP 5.3 and PHP 5.4 but not tested)

Quick start with Make
---------------------
A `Makefile` in the repository root wraps the commands below. Run `make help` to list every target.

```bash
$ make build          # build the extension (phpize + configure + make, as needed)
$ make install        # install the analyzer dependencies via Composer
$ make test           # run both test suites (.phpt for the extension, phpspec for the analyzer)
$ make check          # lint + both test suites
$ make clean          # remove build artifacts and dependencies
```

To build against a specific PHP installation, override the toolchain variables:

```bash
$ make build PHPIZE=/usr/local/php-7.4/bin/phpize PHP_CONFIG=/usr/local/php-7.4/bin/php-config
```

The manual steps remain fully supported and are documented below.

Compilation instructions
------------------------
## Compilation
From the root of the `extension` directory:

```bash
$ phpize
$ ./configure --enable-meminfo
$ make
$ make install
```


## Docker Build


```dockerfile
RUN git clone https://github.com/VantaFinance/php-meminfo && cd php-meminfo/extension && phpize && ./configure --enable-meminfo && make && make install && echo 'extension=meminfo.so' >> /usr/local/etc/php/conf.d/docker-php-ext-meminfo.ini
```

## Build and test in Docker

To work on the extension without a local PHP toolchain, the repository `Dockerfile` builds it on top of
`php:<version>-cli-alpine`. The PHP version is selected with `PHP_VERSION` (`7.0`, `7.1`, `7.2`, `7.3`,
`7.4`, `8.0`, `8.1`; default `8.1`):

```bash
$ make docker-build PHP_VERSION=7.4   # build the image meminfo:php-7.4 with the extension enabled
$ make docker-test  PHP_VERSION=7.4   # run the .phpt and phpspec suites inside that image
$ make docker-matrix                  # docker-test for every supported PHP version
$ make docker-shell PHP_VERSION=7.4   # open a bash shell in the image
$ make docker-clean                   # remove all meminfo:php-* images
```

The extension only lives inside the image, nothing is written to the host. Use the image to take a dump of
your own script:

```bash
$ docker run --rm -v "$PWD":/app meminfo:php-8.1 php /app/script.php
```

Set `DOCKER_PLATFORM` to build for another architecture, e.g. `DOCKER_PLATFORM=linux/amd64` on Apple Silicon.

The first build of each version is slow, especially for PHP 7.0 whose Alpine mirror can take several
minutes to install the compiler toolchain; later builds reuse the cached layer.

## IDE setup (CLion)

`CMakeLists.txt` exists only to give CLion code insight into the Zend API; the extension is still built
with phpize. The PHP headers come from the Docker image of the version you want to work against:

```bash
$ make ide PHP_VERSION=8.1   # copy the PHP 8.1 headers into .ide/php-8.1/
$ make ide-clean             # remove all extracted headers
```

Then in CLion:

1. Right-click `CMakeLists.txt` → **Load CMake Project**.
2. In Settings → Build, Execution, Deployment → CMake, disable the auto-created `Debug` profile and
   enable the `php-X.Y` profile(s) from `CMakePresets.json`.
3. Select the profile matching the extracted headers. `#if PHP_VERSION_ID` branches follow that version.

When switching to a version whose headers are missing, run `make ide PHP_VERSION=X.Y` first — CMake
stops with that hint otherwise.


## Enabling the extension
Add the following line to your `php.ini`:

```ini
extension=meminfo.so
```

Installing analyzers
--------------------
Analyzers allow to analyze a memory dump (see below).

```bash
$ cd analyzer
$ composer install
```

Usage
-----
## Dumping memory content

```php
meminfo_dump(fopen('/tmp/my_dump_file.json', 'w'));

```

This function generates a dump of the PHP memory in a JSON format. This dump can be later analyzed by the provided analyzers.

This function takes a stream handle as a parameter. It allows you to specify a file (ex `fopen('/tmp/file.txt', 'w')`, as well as to use standard output with the `php://stdout` stream.

## Dumping a running process

Calling `meminfo_dump()` requires changing the application. When a worker is
already running and leaking, a dump can instead be requested from outside:

```bash
$ dump-meminfo 12345
/tmp/meminfo-1834201933.json
```

The only requirement on the target is that the extension is loaded. On
receiving the signal it records the request and raises the engine's interrupt
flag; the dump itself is written at the next safe point between two opcodes, so
the application is never interrupted mid-operation.

```bash
$ dump-meminfo <pid> [--dir=DIR] [--signal=SIG] [--timeout=SEC] [--force]
```

It prints the path of the finished dump on stdout and nothing else, so it
composes directly with the analyzer:

```bash
$ bin/analyzer summary "$(dump-meminfo 12345)"
```

The same thing is available from PHP:

```php
$path = meminfo_dump_process(12345, ['dir' => '/var/tmp', 'timeout' => 60]);
```

It returns the path of the dump, or `false` with a warning explaining which
step failed. Install the command with `make install-cli` (honours `PREFIX`).

### Settings

| Setting | Default | Meaning |
|---|---|---|
| `meminfo.dump_signal` | `SIGURG` | Signal the process dumps on, by name (with or without the `SIG` prefix) or number. `0` or empty disables it. |
| `meminfo.dump_dir` | system temp dir | Where dump files are written. |

Both are `PHP_INI_SYSTEM`, so they belong in `php.ini`:

```ini
extension=meminfo.so
meminfo.dump_signal=SIGURG
meminfo.dump_dir=/var/tmp/meminfo
```

`php -i | grep meminfo` reports the resolved signal, the directory, and whether
this build can dump on a signal at all.

#### Why `SIGURG` is the default

It is the only common signal whose default action is to *ignore* it, so sending
it to a process that happens not to be listening does nothing. `SIGUSR1` and
`SIGUSR2` look like the natural choice but are a poor one: php-fpm resets them
to `SIG_DFL` in every worker, where their default action is to **terminate**, so
a mistargeted signal would kill an idle worker. They remain available through
`meminfo.dump_signal` for anyone who needs them.

### Finding an FPM worker

Enable the status page in the pool configuration:

```ini
pm.status_path = /status
```

Then pick the worker that is actually serving the request you care about —
`?full&json` lists one entry per worker with its `pid`, `state` and
`request uri`:

```bash
$ curl -s 'http://127.0.0.1/status?full&json' | jq -r \
    '.processes[] | select(.state == "Running") | "\(.pid) \(."request uri")"'
```

**Aim at a worker, never at the FPM master.** The master interprets `SIGUSR1` as
"reopen logs" and `SIGUSR2` as "reload", and it reports both as handled, so the
safety check below cannot refuse them — sending `SIGUSR2` to the master
restarts the whole pool. Note that `pgrep -f 'php-fpm: pool www'` matches the
master as well; take the pid from the status page.

### When it refuses

Before signalling anything, the trigger checks `/proc/<pid>/status` to see
whether the target actually handles the signal. The handler exists only for the
duration of a request, so this single check catches all of: the extension is
not loaded, the target is a build that cannot serve a dump, or the worker is
idle.

```
$ dump-meminfo 12345
Warning: meminfo_dump_process(): process 12345 does not handle signal 23: ...
```

`--force` skips the check. On a system without `/proc` the check is impossible,
so only the default `SIGURG` is sent without it.

### Limitations

- **The target must be PHP 7.1 or later, non-ZTS, on POSIX.** The interrupt hook
  the dump relies on does not exist before 7.1, and under ZTS a signal arrives
  on an arbitrary thread. `dump-meminfo` and `meminfo_dump_process()` themselves
  work from any build, including PHP 7.0 — a process that cannot serve a dump
  can still ask another one for its.
- **Under FPM, only a worker that is handling a request can be dumped.** Between
  requests there is no PHP memory worth dumping and no handler installed.
- **The dump happens at the next opcode.** A target blocked in a long internal
  call or in I/O only reaches that point when the call returns. Asking for a
  dump interrupts the wait, which is visible to the application: `sleep()`
  returns early, `stream_select()` reports an interrupted system call, and
  `pcntl` dispatches any signals it had pending.
- **`max_execution_time` is paused for the duration of the dump**, so dumping a
  large heap cannot time out the request it interrupts. FPM's
  `request_terminate_timeout` and the web server's `fastcgi_read_timeout` keep
  running, though — a dump big enough to outlast them will lose the worker.
- **FPM's slowlog can swallow the signal.** It uses `ptrace` on the worker to
  collect a backtrace; a signal arriving in that window may be lost. Retry.
- **The analyzer needs memory for large dumps**, roughly a kilobyte per item:
  ```bash
  $ php -d memory_limit=-1 analyzer/bin/analyzer summary /tmp/meminfo-....json
  ```

### Security

A dump contains array keys, class and property names, string contents and heap
addresses — everything the request had in memory. Treat it as sensitive:

- Dump files are created with mode `0600`, owned by the user the target runs as.
- The temporary file is opened with `O_EXCL | O_NOFOLLOW`, so a symlink or a
  leftover file at that path is never written through or into; the attempt is
  recorded in the target's error log.
- The returned path is verified to be a regular file owned by the target before
  it is handed back.
- Requesting a dump needs permission to signal the target: the same user, or
  root. Reading the result across mount namespaces additionally needs
  `CAP_SYS_PTRACE`.
- A signal whose default action would terminate or stop the target is never
  sent to a process that is known not to handle it, not even with `--force`.
  `--force` only covers the case where that cannot be determined.
- **`meminfo.dump_dir` must not be writable by anyone you do not trust.** The
  returned path is verified before it is handed back, but anything that can
  write to that directory can replace the file afterwards. The default system
  temporary directory is shared; prefer a dedicated directory owned by the user
  the target runs as.
- Nothing from the dump path ever reaches the target's own output. Failures go
  to its error log, so a dump cannot corrupt a response or trigger a user error
  handler.

### Containers

Signals do not cross PID namespaces, so the trigger has to run in the target's:

```bash
$ docker exec <container> dump-meminfo <pid>
$ kubectl exec <pod> -c <container> -- dump-meminfo <pid>
```

An ephemeral debug container shares the PID namespace but not the filesystem,
so it will not see the dump; run inside the target container instead. With
`readOnlyRootFilesystem`, point `meminfo.dump_dir` at a writable volume (an
`emptyDir` is enough) and make sure it is large enough — a dump is roughly
800 bytes per item.

### Cleaning up

A dump is written to `<name>.json.tmp` and renamed into place only once it is
complete, so a `.json` file is always whole. A target killed mid-dump leaves its
`.tmp` behind; those are safe to delete:

```bash
$ find /tmp -maxdepth 1 -name 'meminfo-*.json.tmp' -mtime +1 -delete
```

## Displaying a summary of items in memory
```bash
$ bin/analyzer summary <dump-file>

Arguments:
  dump-file             PHP Meminfo Dump File in JSON format
```

### Example
```bash
$ bin/analyzer summary /tmp/my_dump_file.json
+----------+-----------------+-----------------------------+
| Type     | Instances Count | Cumulated Self Size (bytes) |
+----------+-----------------+-----------------------------+
| string   | 132             | 7079                        |
| MyClassA | 100             | 7200                        |
| array    | 10              | 720                         |
| integer  | 5               | 80                          |
| float    | 2               | 32                          |
| null     | 1               | 16                          |
+----------+-----------------+-----------------------------+
```

## Displaying a list of objects with the largest number of children
```bash
$ bin/analyzer top-children [options] [--] <dump-file>

Arguments:
  dump-file             PHP Meminfo Dump File in JSON format

Options:
  -l, --limit[=LIMIT]   limit [default: 5]
```

### Example
```bash
$ bin/analyzer top-children /tmp/my_dump_file.json
+-----+----------------+----------+
| Num | Item ids       | Children |
+-----+----------------+----------+
| 1   | 0x7ffff4e22fe0 | 1000000  |
| 2   | 0x7fffe780e5c8 | 11606    |
| 3   | 0x7fffe9714ef0 | 11602    |
| 4   | 0x7fffeab63ca0 | 3605     |
| 5   | 0x7fffd3161400 | 2400     |
+-----+----------------+----------+

```

## Querying the memory dump to find specific objects
```bash
$ bin/analyzer query [options] [--] <dump-file>

Arguments:
  dump-file              PHP Meminfo Dump File in JSON format

Options:
  -f, --filters=FILTERS  Filter on an attribute. Operators: =, ~. Example: class~User (multiple values allowed)
  -l, --limit=LIMIT      Number of results limit (default 10).
  -v                     Increase the verbosity
```

### Example

```bash
$ bin/analyzer query -v -f "class=MyClassA" -f "is_root=0" /tmp/php_mem_dump.json
+----------------+-------------------+------------------------------+
| Item ids       | Item data         | Children                     |
+----------------+-------------------+------------------------------+
| 0x7f94a1877008 | Type: object      | myObjectName: 0x7f94a185cca0 |
|                | Class: MyClassA   |                              |
|                | Object Handle: 1  |                              |
|                | Size: 72 B        |                              |
|                | Is root: No       |                              |
+----------------+-------------------+------------------------------+
| 0x7f94a1877028 | Type: object      | myObjectName: 0x7f94a185cde0 |
|                | Class: MyClassA   |                              |
|                | Object Handle: 2  |                              |
|                | Size: 72 B        |                              |
|                | Is root: No       |                              |
+----------------+-------------------+------------------------------+
| 0x7f94a1877048 | Type: object      | myObjectName: 0x7f94a185cf20 |
|                | Class: MyClassA   |                              |
...

```

## Displaying the reference path
The reference path is the path between a specific item in memory (identified by its
pointer address) and all the intermediary items up to the one item that is attached
to a variable still alive in the program.

This path shows which items are responsible for the memory leak of the specific item
provided.

```bash
$ bin/analyzer ref-path <item-id> <dump-file>

Arguments:
  item-id               Item Id in 0xaaaaaaaa format
  dump-file             PHP Meminfo Dump File in JSON format

Options:
  -v                     Increase the verbosity
```

### Example

```bash
$ bin/analyzer ref-path -v 0x7f94a1877068 /tmp/php_mem_dump.json
Found 1 paths
Path from 0x7f94a1856260
+--------------------+
| Id: 0x7f94a1877068 |
| Type: object       |
| Class: MyClassA    |
| Object Handle: 4   |
| Size: 72 B         |
| Is root: No        |
| Children count: 1  |
+--------------------+
         ^
         |
         3
         |
         |
+---------------------+
| Id: 0x7f94a185cb60  |
| Type: array         |
| Size: 72 B          |
| Is root: No         |
| Children count: 100 |
+---------------------+
         ^
         |
    second level
         |
         |
+--------------------+
| Id: 0x7f94a185ca20 |
| Type: array        |
| Size: 72 B         |
| Is root: No        |
| Children count: 1  |
+--------------------+
         ^
         |
    first level
         |
         |
+---------------------------+
| Id: 0x7f94a1856260        |
| Type: array               |
| Size: 72 B                |
| Is root: Yes              |
| Execution Frame: <GLOBAL> |
| Symbol Name: myRootArray  |
| Children count: 1         |
+---------------------------+
```

A workflow to find and understand memory leaks using PHP Meminfo
----------------------------------------------------------------

[Hunting down memory leaks](doc/hunting_down_memory_leaks.md)

Other memory debugging tools for PHP
-------------------------------------
 - XDebug (https://xdebug.org/)
With the trace feature and the memory delta option (tool see XDebug documentation), you can trace function memory usage. You can use the provided script to get an aggregated view (TODO link)

 - PHP Memprof (https://github.com/arnaud-lb/php-memory-profiler)
Provides aggregated data about memory usage by functions. Far less resource intensive than a full trace from XDebug.

Troubleshooting
---------------
## "A lot of memory usage is reported by the `memory_usage` entry, but the cumulative size of the items in the summary is far lower than the memory usage"

A lot of memory is used internally by the Zend Engine itself to compile PHP files, to run the virtual machine, to execute the garbage collector, etc... Another part of the memory is usually taken by PHP extensions themselves. And the remaining memory usage comes from the PHP data structures from your program.

In some cases, several hundred megabytes can be used internally by some PHP extensions. Examples are the PDO extension and MySQLi extension.
By default, when executing a SQL query they will buffer all the results inside the PHP memory:
http://php.net/manual/en/mysqlinfo.concepts.buffering.php

In case of very large number of results, this will consume a lot of memory, and this memory usage is not caused by the data you have in your objects or array manipulated by your program, but by the way the extension works.

This is only one example, but the same can happen with image manipulation extensions, that will use a lot of memory to transform images.

All the extensions are using the Zend Memory Manager, so that they will not exceed the maximum memory limit set for the PHP process. So their memory usage is included in the information provided by `memory_get_usage()`.

But PHP Meminfo is only able to get information on memory used by the data structure from the PHP program, not from the extensions themselves.

Hence the difference between those numbers, which can be quite big.

## "Call to undefined function" when calling `meminfo_dump`
This means the extension is not enabled.

Check the PHP Info output and look for the MemInfo data.

To see the PHP Info output, just create a page calling the `phpinfo();` function, and load it from your browser, or call `php -i` from the command line.

## Why most tests are "skipped"?

While doing a `make test`, some tests will need JSON capabilities. But the
compilation system generates a clean env by removing all configuration
directives that load extensions.
So if JSON capabilites are packaged as a separate extension (instead of
being compiled directly in the PHP runtime), the tests will be skipped.

You may run them with the `run-tests.php` generated after the `make test`
command, by providing the `php` executable:

```bash
$ TEST_PHP_EXECUTABLE=$(which php) $(which php) run-tests.php -d extension=$PWD/modules/meminfo.so

```
In this case your tests will run with your local PHP configuration,
including the loading of the JSON extension.

Please note this is not required when working with PHP 8 as the JSON functions are now
usually complied in PHP directly.

Credits
-------
Thanks to Derick Rethans for his inspirational work on the essential XDebug. See http://www.xdebug.org/
