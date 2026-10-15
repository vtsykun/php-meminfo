PHP_ARG_ENABLE(meminfo, whether to enable Memory Information support,
[ --enable-meminfo   Enable Meminfo support])

if test "$PHP_MEMINFO" = "yes"; then
  AC_DEFINE(HAVE_MEMINFO, 1, [Whether you have Memory Info])

  dnl sigqueue() carries an integer with the signal, which is how
  dnl meminfo_dump_process() tells the target which file name to use and
  dnl recognises its own dump. Without it (macOS) the trigger falls back to
  dnl kill() and matches the dump by pid and time.
  AC_CHECK_FUNCS(sigqueue)

  PHP_NEW_EXTENSION(meminfo, meminfo.c, $ext_shared)
fi
