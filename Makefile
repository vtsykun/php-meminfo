# --- Makefile for PHP Meminfo ---
# Two deliverables: the C extension in extension/ and the PHP analyzer in analyzer/.
# Usage: make [target]

SHELL := bash
.ONESHELL:
.SHELLFLAGS := -eu -o pipefail -c
.DELETE_ON_ERROR:
MAKEFLAGS += --warn-undefined-variables
MAKEFLAGS += --no-builtin-rules

# --- Project ---
PROJECT  ?= $(shell basename $(CURDIR))
EXT_DIR  ?= extension
ANA_DIR  ?= analyzer

# --- Toolchain ---
# Override these to build against a specific PHP installation, e.g.:
#   make build PHPIZE=/usr/local/php-7.4/bin/phpize PHP_CONFIG=/usr/local/php-7.4/bin/php-config PHP=/usr/local/php-7.4/bin/php
PHP        ?= php
PHPIZE     ?= phpize
PHP_CONFIG ?= php-config
COMPOSER   ?= composer

CONFIGURE_FLAGS ?= --enable-meminfo --with-php-config=$(PHP_CONFIG)

# --- Build artifacts ---
EXT_SO      := $(EXT_DIR)/modules/meminfo.so
EXT_SOURCES := $(EXT_DIR)/meminfo.c $(EXT_DIR)/php_meminfo.h

# --- Test runner ---
# REPORT_EXIT_STATUS=1 makes failing .phpt tests return a non-zero exit code.
# NO_INTERACTION=1 suppresses the "submit report?" prompt.
TEST_ENV := REPORT_EXIT_STATUS=1 NO_INTERACTION=1

# --- Git ---
VERSION    ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
COMMIT     ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_TIME := $(shell date -u '+%Y-%m-%dT%H:%M:%SZ')

# ============================================================================
.DEFAULT_GOAL := help

##@ Build

# The phpize/configure/make chain is expressed as real file rules so that
# nothing is rebuilt unless its inputs actually changed.

$(EXT_DIR)/configure: $(EXT_DIR)/config.m4
	cd $(EXT_DIR)
	$(PHPIZE)

$(EXT_DIR)/Makefile: $(EXT_DIR)/configure
	cd $(EXT_DIR)
	./configure $(CONFIGURE_FLAGS)

$(EXT_SO): $(EXT_DIR)/Makefile $(EXT_SOURCES)
	$(MAKE) -C $(EXT_DIR)

.PHONY: build
build: $(EXT_SO) ## Build the C extension (phpize + configure + make as needed)
	@echo "Built $(EXT_SO) ($(VERSION), $(COMMIT), $(BUILD_TIME))"

.PHONY: rebuild
rebuild: clean-ext build ## Force a clean rebuild of the extension

.PHONY: install-ext
install-ext: build ## Install the extension into the PHP extension directory (may need sudo)
	$(MAKE) -C $(EXT_DIR) install
	@echo "Add 'extension=meminfo.so' to your php.ini to enable it."

.PHONY: install
install: ## Install analyzer dependencies via Composer
	cd $(ANA_DIR)
	$(COMPOSER) install

.PHONY: update
update: ## Update analyzer dependencies
	cd $(ANA_DIR)
	$(COMPOSER) update

##@ Testing

.PHONY: test
test: test-ext test-analyzer ## Run both test suites

.PHONY: test-ext
test-ext: build ## Run the .phpt test suite for the extension
	$(TEST_ENV) $(MAKE) -C $(EXT_DIR) test

.PHONY: test-one
test-one: build ## Run a single .phpt test (usage: make test-one TEST=tests/dump-array.phpt)
	@if [ -z "$(TEST)" ]; then
		echo "error: TEST is required, e.g. make test-one TEST=tests/dump-array.phpt" >&2
		exit 1
	fi
	$(TEST_ENV) $(MAKE) -C $(EXT_DIR) test TESTS="$(TEST)"

.PHONY: test-analyzer
test-analyzer: install ## Run the phpspec suite for the analyzer
	cd $(ANA_DIR)
	vendor/bin/phpspec run

##@ Code Quality

.PHONY: lint
lint: ## Syntax-check every analyzer PHP file with php -l
	find $(ANA_DIR)/src $(ANA_DIR)/spec $(ANA_DIR)/bin -type f -name '*.php' -print0 \
		| xargs -0 -n1 $(PHP) -l
	$(PHP) -l $(ANA_DIR)/bin/analyzer

.PHONY: check
check: lint test ## Run linting and both test suites

##@ Usage

# doc/example.php dumps to a hardcoded path; keep this in sync if that script changes.
EXAMPLE_DUMP := /tmp/php_mem_dump.json

.PHONY: dump-example
dump-example: build ## Run doc/example.php to produce a sample dump at /tmp/php_mem_dump.json
	$(PHP) -d extension=$(CURDIR)/$(EXT_SO) doc/example.php
	@echo "Dump written to $(EXAMPLE_DUMP)"
	@echo "Inspect it with: make summary DUMP=$(EXAMPLE_DUMP)"

.PHONY: summary
summary: install ## Show a summary of a dump file (usage: make summary DUMP=path/to/dump.json)
	@if [ -z "$(DUMP)" ]; then
		echo "error: DUMP is required, e.g. make summary DUMP=meminfo.dump" >&2
		exit 1
	fi
	$(ANA_DIR)/bin/analyzer summary "$(DUMP)"

.PHONY: analyzer
analyzer: install ## Run any analyzer command (usage: make analyzer ARGS="query dump.json --type=object")
	$(ANA_DIR)/bin/analyzer $(ARGS)

##@ CI

.PHONY: ci
ci: build test ## Run the full pipeline as CI does (build extension, both test suites)

##@ Docker

# Build and test the extension inside php:<version>-cli-alpine instead of the
# host toolchain. The extension lives only in the image $(DOCKER_TAG).
# Keep PHP_VERSIONS in sync with the matrix in .github/workflows/build.yaml.
PHP_VERSION     ?= 8.1
PHP_VERSIONS    ?= 7.0 7.1 7.2 7.3 7.4 8.0 8.1
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
	$(DOCKER) build $(DOCKER_PLATFORM_FLAG) --build-arg PHP_VERSION=$(PHP_VERSION) -t $(DOCKER_TAG) .
	@echo "Built image $(DOCKER_TAG)"

.PHONY: docker-test
docker-test: docker-build ## Run both test suites inside the image (usage: make docker-test PHP_VERSION=7.4)
	$(DOCKER) run --rm $(DOCKER_PLATFORM_FLAG) $(DOCKER_TAG) make test-ext test-analyzer

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

##@ Cleanup

.PHONY: clean-ext
clean-ext: ## Remove extension build artifacts and the generated build system
	@if [ -f $(EXT_DIR)/Makefile ]; then
		$(MAKE) -C $(EXT_DIR) clean
	fi
	cd $(EXT_DIR)
	$(PHPIZE) --clean
	rm -f tests/*.out tests/*.diff tests/*.exp tests/*.log tests/*.sh
	rm -f tests/*.php.tmp

.PHONY: clean-analyzer
clean-analyzer: ## Remove analyzer dependencies
	rm -rf $(ANA_DIR)/vendor

.PHONY: clean
clean: clean-ext clean-analyzer ## Remove all build artifacts and dependencies

##@ Help

.PHONY: help
help: ## Show this help
	@awk 'BEGIN {FS = ":.*##"; printf "Usage:\n  make \033[36m<target>\033[0m\n"} \
		/^[a-zA-Z_-]+:.*?## / {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2} \
		/^##@/ {printf "\n\033[1m%s\033[0m\n", substr($$0, 5)}' $(MAKEFILE_LIST)

# ARGS is optional (used by the analyzer target) — declare it so that
# --warn-undefined-variables stays quiet when it is not passed.
ARGS ?=
TEST ?=
DUMP ?=
