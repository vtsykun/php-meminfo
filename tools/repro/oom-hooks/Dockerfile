# Two deliberately different PHP 8.1 binaries. Build from the repository root:
# docker build --build-arg RUNTIME=legacy-runtime -f tools/repro/oom-hooks/Dockerfile .
# docker build --build-arg RUNTIME=backport-runtime -f tools/repro/oom-hooks/Dockerfile .
ARG RUNTIME=backport-runtime

FROM php:8.1.34-cli-alpine@sha256:7949370448b0b4d9787776dc5968e0fd8d48763292344b5fbf21539441228a98 AS legacy-runtime
RUN apk add --no-cache $PHPIZE_DEPS bash
ENV MEMINFO_EXPECT_ENGINE=legacy-fatal

FROM ubuntu:22.04@sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11 AS backport-runtime
ENV DEBIAN_FRONTEND=noninteractive
# Fail if this exact security-backported package is no longer available; never
# silently replace a compatibility fixture with the newest PPA binary.
ARG PHP_PACKAGE_VERSION=8.1.34-9+ubuntu22.04.1+deb.sury.org+1
RUN set -eu; apt-get update; \
    apt-get install -y --no-install-recommends software-properties-common ca-certificates gpg-agent; \
    add-apt-repository -y ppa:ondrej/php; apt-get update; \
    apt-get install -y --no-install-recommends \
        "php8.1-cli=$PHP_PACKAGE_VERSION" "php8.1-dev=$PHP_PACKAGE_VERSION" make gcc libc6-dev; \
    dpkg-query -W php8.1-cli php8.1-dev; \
    rm -rf /var/lib/apt/lists/*
ENV MEMINFO_EXPECT_ENGINE=immediate-exit

FROM backport-runtime AS fpm-runtime
ARG PHP_PACKAGE_VERSION=8.1.34-9+ubuntu22.04.1+deb.sury.org+1
RUN set -eu; apt-get update; \
    apt-get install -y --no-install-recommends "php8.1-fpm=$PHP_PACKAGE_VERSION" libfcgi-bin; \
    rm -rf /var/lib/apt/lists/*
ENV MEMINFO_FPM_BINARY=/usr/sbin/php-fpm8.1

FROM ${RUNTIME} AS build
COPY extension/ /src/extension/
COPY tools/repro/oom-hooks/verify-engine.php /src/tools/repro/oom-hooks/verify-engine.php
COPY tools/repro/oom-hooks/fault_probe.c /src/tools/repro/oom-hooks/fault_probe.c
COPY tools/repro/oom-hooks/fpm.php /src/tools/repro/oom-hooks/fpm.php
WORKDIR /src/extension
RUN set -eu; phpize; ./configure --with-meminfo-s3=no; make -j2; \
    cc -shared -fPIC -Wall -Wextra -Werror -Wno-unused-parameter $(php-config --includes) \
        /src/tools/repro/oom-hooks/fault_probe.c -o /src/oom_fault.so; \
    php -n /src/tools/repro/oom-hooks/verify-engine.php "$MEMINFO_EXPECT_ENGINE" > oom-engine.json; \
    cat oom-engine.json
ENV MEMINFO_TEST_FAULT_EXTENSION=/src/oom_fault.so
CMD ["php", "-n", "-d", "extension=/src/extension/modules/meminfo.so", "--ri", "meminfo"]
