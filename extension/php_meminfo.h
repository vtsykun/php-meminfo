#ifndef PHP_MEMINFO_H
#define PHP_MEMINFO_H 1

extern zend_module_entry meminfo_module_entry;
#define phpext_meminfo_ptr &meminfo_module_entry

/**
 * Whether this build can dump on a signal.
 *
 * The target side needs zend_interrupt_function and EG(vm_interrupt), which
 * arrived in 7.1, and a POSIX signal API. Under ZTS a signal is delivered to an
 * arbitrary thread while the globals and the interrupt flag are per-thread, so
 * the handler is not installed there either.
 *
 * meminfo_dump() and meminfo_dump_process() work regardless — a process can
 * always be asked for a dump from a build that cannot produce one itself.
 */
#if PHP_VERSION_ID >= 70100 && !defined(ZTS) && !defined(PHP_WIN32)
# define MEMINFO_HAVE_SIGNAL_DUMP 1
#else
# define MEMINFO_HAVE_SIGNAL_DUMP 0
#endif

#ifndef PHP_WIN32
# include <signal.h>
#endif

#define MEMINFO_NAME "PHP Meminfo"
#define MEMINFO_VERSION "2.0.0-beta1"
#define MEMINFO_AUTHOR "Benoit Jacquemont"
#define MEMINFO_COPYRIGHT  "Copyright (c) 2010-2021 by Benoit Jacquemont & contributors"
#define MEMINFO_COPYRIGHT_SHORT "Copyright (c) 2010-2021"

// Size of the writer's staging buffer. Big enough that a dump of a large object
// graph costs hundreds of writes rather than millions.
#define MEMINFO_WRITER_BUFFER_SIZE 65536

/**
 * Output sink of a dump.
 *
 * Output is staged in buf and handed over in large chunks, either through the
 * stream layer (so that a user stream wrapper passed to meminfo_dump() keeps
 * working) or straight to a file descriptor — a dump triggered from an
 * interrupt has no php_stream and must not allocate one.
 *
 * Once a write fails, err is set and every later append is dropped: the
 * traversal notices and stops instead of spending minutes producing output
 * that cannot be stored.
 *
 * Allocate it with malloc, never emalloc: the buffer must not be charged to
 * the memory_limit of the process being inspected, which may already be at its
 * limit — that is often why it is being dumped.
 */
typedef struct {
    php_stream *stream;     /* flush through the stream layer, or NULL */
    int fd;                 /* flush with write(2), or -1 */
    size_t len;             /* bytes staged in buf */
    int err;                /* a write failed; output is being discarded */
    char buf[MEMINFO_WRITER_BUFFER_SIZE];
} meminfo_writer;

/**
 * Per-process state of the extension.
 *
 * dump_requested and dump_id are written by the signal handler and read at the
 * next safe point in the VM, hence volatile sig_atomic_t: nothing else in a
 * handler is guaranteed to be observed correctly.
 */
ZEND_BEGIN_MODULE_GLOBALS(meminfo)
    /* ini: signal name or number to dump on, "0" or empty to disable */
    char *dump_signal_setting;
    /* ini: directory for dump files, empty means the system temp directory */
    char *dump_dir;

    /* dump_signal_setting resolved to a signal number, 0 when disabled */
    int dump_signal;

    /* set by the signal handler, consumed by the interrupt hook */
    volatile sig_atomic_t dump_requested;
    volatile sig_atomic_t dump_id;

    /* the handler is installed for the duration of a request only */
    zend_bool handler_installed;
    /* complained once already about a signal that cannot be installed */
    zend_bool install_failed;
    /* guards against a dump started while one is already running */
    zend_bool dumping;

#if MEMINFO_HAVE_SIGNAL_DUMP
    struct sigaction previous_action;
#endif
ZEND_END_MODULE_GLOBALS(meminfo)

ZEND_EXTERN_MODULE_GLOBALS(meminfo)

#define MEMINFO_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(meminfo, v)

PHP_FUNCTION(meminfo_dump);

#ifndef PHP_WIN32
/**
 * meminfo_dump_process(int $pid, array $options = []): string
 *
 * Signals another process to dump its memory and waits for the file. Works
 * from any build, including ones that cannot serve a dump themselves.
 *
 * Returns the path of the finished dump. Every failure throws instead of
 * returning a value: InvalidArgumentException when the call itself is wrong,
 * RuntimeException for anything about the target or the environment.
 */
PHP_FUNCTION(meminfo_dump_process);
#endif

zend_ulong   meminfo_get_element_size(zval* z);

/**
 * Dump every item reachable from start_frame plus all class static members.
 *
 * start_frame is the innermost execution frame to walk; passing the frame of
 * the caller rather than reading EG(current_execute_data) is what makes the
 * traversal usable from an interrupt handler.
 *
 * include_global_symbol_table additionally walks EG(symbol_table) as
 * <GLOBAL>. The global scope is normally reached through the main script's
 * frame, but a dump taken during shutdown — from a shutdown function or a
 * destructor — starts from a chain that no longer contains it.
 */
void meminfo_dump_to_writer(meminfo_writer *w, zend_execute_data *start_frame, int include_global_symbol_table);

// The traversal itself (frames, static members, items and their children) is
// internal to meminfo.c: it is driven by context and bookkeeping types that
// callers of the extension have no business knowing about.

void meminfo_build_frame_label(char * frame_label, int frame_label_len, zend_execute_data* frame);

extern zend_module_entry meminfo_entry;

#endif
