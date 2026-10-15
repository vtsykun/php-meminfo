# Adds ext-protobuf (and optionally ext-grpc) on top of the meminfo image to reproduce
# dumps of objects whose get_properties handler returns NULL.
#
# Prefer the Makefile wrappers: make repro / repro-test PHP_VERSION=8.1 [WITH_GRPC=1]

ARG PHP_VERSION=8.1

FROM meminfo:php-${PHP_VERSION}

# The latest pecl protobuf requires PHP >= 8.2; 4.29.3 is the last line that builds on 8.1.
ARG PROTOBUF_VERSION=4.29.3
# grpc is not needed to reproduce, and building it takes ~15 minutes.
ARG WITH_GRPC=0
ARG GRPC_VERSION=1.66.0

RUN apk add --no-cache linux-headers zlib-dev

RUN set -eux; \
    MAKEFLAGS="-j$(nproc)" pecl install "protobuf-${PROTOBUF_VERSION}"; \
    docker-php-ext-enable protobuf

RUN set -eux; \
    if [ "${WITH_GRPC}" = "1" ]; then \
        MAKEFLAGS="-j$(nproc)" pecl install "grpc-${GRPC_VERSION}"; \
        docker-php-ext-enable grpc; \
    fi; \
    php -m | grep -E '^(grpc|protobuf|meminfo)$'

COPY tools/repro/protobuf/repro.php /src/tools/repro/protobuf/repro.php

CMD ["php", "/src/tools/repro/protobuf/repro.php"]
