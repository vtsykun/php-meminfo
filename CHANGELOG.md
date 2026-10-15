# Unreleased

## Bug fixes
 - `meminfo_dump()` no longer crashes on deeply nested structures. The traversal used to recurse once per level and overflowed the C stack above roughly 28 000 levels — and around 7 000 inside a `Fiber`, which has a smaller stack. A 200 000-level object chain or nested array now dumps normally
 - Dumping no longer leaves memory behind in the process being dumped. Reading an object's properties through `get_properties()` attached a property table to every object it touched, and walking the frames attached a symbol table to every frame: about 37 MB retained per 100 000 objects, and 1.3 KB per frame. Both are now read without building anything, so a dump retains nothing
 - A dump no longer fails near `memory_limit`. Its bookkeeping used to be allocated by the Zend allocator and added some 75 MB of transient peak per 100 000 objects, which hit the limit of the very process being investigated for memory use. All of it is now allocated outside the limit, and the process's resident size no longer stays at the dump's peak afterwards
 - Dumping is 30 to 35 times faster — 1M objects went from about 35 s to about 1 s — and issues three orders of magnitude fewer `write(2)` calls. The JSON escaper made 34 passes over every string it wrote; it is now a single pass, and output is buffered instead of written per field
 - Variables are attributed to the user function they belong to. A local observed while an internal function such as `array_map()` was on the stack used to be labelled `array_map()`
 - Dumping while a `Fiber` is suspended no longer crashes on PHP 8.1. The writer used to repoint `EG(current_execute_data)` at the fiber's own stack and leave it there
 - Fixes loading of the extension on PHP 7.4 (`GC_TRY_ADDREF: symbol not found`) and the wrong argument passed to `get_properties` on PHP 7.4
 - Object properties are no longer read through `__debugInfo` on PHP 7.0 – 7.3, matching the behaviour on PHP 7.4+
 - The dump stays valid JSON for objects whose `get_properties` handler returns NULL, such as ext-protobuf messages, `MapField` and `RepeatedField` (broke dumps of applications using grpc/protobuf). Such objects now have an empty `children`; the dump format is otherwise unchanged and the analyzer needs no update
 - Item identifier buffers are large enough for any 64-bit pointer, and a type without a name is dumped as `unknown` instead of crashing

## Improvement
 - Dumps the memory of an **already running process**, without changing the application. `dump-meminfo <pid>` (or `meminfo_dump_process()`) signals the target, which writes the dump at the next safe point between opcodes and never into its own output; the command prints the path of the finished file. Configured with `meminfo.dump_signal` (default `SIGURG`, whose default action is to ignore — unlike `SIGUSR2`, which php-fpm leaves lethal in its workers) and `meminfo.dump_dir`. The target must be PHP 7.1 or later, non-ZTS, on POSIX; the trigger works from any build, PHP 7.0 included. See "Dumping a running process" in the README
 - Adds a `Dockerfile` and `make docker-build` / `docker-test` / `docker-matrix` targets to build and test the extension for a chosen `PHP_VERSION`
 - Adds `make install-cli` to install the `dump-meminfo` command, and `tools/repro/memory/` with the probes used to measure what a dump costs the process it is taken from
 - Failures are reported by throwing rather than by a return value. `meminfo_dump_process()` returns the dump path or throws — `InvalidArgumentException` when the call itself is wrong, `RuntimeException` for the target or the environment; `dump-meminfo` maps those onto exit codes 2 and 1. `meminfo_dump()` now also throws `RuntimeException` when the dump cannot be written, instead of silently leaving a truncated file behind. Not a BC break: `meminfo_dump_process()` has never been released, so its `false`-returning form never shipped

# 1.1.1 PHP Meminfo v1.1.1 (2021-02-17)
Fixes segfault on symbol table not properly rebuilt.

# 1.1 PHP Meminfo v1.1 (2021-02-15)
This version drops support for PHP 5.5 and adds official supports for PHP 7.4.

Thanks a lot to @tony2001, @WyriHaximus, @vudaltsov and @ahocquard for their contibutions on making PHP Meminfo compatible with PHP .7.4!

# 1.0.4 Ygramul (2018-08-13)
 - [GITHUB-75](https://github.com/BitOne/php-meminfo/issues/75) Now goes through static members of classes as well the usual frame execution browsing.

# 1.0.3 Ygramul (2018-08-13)
 - [GITHUB-76](https://github.com/BitOne/php-meminfo/issues/76) Fixes wrong non matching item reference from children, introduced in v1.0.2

# 1.0.2 Ygramul (2018-08-08)

## Bug fixes
 - [GITHUB-70](https://github.com/BitOne/php-meminfo/issues/70) Fixes duplication of scalars in PHP 7 when multiple references are used
 - [GITHUB-68](https://github.com/BitOne/php-meminfo/issues/68) Fixes duplication of objects in PHP 7 when multiple references are used
 - [GITHUB-67](https://github.com/BitOne/php-meminfo/issues/67) Fixes wrong starting frame in PHP 7, by @mathieuk
 - [GITHUB-61](https://github.com/BitOne/php-meminfo/issues/61) Removes now obsolete Brew installation instructions

## Improvement
 - [GITHUB-73](https://github.com/BitOne/php-meminfo/issues/73) Adds explanation on memory usage difference, between what is used by objects and what is reported

# 1.0.1 Ygramul (2017-11-24)

## Bug fixes
 - [GITHUB-49](https://github.com/BitOne/php-meminfo/issues/49) Properly escape control character from array index

## Improvements
 - [GITHUB-48](https://github.com/BitOne/php-meminfo/issues/48) Adds information about Brew installation on MacOS X, by @jdecool
 - [GITHUB-47](https://github.com/BitOne/php-meminfo/issues/47) Fixes on version support, by @jdecool

# 1.0.0 Ygramul (2017-11-16)

## Improvements
 - [GITHUB-45](https://github.com/BitOne/php-meminfo/issues/45) Adds PHP 7 support
 - keeps only the dump method in the extension. From the data gathered by it, all analysis can be done.
 - summary is now done as a analyzer
 - doc separation between "usage" and "how to hunt down memory leaks"
 - tests + travis

# 0.3.0 (2016-03-23)

## Improvements
 - Remove UI
 - Adds frame and symbol information
 - Adds analyzers to find path that kept references

# 0.2.0 (2015-04-30)

## Improvements
 - Adds an feature to display instances count summary by class.

# 0.1.0 (2014-02-15)

## Improvements
 - Migrating from Google Code to GitHub
 - ADDS ABility to display a list of object with Class name and handle
