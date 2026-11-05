PHP_ARG_ENABLE(meminfo, whether to enable Memory Information support,
[ --enable-meminfo   Enable Meminfo support])

PHP_ARG_WITH(meminfo-version, for the version meminfo reports,
[  --with-meminfo-version=VER
                          Version reported by phpversion('meminfo') and phpinfo()
                          (default: the content of extension/VERSION)], no, no)

PHP_ARG_WITH(meminfo-s3, for the S3 dump transport,
[  --with-meminfo-s3=MODE  S3 dependencies: auto (default), no, or an existing
                          static AWS CRT install prefix], auto, no)

PHP_ARG_WITH(meminfo-copyright-year, for the meminfo copyright year,
[  --with-meminfo-copyright-year=YYYY
                          Last year of the copyright notice (default: the year of
                          SOURCE_DATE_EPOCH when set, else the current year)], no, no)

if test "$PHP_MEMINFO" = "yes"; then
  AC_DEFINE(HAVE_MEMINFO, 1, [Whether you have Memory Info])

  dnl The version becomes a C string literal: no quotes, backslashes or spaces.
  meminfo_version="$PHP_MEMINFO_VERSION"
  if test "$meminfo_version" = "no" || test -z "$meminfo_version"; then
    meminfo_version=`cat "PHP_EXT_SRCDIR(meminfo)/VERSION" 2>/dev/null | tr -d ' \r\n'`
  fi
  if test -z "$meminfo_version" || test "$meminfo_version" = "yes" \
     || echo "$meminfo_version" | grep '[[^0-9A-Za-z.+~-]]' >/dev/null; then
    AC_MSG_ERROR([invalid meminfo version '$meminfo_version': use only letters, digits and . + ~ -])
  fi

  meminfo_year="$PHP_MEMINFO_COPYRIGHT_YEAR"
  if test "$meminfo_year" = "no" || test -z "$meminfo_year"; then
    if test -n "$SOURCE_DATE_EPOCH"; then
      dnl GNU and BusyBox date take -d @N, BSD date takes -r N.
      meminfo_year=`date -u -d "@$SOURCE_DATE_EPOCH" +%Y 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH" +%Y 2>/dev/null`
    else
      meminfo_year=`date -u +%Y`
    fi
  fi
  if echo "$meminfo_year" | grep '^[[0-9]][[0-9]][[0-9]][[0-9]]$' >/dev/null; then :; else
    AC_MSG_ERROR([invalid meminfo copyright year '$meminfo_year': expected four digits])
  fi

  AC_MSG_CHECKING([meminfo version and copyright year])
  AC_MSG_RESULT([$meminfo_version, $meminfo_year])
  AC_DEFINE_UNQUOTED(MEMINFO_VERSION, ["$meminfo_version"], [Version reported by the extension])
  AC_DEFINE_UNQUOTED(MEMINFO_COPYRIGHT_YEAR, ["$meminfo_year"], [Last year of the copyright notice])

  dnl sigqueue() carries an integer with the signal, which is how
  dnl meminfo_dump_process() tells the target which file name to use and
  dnl recognises its own dump. Without it (macOS) the trigger falls back to
  dnl kill() and matches the dump by pid and time.
  AC_CHECK_FUNCS(sigqueue)

  AC_MSG_NOTICE([meminfo S3 dependency mode: $PHP_MEMINFO_S3])
  case "$PHP_MEMINFO_S3" in
    auto|yes)
      if test "$cross_compiling" = "yes"; then
        AC_MSG_ERROR([automatic S3 dependencies do not support cross-compilation; use an explicit --with-meminfo-s3=DIR or --with-meminfo-s3=no])
      fi
      dnl Probe the actual selected compiler (including flags), not just uname.
      meminfo_compiler_arch=unsupported
      AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#if !defined(__aarch64__) && !defined(__arm64__)
#error not arm64
#endif
      ]], [[]])], [meminfo_compiler_arch=arm64])
      AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#if !defined(__x86_64__) || defined(__ILP32__)
#error not x64
#endif
      ]], [[]])], [meminfo_compiler_arch=x64])
      meminfo_s3_source=`cd "PHP_EXT_SRCDIR(meminfo)" && pwd`
      meminfo_s3_build=`pwd`
      if meminfo_s3_dir=`CC="$CC" CXX="$CXX" sh "$meminfo_s3_source/deps/s3-deps.sh" \
          "$meminfo_s3_source" "$meminfo_s3_build" "$host" "$meminfo_compiler_arch"`; then
        AC_MSG_NOTICE([meminfo S3 dependencies: $meminfo_s3_dir])
      else
        AC_MSG_ERROR([S3 dependency preparation failed; see the meminfo.deps error/log path above. Retry with MEMINFO_BUILD_VERBOSE=1 pie -v install vanta/meminfo, or explicitly select --with-meminfo-s3=no])
      fi
      ;;
    no) ;;
    "") AC_MSG_ERROR([empty --with-meminfo-s3 value; use auto, no, or an existing prefix]) ;;
    *) meminfo_s3_dir="$PHP_MEMINFO_S3" ;;
  esac

  dnl The S3 transport links aws-c-s3 and the rest of the AWS CRT statically,
  dnl in dependency order. On Linux the CRT brings s2n and its own OpenSSL:
  dnl --exclude-libs keeps every symbol of those archives out of the dynamic
  dnl symbol table, so they cannot clash with the OpenSSL of ext/openssl or
  dnl ext/curl loaded into the same process.
  if test "$PHP_MEMINFO_S3" != "no"; then
    case "$meminfo_s3_dir" in
      *[[![:graph:]]]*|*\'*|*\"*|*\`*|*\$*|*\;*|*\&*|*\|*|*\(*|*\)*|*\<*|*\>*|*\\*)
        AC_MSG_ERROR([S3 dependency prefixes must not contain whitespace or shell metacharacters]) ;;
    esac
    if test ! -f "$meminfo_s3_dir/include/aws/s3/s3_client.h"; then
      AC_MSG_ERROR([--with-meminfo-s3 prefix must contain include/aws/s3/s3_client.h; use --with-meminfo-s3=auto to prepare dependencies automatically])
    fi

    AC_MSG_CHECKING([for the AWS CRT libraries in $meminfo_s3_dir/lib])
    meminfo_s3_libs=""
    meminfo_s3_found=""
    for meminfo_s3_lib in aws-c-s3 aws-c-auth aws-c-http aws-c-io aws-c-compression aws-c-cal \
                          aws-c-sdkutils aws-checksums aws-c-common s2n ssl crypto; do
      if test -f "$meminfo_s3_dir/lib/lib$meminfo_s3_lib.a"; then
        meminfo_s3_libs="$meminfo_s3_libs $meminfo_s3_dir/lib/lib$meminfo_s3_lib.a"
        meminfo_s3_found="$meminfo_s3_found $meminfo_s3_lib"
      fi
    done
    AC_MSG_RESULT([$meminfo_s3_found])
    case " $meminfo_s3_found " in
      *" aws-c-s3 "*" aws-c-common "*) ;;
      *) AC_MSG_ERROR([static libaws-c-s3.a and libaws-c-common.a not found in $meminfo_s3_dir/lib]) ;;
    esac

    case $host_os in
      darwin*)
        meminfo_s3_ldflags="-framework CoreFoundation -framework Security -framework Network"
        ;;
      *)
        meminfo_s3_ldflags="-Wl,--exclude-libs,ALL -lpthread -ldl -lm"
        ;;
    esac

    AC_MSG_CHECKING([whether aws-c-s3 links])
    meminfo_s3_save_LIBS="$LIBS"
    meminfo_s3_save_CPPFLAGS="$CPPFLAGS"
    LIBS="$meminfo_s3_libs $meminfo_s3_ldflags $LIBS"
    CPPFLAGS="-I$meminfo_s3_dir/include $CPPFLAGS"
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <aws/s3/s3_client.h>]],
      [[aws_s3_library_init(aws_default_allocator()); aws_s3_library_clean_up();]])],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no]); AC_MSG_ERROR([cannot link aws-c-s3 from $meminfo_s3_dir; see config.log])])
    LIBS="$meminfo_s3_save_LIBS"
    CPPFLAGS="$meminfo_s3_save_CPPFLAGS"

    PHP_ADD_INCLUDE([$meminfo_s3_dir/include])
    if test "$ext_shared" = "yes"; then
      MEMINFO_SHARED_LIBADD="$MEMINFO_SHARED_LIBADD $meminfo_s3_libs $meminfo_s3_ldflags"
      PHP_SUBST(MEMINFO_SHARED_LIBADD)
    else
      dnl Built into the PHP binary (./configure --enable-meminfo in php-src).
      LIBS="$LIBS $meminfo_s3_libs $meminfo_s3_ldflags"
    fi
    AC_DEFINE(MEMINFO_HAVE_S3, 1, [Whether dumps can be uploaded to S3])
  fi

  dnl Every source is listed unconditionally: a module whose platform or PHP
  dnl version guard is off compiles to an empty object. Keep in step with
  dnl CMakeLists.txt.
  PHP_NEW_EXTENSION(meminfo, meminfo.c writer.c ptr_set.c traverse.c items.c fibers.c signal.c procinfo.c trigger.c generations.c sites.c upload.c upload_s3.c, $ext_shared)

  dnl Errors are reported by throwing SPL exceptions, so SPL has to be
  dnl initialised before this extension is.
  PHP_ADD_EXTENSION_DEP(meminfo, spl)
fi
