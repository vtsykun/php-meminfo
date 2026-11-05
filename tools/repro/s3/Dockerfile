# Builds meminfo with the S3 transport (--with-meminfo-s3) against a selectable
# PHP version. aws-c-s3 and its dependencies come from vcpkg (extension/deps/vcpkg.json in the
# repository).
#
# Debian rather than Alpine on purpose: vcpkg's prebuilt tool and its ports are
# glibc-only, and the main image (Dockerfile) stays S3-less.
#
# Prefer the Makefile wrappers: make s3-build / s3-test PHP_VERSION=8.4

ARG PHP_VERSION=8.4
# cli, or fpm for the PHP-FPM scenarios (make repro-s3-fpm)
ARG PHP_VARIANT=cli

FROM php:${PHP_VERSION}-${PHP_VARIANT} AS base

# More packages for the image, e.g. valgrind (make s3-test-valgrind).
ARG EXTRA_PACKAGES=

# Debian releases past their LTS (bullseye under php:7.4, buster, stretch)
# only live on archive.debian.org.
RUN set -eux; \
    . /etc/os-release; \
    case "$VERSION_CODENAME" in stretch|buster|bullseye) \
        rm -f /etc/apt/sources.list.d/debian.sources; \
        echo "deb http://archive.debian.org/debian $VERSION_CODENAME main" > /etc/apt/sources.list; \
        echo "deb http://archive.debian.org/debian-security $VERSION_CODENAME-security main" >> /etc/apt/sources.list; \
        echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archive;; \
    esac; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        $PHPIZE_DEPS git curl ca-certificates zip unzip tar cmake ninja-build pkg-config \
        linux-libc-dev procps ${EXTRA_PACKAGES}; \
    rm -rf /var/lib/apt/lists/*; \
    docker-php-ext-install pcntl

# vcpkg needs CMake 3.21+; older Debian (bullseye under php:7.4) ships 3.18.
ARG CMAKE_VERSION=3.31.8
RUN set -eux; \
    if cmake --version | awk 'NR==1 { split($3, v, "."); exit !(v[1] < 3 || (v[1] == 3 && v[2] < 21)) }'; then \
        curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-$(uname -m).tar.gz" \
            | tar -xz -C /opt; \
        ln -sf /opt/cmake-${CMAKE_VERSION}-linux-$(uname -m)/bin/* /usr/local/bin/; \
    fi; \
    cmake --version | head -1

# Derive the checkout pin from the package manifest.
COPY extension/deps/vcpkg.json /src/extension/deps/vcpkg.json
ENV VCPKG_ROOT=/opt/vcpkg VCPKG_FORCE_SYSTEM_BINARIES=1 VCPKG_DISABLE_METRICS=1
RUN set -eux; \
    VCPKG_COMMIT=$(php -r 'echo json_decode(file_get_contents("/src/extension/deps/vcpkg.json"), true)["builtin-baseline"];'); \
    git init -q "$VCPKG_ROOT"; \
    git -C "$VCPKG_ROOT" fetch -q --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_COMMIT"; \
    git -C "$VCPKG_ROOT" checkout -q FETCH_HEAD; \
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src

# The dependency layer depends on the manifest only: source changes never
# rebuild OpenSSL and the AWS CRT. A BuildKit cache keeps vcpkg's binary
# packages across manifest changes too.
COPY Makefile /src/Makefile
RUN --mount=type=cache,target=/root/.cache/vcpkg set -eux; \
    make vcpkg-install VCPKG_ROOT="$VCPKG_ROOT"; \
    ln -s "$(ls -d /src/vcpkg_installed/*-linux)" /opt/meminfo-s3; \
    ls /opt/meminfo-s3/lib

# Kept in the environment so that `make test-ext` inside the image configures
# the same way and does not rebuild without S3.
ENV MEMINFO_S3=/opt/meminfo-s3

FROM base AS build


COPY extension/ /src/extension/
COPY Makefile /src/Makefile

RUN set -eux; \
    make build; \
    make -C extension install; \
    docker-php-ext-enable meminfo; \
    php -m | grep -qx meminfo; \
    php --ri meminfo | grep -E '^(Version|S3)'

COPY doc/ /src/doc/
COPY cli/ /src/cli/
RUN install -m 0755 /src/cli/bin/dump-meminfo /usr/local/bin/dump-meminfo
COPY tools/repro/s3/ /src/tools/repro/s3/

CMD ["php", "--ri", "meminfo"]
