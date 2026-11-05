# --- Makefile for PHP Meminfo ---
# The C extension in extension/ and the browser UI in ui/.
# Usage: make [target]
#
# Works with the GNU Make 3.81 that macOS ships: no .ONESHELL and no
# .SHELLFLAGS, so every recipe line is a complete shell command on its own.
# Nothing is added to MAKEFLAGS either, because MAKEFLAGS is passed down to
# the Makefile phpize generates in extension/.

SHELL := bash
.DELETE_ON_ERROR:
.SUFFIXES:

# --- Project ---
PROJECT  ?= $(shell basename $(CURDIR))
EXT_DIR  ?= extension
CLI_DIR  ?= cli

# --- Toolchain ---
# Override these to build against a specific PHP installation, e.g.:
#   make build PHPIZE=/usr/local/php-7.4/bin/phpize PHP_CONFIG=/usr/local/php-7.4/bin/php-config PHP=/usr/local/php-7.4/bin/php
PHP        ?= php
PHPIZE     ?= phpize
PHP_CONFIG ?= php-config

CONFIGURE_FLAGS ?= --enable-meminfo --with-php-config=$(PHP_CONFIG)

# --- Version and copyright year ---
# Empty means configure's default: extension/VERSION for the version, the year
# of SOURCE_DATE_EPOCH (or the current year) for the copyright notice.
#   make build MEMINFO_VERSION=2.1.0 MEMINFO_COPYRIGHT_YEAR=2026
MEMINFO_VERSION        ?=
MEMINFO_COPYRIGHT_YEAR ?=
MEMINFO_CONFIGURE_FLAGS := $(if $(MEMINFO_VERSION),--with-meminfo-version=$(MEMINFO_VERSION)) \
                           $(if $(MEMINFO_COPYRIGHT_YEAR),--with-meminfo-copyright-year=$(MEMINFO_COPYRIGHT_YEAR)) \
                           $(if $(MEMINFO_S3),--with-meminfo-s3=$(if $(filter auto yes no,$(MEMINFO_S3)),$(MEMINFO_S3),$(abspath $(MEMINFO_S3))))

# --- Optional S3 transport ---
# aws-c-s3 and its dependencies come from extension/deps/vcpkg.json:
#   make vcpkg-install && make build MEMINFO_S3=vcpkg_installed/$(VCPKG_TRIPLET)
# MEMINFO_S3 is any prefix holding include/aws/s3/s3_client.h and the static
# libraries; empty/auto prepares dependencies automatically, no disables S3.
MEMINFO_S3     ?=
VCPKG_ROOT     ?=
VCPKG          ?= $(if $(VCPKG_ROOT),$(VCPKG_ROOT)/vcpkg,vcpkg)
VCPKG_ARCH     := $(subst x86_64,x64,$(subst amd64,x64,$(subst aarch64,arm64,$(shell uname -m))))
VCPKG_TRIPLET  ?= $(VCPKG_ARCH)-$(if $(filter Darwin,$(shell uname -s)),osx,linux)

# --- Build artifacts ---
EXT_SO      := $(EXT_DIR)/modules/meminfo.so
# Same list as PHP_NEW_EXTENSION in extension/config.m4, plus the headers
EXT_SOURCES := $(addprefix $(EXT_DIR)/,meminfo.c writer.c ptr_set.c traverse.c items.c fibers.c \
                 signal.c procinfo.c trigger.c generations.c sites.c upload.c upload_s3.c \
                 internal.h php_meminfo.h)

# --- Test runner ---
# REPORT_EXIT_STATUS=1 makes failing .phpt tests return a non-zero exit code.
# NO_INTERACTION=1 suppresses the "submit report?" prompt.
TEST_ENV := REPORT_EXIT_STATUS=1 NO_INTERACTION=1

# ============================================================================
.DEFAULT_GOAL := help

##@ Build

# The phpize/configure/make chain is expressed as real file rules so that
# nothing is rebuilt unless its inputs actually changed.

$(EXT_DIR)/configure: $(EXT_DIR)/config.m4
	cd $(EXT_DIR) && $(PHPIZE)

# The configure arguments in effect, rewritten only when they change, so that
# changing MEMINFO_VERSION or MEMINFO_COPYRIGHT_YEAR reconfigures.
$(EXT_DIR)/.configure-args: FORCE
	@echo '$(strip $(CONFIGURE_FLAGS) $(MEMINFO_CONFIGURE_FLAGS))' | cmp -s - $@ \
		|| echo '$(strip $(CONFIGURE_FLAGS) $(MEMINFO_CONFIGURE_FLAGS))' > $@

$(EXT_DIR)/Makefile: $(EXT_DIR)/configure $(EXT_DIR)/.configure-args $(EXT_DIR)/VERSION $(EXT_DIR)/deps/s3-deps.sh $(EXT_DIR)/deps/vcpkg.json
	cd $(EXT_DIR) && ./configure $(CONFIGURE_FLAGS) $(MEMINFO_CONFIGURE_FLAGS)

$(EXT_SO): $(EXT_DIR)/Makefile $(EXT_SOURCES)
	$(MAKE) -C $(EXT_DIR)

.PHONY: build
build: $(EXT_SO) ## Build the C extension (usage: make build [MEMINFO_VERSION=X.Y.Z] [MEMINFO_COPYRIGHT_YEAR=YYYY] [MEMINFO_S3=auto|no|prefix])
	@echo "Built $(EXT_SO) with:"; grep -E '^#define MEMINFO_(VERSION|COPYRIGHT_YEAR|HAVE_S3) ' $(EXT_DIR)/config.h

.PHONY: FORCE
FORCE:

.PHONY: vcpkg-install
vcpkg-install: ## Install native dependencies into an explicit prefix (advanced: make vcpkg-install VCPKG_ROOT=...)
	@echo "vcpkg: installing extension/deps/vcpkg.json for $(VCPKG_TRIPLET) into vcpkg_installed/"
	$(VCPKG) install --triplet=$(VCPKG_TRIPLET) --x-manifest-root=$(CURDIR)/extension/deps --x-install-root=$(CURDIR)/vcpkg_installed
	@echo "Build with S3: make build MEMINFO_S3=vcpkg_installed/$(VCPKG_TRIPLET)"

# Sequential sub-makes, so that `make -j rebuild` cannot build while it cleans.
.PHONY: rebuild
rebuild: ## Force a clean rebuild of the extension
	$(MAKE) clean-ext
	$(MAKE) build

.PHONY: install-ext
install-ext: build ## Install the extension into the PHP extension directory (may need sudo)
	$(MAKE) -C $(EXT_DIR) install
	@echo "Add 'extension=meminfo.so' to your php.ini to enable it."

# The dump-meminfo command only needs a PHP with the extension loaded, so it is
# installed independently of the build.
PREFIX  ?= /usr/local
DESTDIR ?=

.PHONY: install-cli
install-cli: ## Install the dump-meminfo command into $(PREFIX)/bin (may need sudo)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(CLI_DIR)/bin/dump-meminfo $(DESTDIR)$(PREFIX)/bin/dump-meminfo
	@echo "Installed $(DESTDIR)$(PREFIX)/bin/dump-meminfo"

##@ Testing

.PHONY: test
test: test-ext ## Run the extension's test suite (alias of test-ext)

.PHONY: test-ext
test-ext: build ## Run the .phpt test suite for the extension
	$(TEST_ENV) $(MAKE) -C $(EXT_DIR) test

.PHONY: test-one
test-one: build ## Run a single .phpt test (usage: make test-one TEST=tests/dump-array.phpt)
	@if [ -z "$(TEST)" ]; then \
		echo "error: TEST is required, e.g. make test-one TEST=tests/dump-array.phpt" >&2; \
		exit 1; \
	fi
	$(TEST_ENV) $(MAKE) -C $(EXT_DIR) test TESTS="$(TEST)"

##@ Code Quality

# The standalone PHP scripts: examples, fixture generators and repro scripts.
LINT_DIRS := doc $(EXT_DIR)/tests/fixtures ui/tests/fixtures tools/repro tools/bench

.PHONY: lint
lint: ## Syntax-check the repository's PHP scripts with php -l
	find $(LINT_DIRS) -type f -name '*.php' -print0 | xargs -0 -n1 $(PHP) -l
	$(PHP) -l $(CLI_DIR)/bin/dump-meminfo

.PHONY: check
check: lint test ## Run linting and the extension's test suite

##@ Usage

# doc/example.php dumps to a hardcoded path; keep this in sync if that script changes.
EXAMPLE_DUMP := /tmp/php_mem_dump.json

.PHONY: dump-example
dump-example: build ## Run doc/example.php to produce a sample dump at /tmp/php_mem_dump.json
	$(PHP) -d extension=$(CURDIR)/$(EXT_SO) doc/example.php
	@echo "Dump written to $(EXAMPLE_DUMP)"
	@echo "Open it in the browser UI: make ui-dev, or https://vantafinance.github.io/php-meminfo/"

##@ Benchmark

# tools/bench/: how fast meminfo_dump() writes a ~2M-item graph. PHP is started
# with -n so an installed meminfo.so is not loaded twice; json is built in from 8.0.
BENCH_ITEMS     ?= 2000000
BENCH_RUNS      ?= 5
BENCH_BASE      ?= origin/master
BENCH_THRESHOLD ?= 15
BENCH_ROUNDS    ?= 2

.PHONY: bench-dump
bench-dump: build ## Time meminfo_dump() of a large graph (usage: make bench-dump [BENCH_ITEMS=2000000] [BENCH_RUNS=5])
	$(PHP) -n -d extension=$(CURDIR)/$(EXT_SO) -d memory_limit=-1 tools/bench/bench-dump.php \
		--items=$(BENCH_ITEMS) --runs=$(BENCH_RUNS)

.PHONY: bench-compare
bench-compare: ## Fail when the dump is slower than BENCH_BASE by more than BENCH_THRESHOLD percent
	BENCH_ITEMS=$(BENCH_ITEMS) BENCH_RUNS=$(BENCH_RUNS) BENCH_ROUNDS=$(BENCH_ROUNDS) PHP=$(PHP) PHPIZE=$(PHPIZE) PHP_CONFIG=$(PHP_CONFIG) \
		tools/bench/compare.sh $(BENCH_BASE) $(BENCH_THRESHOLD)

##@ CI

.PHONY: ci
ci: build test ## Run what CI runs for the extension (build + .phpt suite)

##@ Docker

# Build and test the extension inside php:<version>-cli-alpine instead of the
# host toolchain. The extension lives only in the image $(DOCKER_TAG).
# Keep PHP_VERSIONS in sync with the matrix in .github/workflows/build.yaml.
PHP_VERSION     ?= 8.1
PHP_VERSIONS    ?= 7.0 7.1 7.2 7.3 7.4 8.0 8.1 8.2 8.3 8.4 8.5
DOCKER          ?= docker
DOCKER_IMAGE    ?= meminfo
DOCKER_TAG      := $(DOCKER_IMAGE):php-$(PHP_VERSION)
# Empty means the native architecture, e.g. DOCKER_PLATFORM=linux/amd64
DOCKER_PLATFORM ?=
DOCKER_PLATFORM_FLAG := $(if $(DOCKER_PLATFORM),--platform $(DOCKER_PLATFORM))

.PHONY: docker-build
docker-build: ## Build image meminfo:php-X.Y with the extension (usage: make docker-build PHP_VERSION=7.4)
	@if [[ " $(PHP_VERSIONS) " != *" $(PHP_VERSION) "* ]]; then \
		echo "error: unsupported PHP_VERSION=$(PHP_VERSION), expected one of: $(PHP_VERSIONS)" >&2; \
		exit 1; \
	fi
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) --build-arg PHP_VERSION=$(PHP_VERSION) \
		--build-arg MEMINFO_VERSION=$(MEMINFO_VERSION) --build-arg MEMINFO_COPYRIGHT_YEAR=$(MEMINFO_COPYRIGHT_YEAR) \
		-t $(DOCKER_TAG) .
	@echo "Built image $(DOCKER_TAG)"

.PHONY: docker-test
docker-test: docker-build ## Run the .phpt suite inside the image (usage: make docker-test PHP_VERSION=7.4)
	$(DOCKER) run --rm $(DOCKER_PLATFORM_FLAG) $(DOCKER_TAG) make test-ext

.PHONY: docker-matrix
docker-matrix: ## Run docker-test for every version in PHP_VERSIONS
	for v in $(PHP_VERSIONS); do \
		$(MAKE) docker-test PHP_VERSION=$$v || { echo "error: docker-test failed for PHP $$v" >&2; exit 1; }; \
	done

.PHONY: docker-shell
docker-shell: docker-build ## Open a bash shell in the image (usage: make docker-shell PHP_VERSION=7.4)
	$(DOCKER) run --rm -it $(DOCKER_PLATFORM_FLAG) $(DOCKER_TAG) bash

.PHONY: docker-clean
docker-clean: ## Remove all meminfo:php-* images
	images=$$($(DOCKER) image ls --format '{{.Repository}}:{{.Tag}}' --filter 'reference=$(DOCKER_IMAGE):php-*'); \
	if [ -n "$$images" ]; then \
		$(DOCKER) image rm $$images; \
	fi

# The S3 transport: meminfo built with --with-meminfo-s3 from vcpkg in a Debian
# image (tools/repro/s3/), tested against a throwaway S3-compatible server.
S3_TAG           := $(DOCKER_IMAGE)-s3:php-$(PHP_VERSION)
S3_GATEWAY_IMAGE ?= versity/versitygw:v1.10.0
S3_TESTS         ?= tests/

.PHONY: s3-build
s3-build: ## Build the S3-enabled image meminfo-s3:php-X.Y (usage: make s3-build PHP_VERSION=8.4)
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) --build-arg PHP_VERSION=$(PHP_VERSION) \
		-f tools/repro/s3/Dockerfile -t $(S3_TAG) .
	@echo "Built image $(S3_TAG)"

.PHONY: s3-test
s3-test: s3-build ## Run the .phpt suite with S3 uploads against a local S3 server (usage: make s3-test [S3_TESTS="tests/dump-s3_*.phpt"])
	DOCKER=$(DOCKER) S3_GATEWAY_IMAGE=$(S3_GATEWAY_IMAGE) DOCKER_PLATFORM_FLAG="$(DOCKER_PLATFORM_FLAG)" \
		tools/repro/s3/run-tests.sh $(S3_TAG) $(S3_TESTS)

# The S3 tests with every child process under valgrind: CRT objects outliving
# their client, callbacks after shutdown, overruns in building the request.
# Valgrind rather than ASan: PHP loads extensions with RTLD_DEEPBIND, which an
# ASan runtime preloaded into a PHP built without it refuses.
S3_VALGRIND_TAG   := $(S3_TAG)-valgrind
S3_VALGRIND_TESTS ?= tests/dump-s3_signal.phpt tests/dump-s3_failure.phpt tests/dump-s3_no_wait.phpt tests/dump-s3_keep_local.phpt tests/dump-s3_fork.phpt

.PHONY: s3-test-valgrind
s3-test-valgrind: ## Run the S3 tests with the target processes under valgrind (usage: make s3-test-valgrind PHP_VERSION=8.4)
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) --build-arg PHP_VERSION=$(PHP_VERSION) --build-arg EXTRA_PACKAGES=valgrind \
		-f tools/repro/s3/Dockerfile -t $(S3_VALGRIND_TAG) .
	DOCKER=$(DOCKER) S3_GATEWAY_IMAGE=$(S3_GATEWAY_IMAGE) DOCKER_PLATFORM_FLAG="$(DOCKER_PLATFORM_FLAG)" \
		S3_EXTRA_ENV="USE_ZEND_ALLOC=0 ZEND_DONT_UNLOAD_MODULES=1" \
		S3_RUN='mkdir -p /tmp/vg && MEMINFO_TEST_CHILD_PREFIX="valgrind -q --trace-children=no --error-exitcode=0 --log-file=/tmp/vg/%p.log" make test-one TEST="$(S3_VALGRIND_TESTS)"; status=$$?; cat /tmp/vg/*.log; if grep -qE "Invalid (read|write|free)|uninitialised|Mismatched|ERROR SUMMARY: [1-9]" /tmp/vg/*.log; then echo "valgrind found errors" >&2; exit 1; fi; exit $$status' \
		tools/repro/s3/run-tests.sh $(S3_VALGRIND_TAG)

# PHP-FPM end to end (tools/repro/s3/fpm/run.php): per-pool targets, a dump on
# memory_limit uploaded while the worker keeps serving, responses that never
# wait for an upload, a recycled worker waiting at most the timeout, and
# clear_env without credentials.
S3_FPM_TAG := $(DOCKER_IMAGE)-s3-fpm:php-$(PHP_VERSION)

.PHONY: repro-s3-fpm
repro-s3-fpm: ## Run the PHP-FPM scenarios of the S3 transport (usage: make repro-s3-fpm PHP_VERSION=8.4)
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) --build-arg PHP_VERSION=$(PHP_VERSION) --build-arg PHP_VARIANT=fpm \
		--build-arg EXTRA_PACKAGES=libfcgi-bin -f tools/repro/s3/Dockerfile -t $(S3_FPM_TAG) .
	DOCKER=$(DOCKER) S3_GATEWAY_IMAGE=$(S3_GATEWAY_IMAGE) DOCKER_PLATFORM_FLAG="$(DOCKER_PLATFORM_FLAG)" \
		S3_RUN='php /src/tools/repro/s3/fpm/run.php' tools/repro/s3/run-tests.sh $(S3_FPM_TAG)

.PHONY: s3-shell
s3-shell: s3-build ## Open a shell in the S3 image next to a running S3 server
	DOCKER=$(DOCKER) S3_GATEWAY_IMAGE=$(S3_GATEWAY_IMAGE) S3_RUN=bash \
		tools/repro/s3/run-tests.sh $(S3_TAG)

# $(DOCKER_TAG) plus ext-protobuf (and ext-grpc with WITH_GRPC=1), see tools/repro/protobuf/.
# The pinned PROTOBUF_VERSION builds on PHP 8.1; override it for other versions.
WITH_GRPC        ?= 0
PROTOBUF_VERSION ?= 4.29.3
REPRO_TAG        := $(DOCKER_IMAGE)-repro:php-$(PHP_VERSION)$(if $(filter 1,$(WITH_GRPC)),-grpc)

.PHONY: repro-build
repro-build: docker-build ## Build the protobuf repro image (usage: make repro-build PHP_VERSION=8.1 WITH_GRPC=1)
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) \
		--build-arg PHP_VERSION=$(PHP_VERSION) \
		--build-arg PROTOBUF_VERSION=$(PROTOBUF_VERSION) \
		--build-arg WITH_GRPC=$(WITH_GRPC) \
		-f tools/repro/protobuf/Dockerfile -t $(REPRO_TAG) .
	@echo "Built image $(REPRO_TAG)"

.PHONY: repro
repro: repro-build ## Dump protobuf/grpc objects and check the dump is valid JSON
	$(DOCKER) run --rm $(DOCKER_PLATFORM_FLAG) $(REPRO_TAG)

# run-tests.php starts PHP with -n, so protobuf has to be loaded explicitly by its full path.
.PHONY: repro-test
repro-test: repro-build ## Run the .phpt suite with ext-protobuf loaded
	$(DOCKER) run --rm $(DOCKER_PLATFORM_FLAG) $(REPRO_TAG) sh -c \
		'make test-one TEST="-d extension=$$(php-config --extension-dir)/protobuf.so tests/"'

# $(DOCKER_TAG) plus ext-swoole, see tools/repro/swoole/. PHP 8.1+ only; Swoole 6
# needs 8.2, so 8.1 gets the 5.1 line.
SWOOLE_VERSION ?= $(if $(filter 8.1,$(PHP_VERSION)),5.1.8,6.2.3)
SWOOLE_TAG     := $(DOCKER_IMAGE)-swoole:php-$(PHP_VERSION)

.PHONY: repro-swoole-build
repro-swoole-build: docker-build ## Build the Swoole repro image (usage: make repro-swoole-build PHP_VERSION=8.3)
	@case "$(PHP_VERSION)" in 7.*|8.0) \
		echo "error: the Swoole repro needs PHP 8.1+, got PHP_VERSION=$(PHP_VERSION)" >&2; exit 1;; \
	esac
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) \
		--build-arg PHP_VERSION=$(PHP_VERSION) \
		--build-arg SWOOLE_VERSION=$(SWOOLE_VERSION) \
		-f tools/repro/swoole/Dockerfile -t $(SWOOLE_TAG) .
	@echo "Built image $(SWOOLE_TAG) (Swoole $(SWOOLE_VERSION))"

.PHONY: repro-swoole
repro-swoole: repro-swoole-build ## Dump coroutine processes under Swoole and check what the dumps hold
	$(DOCKER) run --rm $(DOCKER_PLATFORM_FLAG) $(SWOOLE_TAG)

.PHONY: repro-swoole-test
repro-swoole-test: repro-swoole-build ## Run the .phpt suite with ext-swoole loaded
	$(DOCKER) run --rm $(DOCKER_PLATFORM_FLAG) $(SWOOLE_TAG) sh -c \
		'make test-one TEST="-d extension=$$(php-config --extension-dir)/swoole.so tests/"'

##@ IDE

# PHP headers copied out of $(DOCKER_TAG) for code insight in CLion (see CMakeLists.txt).
# Keep the presets in CMakePresets.json in sync with PHP_VERSIONS.
IDE_DIR     ?= .ide
IDE_PHP_DIR := $(IDE_DIR)/php-$(PHP_VERSION)

.PHONY: ide
ide: docker-build ## Extract PHP headers for CLion (usage: make ide PHP_VERSION=7.4)
	@id=$$($(DOCKER) create $(DOCKER_PLATFORM_FLAG) $(DOCKER_TAG)); \
	rm -rf $(IDE_PHP_DIR); \
	mkdir -p $(IDE_PHP_DIR)/include; \
	status=0; \
	$(DOCKER) cp -q $$id:/usr/local/include/php $(IDE_PHP_DIR)/include/php || status=$$?; \
	$(DOCKER) rm $$id >/dev/null; \
	if [ $$status -ne 0 ]; then exit $$status; fi
	@echo "PHP $(PHP_VERSION) headers in $(IDE_PHP_DIR)/include/php"

.PHONY: ide-clean
ide-clean: ## Remove the PHP headers extracted for the IDE
	rm -rf $(IDE_DIR)

##@ Browser UI

# The UI is a separate Node project (Node 24+). `check` and `ci` above stay
# PHP-only, so PHP contributors never need Node.
UI_DIR ?= ui
NPM    ?= npm

.PHONY: ui-install
ui-install: ## Install the browser UI's dependencies (Node 24+)
	$(NPM) --prefix $(UI_DIR) ci

.PHONY: ui-dev
ui-dev: ## Run the browser UI on a local dev server
	$(NPM) --prefix $(UI_DIR) run dev

.PHONY: ui-build
ui-build: ## Build the browser UI into ui/dist (serve it; file:// does not work)
	$(NPM) --prefix $(UI_DIR) run build

.PHONY: ui-check
ui-check: ## Type-check the browser UI (svelte-check + tsc)
	$(NPM) --prefix $(UI_DIR) run check

.PHONY: ui-test
ui-test: ## Run the browser UI's engine tests (Vitest)
	$(NPM) --prefix $(UI_DIR) test

.PHONY: ui-e2e
ui-e2e: ## Run the browser UI's end-to-end and memory tests (Playwright, Chromium)
	$(NPM) --prefix $(UI_DIR) run test:e2e

.PHONY: ui-fixtures
ui-fixtures: ## Regenerate the UI's ui-only dump fixtures (needs PHP 8.1+ with meminfo)
	$(PHP) $(UI_DIR)/tests/fixtures/make-fixtures.php

.PHONY: clean-ui
clean-ui: ## Remove the browser UI's dependencies, build and generated test dumps
	rm -rf $(UI_DIR)/node_modules $(UI_DIR)/dist $(UI_DIR)/tests-e2e/.generated $(UI_DIR)/test-results $(UI_DIR)/playwright-report

##@ Cleanup

.PHONY: clean-ext
clean-ext: ## Remove extension build artifacts and the generated build system
	@if [ -f $(EXT_DIR)/Makefile ]; then \
		echo "Cleaning the extension build in $(EXT_DIR)"; \
		$(MAKE) -C $(EXT_DIR) clean; \
	fi
	@if command -v $(PHPIZE) >/dev/null 2>&1; then \
		echo "Removing the files phpize generated in $(EXT_DIR)"; \
		cd $(EXT_DIR) && $(PHPIZE) --clean; \
	else \
		echo "warning: $(PHPIZE) not found, the files it generated are left in $(EXT_DIR)" >&2; \
	fi
	@# What phpize --clean leaves behind: autoconf backups (configure~,
	@# config.h.in~), the include/ directory and run-tests.php output.
	rm -rf $(EXT_DIR)/include $(EXT_DIR)/*~ $(EXT_DIR)/run-tests.php $(EXT_DIR)/tmp-php.ini $(EXT_DIR)/.configure-args
	rm -f $(EXT_DIR)/tests/*.out $(EXT_DIR)/tests/*.diff $(EXT_DIR)/tests/*.exp \
		$(EXT_DIR)/tests/*.log $(EXT_DIR)/tests/*.sh $(EXT_DIR)/tests/*.mem \
		$(EXT_DIR)/tests/*.php.tmp $(EXT_DIR)/tests/*.php

.PHONY: clean
clean: clean-ext clean-ui ## Remove build artifacts and UI dependencies (native caches stay)

##@ Help

.PHONY: help
help: ## Show this help
	@awk 'BEGIN {FS = ":.*##"; printf "Usage:\n  make \033[36m<target>\033[0m\n"} \
		/^[a-zA-Z0-9_-]+:.*?## / {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2} \
		/^##@/ {printf "\n\033[1m%s\033[0m\n", substr($$0, 5)}' $(MAKEFILE_LIST)

TEST ?=
