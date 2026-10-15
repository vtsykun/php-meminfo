# AGENTS.md

> Structural map of this repository for AI agents and new contributors. Keep it factual and update
> it when the project structure changes significantly. Detailed specification lives in
> `.ai-factory/DESCRIPTION.md` — this file does not duplicate it.

## Project Overview

PHP Meminfo is a PHP extension that dumps the content of PHP memory as JSON, plus a PHP CLI
analyzer that inspects those dumps to help track down memory leaks.

## Tech Stack

- **Programming language:** C (extension, Zend Engine API) and PHP (analyzer)
- **Framework:** none — the analyzer uses Symfony Console as a library (`^3.4 || ^4.4 || ^5.0`)
- **Database:** none — the persistence format is a JSON dump file
- **ORM:** not applicable
- **Build:** `phpize` + `config.m4` for the extension, Composer for the analyzer; optional Docker
  build (`Dockerfile`, `php:<version>-cli-alpine`) selected by `PHP_VERSION`; `CMakeLists.txt` is
  IDE-only (CLion code insight), never used to build the extension
- **Testing:** `.phpt` (extension), phpspec (analyzer)
- **Supported PHP:** 7.0 – 8.1 (CI matrix)

## Project Structure

```
extension/                    C extension for the Zend Engine
  meminfo.c                   all extension implementation
  php_meminfo.h               public prototypes, module globals, version/copyright macros
  config.m4                   phpize/autotools build definition
  dump-meminfo                CLI wrapper over meminfo_dump_process() (installed by make install-cli)
  tests/                      .phpt tests
    fixtures/                 shared PHP fixtures for tests
analyzer/                     standalone PHP CLI application (PSR-4, BitOne\ namespace)
  bin/analyzer                executable entry point
  src/BitOne/PhpMemInfo/
    Loader.php                the only reader of dump files
    Analyzer/                 all analysis logic — the layer covered by specs
    Console/                  Application + Console/Command/ presentation layer
  spec/                       phpspec specs, mirroring src/ paths
doc/                          usage guides and example scripts
.github/workflows/            CI: build extension + run both test suites on PHP 7.0–8.1
Dockerfile                    builds + enables the extension for a given PHP_VERSION (image meminfo:php-X.Y)
.dockerignore                 keeps host build artifacts and vendor/ out of the Docker build context
tools/repro/protobuf/         repro image (meminfo + ext-protobuf, optional ext-grpc) and repro.php dump check
tools/repro/memory/           probes for what a dump costs the dumped process: memory, speed, deep structures, signal path, target-side file hardening
CMakeLists.txt                IDE-only CLion project model over extension/ (Zend headers from .ide/)
CMakePresets.json             one CMake profile per PHP version (php-7.0 … php-8.1)
.ide/                         PHP headers extracted by `make ide` (git-ignored)
.ai-factory/                  AI Factory configuration and generated context
.claude/skills/               project-specific agent skills
```

Dependency direction in the analyzer: `Console/Command/*` → `Analyzer/*` + `Loader`, never the
reverse. Commands stay thin; analysis logic belongs in `Analyzer/*` so it can be specced.

## Key Entry Points

| File | Purpose |
|---|---|
| `extension/meminfo.c` | Implements `meminfo_dump()` and `meminfo_dump_process()`, the memory traversal, the buffered writer, and the signal/interrupt path that dumps on request |
| `extension/php_meminfo.h` | Exported prototypes, the writer type, module globals, `MEMINFO_VERSION`, copyright macros |
| `extension/config.m4` | Build definition — new `.c` files must be registered here |
| `extension/dump-meminfo` | `dump-meminfo <pid>` command; prints the path of the dump the target wrote |
| `analyzer/bin/analyzer` | CLI entry point for the analyzer |
| `analyzer/src/BitOne/PhpMemInfo/Console/Application.php` | Registers the console commands |
| `analyzer/src/BitOne/PhpMemInfo/Loader.php` | Single reader of dump files |
| `analyzer/composer.json` | Analyzer dependencies and PSR-4 autoloading |
| `.github/workflows/build.yaml` | CI matrix and test invocations |

## Common Commands

The root `Makefile` wraps both halves of the project. Run `make help` for the full list.

```bash
make build          # build the extension (phpize + configure + make, as needed)
make install        # install analyzer dependencies via Composer
make install-cli    # install the dump-meminfo command into $(PREFIX)/bin (default /usr/local)
make test           # both suites: .phpt for the extension, phpspec for the analyzer
make test-ext       # extension tests only
make test-one TEST=tests/dump-array.phpt   # a single .phpt test
make test-analyzer  # analyzer specs only
make lint           # php -l over the analyzer sources
make check          # lint + both test suites
make ci             # what CI runs: build + both test suites
make clean          # remove build artifacts and dependencies
make docker-build PHP_VERSION=7.4   # build image meminfo:php-7.4 with the extension enabled
make docker-test  PHP_VERSION=7.4   # both test suites inside that image
make docker-matrix  # docker-test for every version in PHP_VERSIONS (7.0 – 8.1)
make docker-shell PHP_VERSION=7.4   # bash shell in the image
make docker-clean   # remove all meminfo:php-* images
make repro          # dump ext-protobuf objects (WITH_GRPC=1 adds ext-grpc) and check the JSON
make repro-test     # .phpt suite with ext-protobuf loaded (runs the protobuf-only test)
tools/repro/memory/compare.sh 8.1   # measure a dump's cost against a baseline revision (see its README)
make ide PHP_VERSION=7.4   # extract PHP 7.4 headers into .ide/php-7.4 for CLion
make ide-clean      # remove extracted headers
```

Build against a specific PHP by overriding the toolchain variables:
`make build PHPIZE=/path/to/phpize PHP_CONFIG=/path/to/php-config PHP=/path/to/php`

The underlying commands, if you need them directly:

```bash
cd extension && phpize && ./configure --enable-meminfo && make
cd extension && REPORT_EXIT_STATUS=1 NO_INTERACTION=1 make test
cd analyzer && composer install && vendor/bin/phpspec run
```

## Documentation

| Document | Path | Description |
|---|---|---|
| README | `README.md` | Installation, compilation and usage of the extension and analyzers |
| Changelog | `CHANGELOG.md` | Released versions and notable changes |
| Leak-hunting guide | `doc/hunting_down_memory_leaks.md` | Walkthrough of a real memory-leak investigation |
| Example script | `doc/example.php` | Minimal usage example of `meminfo_dump()` |

## AI Context Files

| File | Purpose |
|---|---|
| `AGENTS.md` | This structural map of the repository |
| `.ai-factory/DESCRIPTION.md` | Project specification: stack, features, architecture notes |
| `.ai-factory/ARCHITECTURE.md` | Architecture guidelines: Structured Modules (Technical Layers), folder structure, dependency rules |
| `.ai-factory/rules/base.md` | Detected coding conventions for C and PHP in this repo |
| `.ai-factory/config.yaml` | AI Factory configuration (languages, paths, git workflow) |

## Project Skills

| Skill | Use when |
|---|---|
| `php-ext-zend-api` | Editing the C extension: zval handling, traversal, memory ownership, version guards |
| `phpt-tests` | Building the extension, writing or debugging `.phpt` tests |
| `meminfo-dump-format` | Changing or consuming the JSON dump structure on either side |

## Agent Rules

- Decompose shell commands instead of chaining them with `&&`, so each step's outcome is visible:
  - Incorrect: `git checkout master && git pull`
  - Correct: first `git checkout master`, then `git pull origin master`
- The dump file format is a contract between `extension/` and `analyzer/`. A change on one side
  requires the matching change, tests and specs on the other side in the same commit.
- All changes must keep the PHP 7.0 – 8.1 CI matrix green. Engine API differences are handled with
  `PHP_VERSION_ID` preprocessor guards, not runtime checks.
