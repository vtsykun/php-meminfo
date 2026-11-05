#!/bin/sh
# Called by configure, not by PIE directly. Stdout is the installed prefix only.
set -eu

fail() {
    printf 'ERROR [meminfo.deps] %s\n' "$*" >&2
    if [ -n "${log_file:-}" ]; then
        printf 'ERROR [meminfo.deps] %s\n' "$*" >> "$log_file" 2>/dev/null || :
        printf 'Full build log: %s\nRetry with: MEMINFO_BUILD_VERBOSE=1 pie -v install vanta/meminfo\n' "$log_file" >&2
    fi
    exit 1
}

check_path() {
    case "$1" in
        ''|*[![:graph:]]*|*\'*|*\"*|*\`*|*\$*|*\;*|*\&*|*\|*|*\(*|*\)*|*\<*|*\>*|*\\*)
            fail 'dependency paths must not contain whitespace or shell metacharacters' ;;
    esac
}

log() {
    printf '%s [meminfo.deps] %s\n' "$1" "$2" >> "$log_file"
    if [ "$1" != DEBUG ] || [ "${MEMINFO_BUILD_VERBOSE:-0}" = 1 ]; then
        printf '%s [meminfo.deps] %s\n' "$1" "$2" >&2
    fi
}

run() {
    stage=$1
    shift
    log INFO "$stage"
    if "$@" >> "$log_file" 2>&1; then
        return
    else
        result=$?
        tail -n 30 "$log_file" >&2
        fail "$stage failed (exit $result)"
    fi
}

cleanup() {
    if [ -n "${tail_pid:-}" ]; then kill "$tail_pid" 2>/dev/null || :; fi
    if [ "${locked:-0}" = 1 ]; then
        rm -f "$lock_dir/owner"
        rmdir "$lock_dir" 2>/dev/null || :
    fi
}

prefix_complete() {
    [ -f "$1/include/aws/s3/s3_client.h" ] || return 1
    for archive in aws-c-s3 aws-c-auth aws-c-http aws-c-io aws-c-compression \
        aws-c-cal aws-c-sdkutils aws-checksums aws-c-common; do
        [ -f "$1/lib/lib$archive.a" ] || return 1
    done
}

[ "$#" = 4 ] || fail 'usage: s3-deps.sh SOURCE_DIR BUILD_DIR HOST COMPILER_ARCH'
source_dir=$1
build_dir=$2
host=$3
compiler_arch=$4
check_path "$source_dir"
check_path "$build_dir"
source_dir=$(cd "$source_dir" && pwd -P)
build_dir=$(cd "$build_dir" && pwd -P)
manifest_dir="$source_dir/deps"
manifest="$manifest_dir/vcpkg.json"
cache_dir=${MEMINFO_DEPS_CACHE:-$build_dir/.meminfo-deps}
check_path "$cache_dir"
mkdir -p "$cache_dir/logs" || fail "cannot create dependency workspace $cache_dir"
cache_dir=$(cd "$cache_dir" && pwd -P)
log_file=$(mktemp "$cache_dir/logs/build.XXXXXX")
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

case "$(uname -m)" in
    x86_64|amd64) arch=x64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) fail 'automatic S3 dependencies support native x86_64 and arm64 only; use --with-meminfo-s3=no or an explicit prefix' ;;
esac
[ "$compiler_arch" = "$arch" ] || fail "compiler target $compiler_arch differs from native $arch; automatic cross-compilation is unsupported"
case "$(uname -s):$host" in
    Linux:*-linux*)
        getconf GNU_LIBC_VERSION >/dev/null 2>&1 || fail 'automatic S3 dependencies require glibc Linux; use --with-meminfo-s3=no or an explicit prefix on musl'
        target_os=linux ;;
    Darwin:*-darwin*) target_os=osx ;;
    *) fail "unsupported automatic target $host; use --with-meminfo-s3=no or an explicit prefix" ;;
esac
triplet="$arch-$target_os"
log INFO "target=$triplet; log=$log_file"

for tool in git curl zip unzip tar make cmake ninja pkg-config perl; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing $tool; install the native build prerequisites (see doc/process_dump.md)"
done
command -v "${CXX:-c++}" >/dev/null 2>&1 || fail 'missing C++ compiler (CXX must name an executable)'
if ! command -v sha512sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    fail 'missing SHA512 verifier: install sha512sum or shasum'
fi
perl -MIPC::Cmd -e 1 >/dev/null 2>&1 || fail 'Perl module IPC::Cmd is required by OpenSSL'
cmake_version=$(cmake --version | sed -n '1s/.*version //p')
if ! printf '%s\n' "$cmake_version" | awk -F. '{exit !($1 > 3 || ($1 == 3 && $2 >= 21))}'; then
    fail "CMake >=3.21 required; found $cmake_version"
fi
if [ "$target_os" = linux ]; then
    [ -f /usr/include/linux/version.h ] || fail 'missing Linux kernel headers; install linux-libc-dev'
fi
baseline=$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$manifest")
[ "${#baseline}" = 40 ] || fail "invalid builtin-baseline in $manifest"
log DEBUG "baseline=$baseline; compiler=${CC:-cc}; cmake=$cmake_version; workspace=$cache_dir"

if [ -n "${VCPKG_ROOT:-}" ]; then
    check_path "$VCPKG_ROOT"
    vcpkg_root=$(cd "$VCPKG_ROOT" && pwd -P) || fail 'VCPKG_ROOT does not exist'
    external_root=1
else
    vcpkg_root="$cache_dir/checkouts/$baseline"
    mkdir -p "$vcpkg_root"
    vcpkg_root=$(cd "$vcpkg_root" && pwd -P)
    external_root=0
fi
lock_dir="$vcpkg_root/.meminfo-build-lock"
waited=0
while ! mkdir "$lock_dir" 2>/dev/null; do
    [ -d "$lock_dir" ] || fail "cannot lock vcpkg root $vcpkg_root (check permissions)"
    if [ "$waited" -ge 120 ]; then
        owner=$(cat "$lock_dir/owner" 2>/dev/null || printf unknown)
        fail "vcpkg root is busy: $lock_dir (owner $owner); check the owner before removing a stale lock"
    fi
    if [ "$waited" = 0 ]; then log INFO "waiting for vcpkg root lock $lock_dir"; fi
    sleep 1
    waited=$((waited + 1))
done
locked=1
printf '%s:%s\n' "$(uname -n)" "$$" > "$lock_dir/owner"

if [ "${MEMINFO_BUILD_VERBOSE:-0}" = 1 ]; then
    tail -n 0 -f "$log_file" >&2 &
    tail_pid=$!
fi
if [ "$external_root" = 0 ] && [ ! -e "$vcpkg_root/.git" ]; then
    # rev-parse searches parent directories: a cache inside the extension's
    # checkout is not itself a vcpkg repository until it has its own .git.
    log DEBUG '[FIX:vcpkg-root] creating an owned repository before resolving HEAD'
    run 'initialize pinned vcpkg checkout' git init -q "$vcpkg_root"
fi
checkout_root=$(git -C "$vcpkg_root" rev-parse --show-toplevel) || fail 'VCPKG_ROOT must be the root of a git checkout'
checkout_root=$(cd "$checkout_root" && pwd -P)
[ "$checkout_root" = "$vcpkg_root" ] || fail "VCPKG_ROOT is inside another repository ($checkout_root); specify the vcpkg checkout root"
log DEBUG "[FIX:vcpkg-root] verified repository root $checkout_root"
if [ "$external_root" = 0 ]; then
    if ! git -C "$vcpkg_root" rev-parse --verify HEAD >/dev/null 2>&1; then
        run "download vcpkg baseline $baseline" git -C "$vcpkg_root" fetch -q --depth 1 https://github.com/microsoft/vcpkg.git "$baseline"
        run 'select pinned vcpkg baseline' git -C "$vcpkg_root" checkout -q --detach FETCH_HEAD
    fi
fi
revision=$(git -C "$vcpkg_root" rev-parse HEAD) || fail 'VCPKG_ROOT must be a git checkout at the package baseline'
[ "$revision" = "$baseline" ] || fail "VCPKG_ROOT revision $revision differs from required $baseline; it will not be reset"
[ -z "$(git -C "$vcpkg_root" status --porcelain --untracked-files=no)" ] || fail 'VCPKG_ROOT contains modified tracked files; use a clean pinned checkout'
export VCPKG_DISABLE_METRICS=1 VCPKG_FORCE_SYSTEM_BINARIES=1
if [ ! -x "$vcpkg_root/vcpkg" ] || [ ! -f "$vcpkg_root/.meminfo-bootstrap-ready" ]; then
    if [ "$external_root" = 1 ] && [ -x "$vcpkg_root/vcpkg" ]; then
        run 'verify external vcpkg executable' "$vcpkg_root/vcpkg" version
    else
        run 'bootstrap vcpkg (pinned tool checksums)' sh "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
    fi
    if [ "$external_root" = 0 ]; then
        run 'verify bootstrapped vcpkg executable' "$vcpkg_root/vcpkg" version
        printf '%s\n' "$baseline" > "$vcpkg_root/.meminfo-bootstrap-ready"
    fi
fi
if [ -z "${VCPKG_DEFAULT_BINARY_CACHE:-}" ] && [ -z "${VCPKG_BINARY_SOURCES:-}" ]; then
    VCPKG_DEFAULT_BINARY_CACHE="$cache_dir/archives"
    mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"
    export VCPKG_DEFAULT_BINARY_CACHE
fi
# The returned archives remain in use after this helper releases the root lock.
# Keep the installed tree local to this build; another build sharing the cache
# must not replace its libraries while configure/make are still linking them.
install_root="$build_dir/.meminfo-deps/installed/$baseline"
# PHP's generated `make clean` recursively deletes *.a, including libraries
# inside this owned workspace. Installed-package metadata alone cannot detect
# that deletion. Drop incomplete installation metadata and restore from cache.
if [ -d "$install_root" ] && ! prefix_complete "$install_root/$triplet"; then
    log WARN 'repairing incomplete native install tree (possibly removed by make clean)'
    rm -rf "$install_root"
fi
mkdir -p "$install_root"
# vcpkg performs the ABI/cache evaluation on every invocation, even on a warm build.
run "install AWS CRT for $triplet (vcpkg ABI/cache evaluation)" "$vcpkg_root/vcpkg" install \
    --triplet="$triplet" --x-manifest-root="$manifest_dir" --x-install-root="$install_root"
prefix="$install_root/$triplet"
[ -f "$prefix/include/aws/s3/s3_client.h" ] || fail 'AWS S3 header missing after vcpkg install'
for library in aws-c-s3 aws-c-auth aws-c-http aws-c-io aws-c-compression \
    aws-c-cal aws-c-sdkutils aws-checksums aws-c-common; do
    [ -f "$prefix/lib/lib$library.a" ] || fail "static lib$library.a missing after vcpkg install"
done
log INFO "S3 dependencies ready: $prefix"
printf '%s\n' "$prefix"
