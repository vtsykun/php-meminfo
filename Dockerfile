# Builds the meminfo extension against a selectable PHP version.
#
#   docker build --build-arg PHP_VERSION=7.4 -t meminfo:php-7.4 .
#
# Prefer the Makefile wrappers: make docker-build / docker-test / docker-matrix PHP_VERSION=X.Y

ARG PHP_VERSION=8.1

FROM php:${PHP_VERSION}-cli-alpine AS base

# Kept as the first layer so that source changes never invalidate it:
# the Alpine mirrors behind old PHP tags (e.g. Alpine 3.7 for PHP 7.0) are slow.
RUN apk add --no-cache $PHPIZE_DEPS bash git make unzip

WORKDIR /src

FROM base AS build

# Version and copyright year of the build (make docker-build MEMINFO_VERSION=…).
# Empty means configure's default. Declared here, not in base, so that changing
# them never invalidates the cached toolchain layer. Kept in the environment so
# that `make test-ext` inside the image sees the same values and does not
# reconfigure.
ARG MEMINFO_VERSION=
ARG MEMINFO_COPYRIGHT_YEAR=
ENV MEMINFO_VERSION=${MEMINFO_VERSION} MEMINFO_COPYRIGHT_YEAR=${MEMINFO_COPYRIGHT_YEAR} MEMINFO_S3=no

COPY extension/ /src/extension/
COPY Makefile /src/Makefile

RUN set -eux; \
    make build; \
    make -C extension install; \
    docker-php-ext-enable meminfo; \
    php -m | grep -qx meminfo; \
    php --ri meminfo | grep -E '^(Version|Copyright)'
COPY doc/ /src/doc/

# The CLI wrapper for dumping other processes; handy for poking at a worker
# from inside the image. The process .phpt tests run it from /src/cli.
COPY cli/ /src/cli/
RUN install -m 0755 /src/cli/bin/dump-meminfo /usr/local/bin/dump-meminfo

CMD ["php", "-m"]
