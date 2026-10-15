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

# Composer 2.2 is the LTS line that still runs on PHP 7.0/7.1.
COPY --from=composer:2.2 /usr/bin/composer /usr/bin/composer

WORKDIR /src

FROM base AS build

COPY extension/ /src/extension/

RUN set -eux; \
    cd /src/extension; \
    phpize; \
    ./configure --enable-meminfo; \
    make; \
    make install; \
    docker-php-ext-enable meminfo; \
    php -m | grep -qx meminfo

COPY Makefile /src/Makefile
COPY doc/ /src/doc/
COPY analyzer/ /src/analyzer/

# The CLI wrapper for dumping other processes; handy for poking at a worker
# from inside the image.
RUN install -m 0755 /src/extension/dump-meminfo /usr/local/bin/dump-meminfo

RUN set -eux; \
    cd /src/analyzer; \
    composer install --no-interaction --no-progress

CMD ["php", "-m"]
