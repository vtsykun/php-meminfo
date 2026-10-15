#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#ifndef PHP_WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "ext/standard/info.h"
#include "php_ini.h"
#include "php_open_temporary_file.h"

#include "zend_extensions.h"
#include "Zend/zend_compile.h"

#if PHP_VERSION_ID >= 80100
#include "zend_fibers.h"
#endif

#include "zend.h"
#include "SAPI.h"
#include "zend_API.h"
#include "zend_object_handlers.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#if PHP_VERSION_ID >= 80000
ZEND_BEGIN_ARG_INFO_EX(arginfo_meminfo_dump, 0, 0, 1)
    ZEND_ARG_INFO(0, output_stream)
ZEND_END_ARG_INFO()

// Always a string: every failure path throws instead of returning a value.
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_meminfo_dump_process, 0, 1, IS_STRING, 0)
    ZEND_ARG_INFO(0, pid)
    ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

const zend_function_entry meminfo_functions[] = {
    PHP_FE(meminfo_dump, arginfo_meminfo_dump)
#ifndef PHP_WIN32
    PHP_FE(meminfo_dump_process, arginfo_meminfo_dump_process)
#endif
    PHP_FE_END
};
#else
const zend_function_entry meminfo_functions[] = {
    PHP_FE(meminfo_dump, NULL)
#ifndef PHP_WIN32
    PHP_FE(meminfo_dump_process, NULL)
#endif
    PHP_FE_END
};
#endif

/**
 * Set of already-visited item addresses.
 *
 * Open addressing with linear probing over a power-of-two table of raw
 * pointers. A zend_hash keyed by the printed "%p" identifier was doing the same
 * job, but it allocated a zend_string per lookup and charged the whole table to
 * the process being inspected; a persistent zend_hash would instead pemalloc
 * and abort the process when memory runs out. Slot value 0 means empty, which
 * is unambiguous because no item ever lives at address 0.
 */
typedef struct {
    uintptr_t *slots;
    size_t capacity;    /* power of two, 0 before the first insert */
    size_t count;
    int failed;         /* allocation failed: the dump has to stop */
} meminfo_ptr_set;

/**
 * A declared property read straight out of an object.
 */
typedef struct {
    zend_string *name;      /* borrowed from the class entry; still mangled */
    zval *value;            /* borrowed: the property slot inside the object */
} meminfo_prop_slot;

/**
 * One level of the traversal: an item whose children still have to be visited.
 *
 * The children of an item are written as a flat "children" block when the item
 * itself is written; the zvals behind them are visited afterwards, which is
 * what this level records. Children come either from a HashTable or, for a
 * plain object whose property table does not exist yet, from a list of
 * declared property slots.
 */
typedef struct {
    HashTable *ht;                      /* children left to visit, NULL when not iterating a table */
    HashPosition pos;                   /* cursor into ht */

    meminfo_prop_slot *slots;           /* declared properties, NULL when not iterating slots */
    uint32_t slot_count;
    uint32_t slot_pos;

#if PHP_VERSION_ID >= 70400
    HashTable *properties_to_release;   /* get_properties() result, released once the subtree is done */
#endif
#if PHP_VERSION_ID >= 80100
    zend_execute_data *pending_fiber;   /* fiber frames to walk once the subtree is done */
#endif
} meminfo_level;

/**
 * Explicit work stack replacing C recursion over the object graph.
 *
 * It is deliberately allocated with malloc rather than emalloc: the dump runs
 * inside a process that is being inspected for memory problems and must not
 * push it over memory_limit, and a dump triggered from an interrupt may run
 * while the Zend allocator is already near its limit.
 */
typedef struct {
    meminfo_level *levels;
    size_t len;
    size_t capacity;
    int exhausted;      /* an allocation failed: stop descending, still close the JSON */
} meminfo_stack;

/**
 * State threaded through the whole traversal.
 */
typedef struct {
    meminfo_writer *w;
    meminfo_ptr_set visited_items;
    int first_element;
    meminfo_stack stack;
    int aborted;        /* an allocation outside the stack and the set failed */
} meminfo_context;

/**
 * Whether the traversal has given up (an allocation failed).
 *
 * The dump is still closed as valid JSON when this is set — a truncated but
 * parseable dump is far more useful than a corrupt one.
 */
static int meminfo_aborted(const meminfo_context *ctx)
{
    // A failed write counts too: once the output cannot be stored there is no
    // point walking the rest of the heap.
    return ctx->aborted || ctx->stack.exhausted || ctx->visited_items.failed || ctx->w->err;
}

/* ------------------------------------------------------------------------- *
 * Writer
 *
 * Everything the dump emits goes through these. There is deliberately no
 * printf-style entry point: the old writer spent roughly 60% of its time in a
 * 34-pass escaper and issued about nine stream writes per item.
 * ------------------------------------------------------------------------- */

/* Append a string literal, without a strlen call. */
#define MEMINFO_WRITE_LIT(w, literal) \
    meminfo_write_raw((w), (literal), sizeof(literal) - 1)

/**
 * Hand a buffer over to the sink, retrying partial writes.
 */
static void meminfo_write_through(meminfo_writer *w, const char *ptr, size_t len)
{
    if (w->stream) {
        if ((size_t) php_stream_write(w->stream, ptr, len) != len) {
            w->err = 1;
        }
        return;
    }

#ifndef PHP_WIN32
    if (w->fd >= 0) {
        size_t offset = 0;

        while (offset < len) {
            ssize_t written = write(w->fd, ptr + offset, len - offset);

            if (written < 0) {
                if (errno == EINTR) {
                    // A signal arrived mid-write; the dump itself may well be
                    // what triggered another one.
                    continue;
                }
                w->err = 1;
                return;
            }

            offset += (size_t) written;
        }
        return;
    }
#endif

    w->err = 1;
}

static void meminfo_writer_flush(meminfo_writer *w)
{
    if (w->err || w->len == 0) {
        w->len = 0;
        return;
    }

    meminfo_write_through(w, w->buf, w->len);
    w->len = 0;
}

static void meminfo_write_raw(meminfo_writer *w, const char *ptr, size_t len)
{
    if (w->err || len == 0) {
        return;
    }

    if (len >= sizeof(w->buf)) {
        // Larger than the buffer: flush what is staged, then pass it straight
        // through rather than copying it in pieces.
        meminfo_writer_flush(w);
        if (!w->err) {
            meminfo_write_through(w, ptr, len);
        }
        return;
    }

    if (w->len + len > sizeof(w->buf)) {
        meminfo_writer_flush(w);
        if (w->err) {
            return;
        }
    }

    memcpy(w->buf + w->len, ptr, len);
    w->len += len;
}

/**
 * Append a NUL-terminated string with JSON string escaping.
 *
 * Only what the format requires is escaped: '"', '\' and the C0 controls as
 * \u00xx. Bytes above 0x1f are passed through untouched, so a non-UTF-8 byte
 * stays exactly as it was found in memory.
 *
 * Runs of unescaped bytes are copied in one go, which is what makes this a
 * single pass over the input with no allocation at all.
 */
static void meminfo_write_json_escaped(meminfo_writer *w, const char *s)
{
    static const char hex_digits[] = "0123456789abcdef";

    const char *run = s;
    const char *p = s;

    for (; *p != '\0'; p++) {
        unsigned char c = (unsigned char) *p;
        const char *escape;
        size_t escape_len;
        char unicode[6];

        if (c == '"') {
            escape = "\\\"";
            escape_len = 2;
        } else if (c == '\\') {
            escape = "\\\\";
            escape_len = 2;
        } else if (c <= 0x1f) {
            unicode[0] = '\\';
            unicode[1] = 'u';
            unicode[2] = '0';
            unicode[3] = '0';
            unicode[4] = hex_digits[(c >> 4) & 0xf];
            unicode[5] = hex_digits[c & 0xf];

            escape = unicode;
            escape_len = 6;
        } else {
            continue;
        }

        if (p > run) {
            meminfo_write_raw(w, run, (size_t) (p - run));
        }
        meminfo_write_raw(w, escape, escape_len);

        run = p + 1;
    }

    if (p > run) {
        meminfo_write_raw(w, run, (size_t) (p - run));
    }
}

/**
 * Append a signed decimal integer.
 *
 * Every number the dump emits used to go through a "%ld"/"%zd"/"%d" format,
 * all of which are signed, so values are formatted as signed here too — a
 * negative array key must keep printing as a negative number.
 */
static void meminfo_write_int(meminfo_writer *w, int64_t value)
{
    char digits[24];
    size_t len = 0;
    uint64_t magnitude;
    int negative = value < 0;

    // Negated as unsigned: -INT64_MIN does not fit in an int64_t.
    magnitude = negative ? (~(uint64_t) value) + 1 : (uint64_t) value;

    do {
        digits[len++] = (char) ('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    if (negative) {
        digits[len++] = '-';
    }

    while (len > 0) {
        char c = digits[--len];
        meminfo_write_raw(w, &c, 1);
    }
}

/**
 * Append an address exactly as "%p" renders it: "0x" and lowercase hex with no
 * leading zeros.
 *
 * Written out by hand both for speed and because php.h redirects snprintf to
 * ap_php_snprintf, whose "%p" is not unique on PHP 7.0 — item identifiers must
 * be, since the whole dump is keyed by them.
 */
static void meminfo_write_ptr(meminfo_writer *w, const void *ptr)
{
    static const char hex_digits[] = "0123456789abcdef";

    char out[2 + sizeof(uintptr_t) * 2];
    uintptr_t value = (uintptr_t) ptr;
    size_t digits = 0;
    size_t i;
    uintptr_t probe = value;

    do {
        digits++;
        probe >>= 4;
    } while (probe != 0);

    out[0] = '0';
    out[1] = 'x';

    for (i = 0; i < digits; i++) {
        out[2 + digits - 1 - i] = hex_digits[(value >> (i * 4)) & 0xf];
    }

    meminfo_write_raw(w, out, 2 + digits);
}

static int meminfo_visit_item(meminfo_ptr_set *set, const void *ptr);
/* ------------------------------------------------------------------------- *
 * Module globals, configuration and lifecycle
 * ------------------------------------------------------------------------- */

ZEND_DECLARE_MODULE_GLOBALS(meminfo)

#ifndef PHP_WIN32
/**
 * Signal names accepted by meminfo.dump_signal, without the "SIG" prefix.
 *
 * Deliberately not a whitelist of *sensible* signals: an operator may have a
 * reason to pick an unusual one, and a signal that cannot be caught is
 * reported when sigaction() refuses it rather than being rejected here.
 */
typedef struct {
    const char *name;
    int number;
} meminfo_signal_name;

static const meminfo_signal_name meminfo_signal_names[] = {
#ifdef SIGURG
    { "URG", SIGURG },
#endif
#ifdef SIGUSR1
    { "USR1", SIGUSR1 },
#endif
#ifdef SIGUSR2
    { "USR2", SIGUSR2 },
#endif
#ifdef SIGHUP
    { "HUP", SIGHUP },
#endif
#ifdef SIGINT
    { "INT", SIGINT },
#endif
#ifdef SIGQUIT
    { "QUIT", SIGQUIT },
#endif
#ifdef SIGTERM
    { "TERM", SIGTERM },
#endif
#ifdef SIGWINCH
    { "WINCH", SIGWINCH },
#endif
#ifdef SIGIO
    { "IO", SIGIO },
#endif
#ifdef SIGPROF
    { "PROF", SIGPROF },
#endif
#ifdef SIGVTALRM
    { "VTALRM", SIGVTALRM },
#endif
#ifdef SIGALRM
    { "ALRM", SIGALRM },
#endif
#ifdef SIGCONT
    { "CONT", SIGCONT },
#endif
#ifdef SIGTRAP
    { "TRAP", SIGTRAP },
#endif
#ifdef SIGPWR
    { "PWR", SIGPWR },
#endif
#ifdef SIGKILL
    { "KILL", SIGKILL },
#endif
#ifdef SIGSTOP
    { "STOP", SIGSTOP },
#endif
    { NULL, 0 }
};

/**
 * Resolve a meminfo.dump_signal value to a signal number.
 *
 * Accepts a decimal number, or a name with or without the "SIG" prefix, in any
 * case. An empty value or "0" means "do not install a handler".
 *
 * Returns 0 when the value cannot be resolved, in which case *out is untouched.
 */
static int meminfo_parse_signal(const char *value, int *out)
{
    const meminfo_signal_name *entry;
    const char *name;
    size_t i;

    if (value == NULL) {
        *out = 0;
        return 1;
    }

    while (*value == ' ' || *value == '\t') {
        value++;
    }

    if (*value == '\0') {
        *out = 0;
        return 1;
    }

    if (*value >= '0' && *value <= '9') {
        int number = 0;

        for (i = 0; value[i] != '\0'; i++) {
            if (value[i] < '0' || value[i] > '9') {
                return 0;
            }
            number = number * 10 + (value[i] - '0');
            if (number > 255) {
                return 0;
            }
        }

        *out = number;
        return 1;
    }

    name = value;
    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    for (entry = meminfo_signal_names; entry->name != NULL; entry++) {
        if (strcasecmp(name, entry->name) == 0) {
            *out = entry->number;
            return 1;
        }
    }

    return 0;
}
#endif /* !PHP_WIN32 */

/**
 * Directory dump files are written to: the configured one, or the system
 * temporary directory when it is left empty.
 */
static const char *meminfo_dump_directory(void)
{
    const char *dir = MEMINFO_G(dump_dir);

    if (dir != NULL && dir[0] != '\0') {
        return dir;
    }

    return php_get_temporary_directory();
}

// Default action of SIGURG is to ignore it, which makes it the only common
// choice that cannot kill a process that is not listening. php-fpm resets
// SIGUSR2 to SIG_DFL in every worker, where its default action is to
// terminate, so an idle worker signalled with it would simply die.
#define MEMINFO_DEFAULT_DUMP_SIGNAL "SIGURG"

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("meminfo.dump_signal", MEMINFO_DEFAULT_DUMP_SIGNAL, PHP_INI_SYSTEM, OnUpdateString,
        dump_signal_setting, zend_meminfo_globals, meminfo_globals)
    STD_PHP_INI_ENTRY("meminfo.dump_dir", "", PHP_INI_SYSTEM, OnUpdateString,
        dump_dir, zend_meminfo_globals, meminfo_globals)
PHP_INI_END()

#if MEMINFO_HAVE_SIGNAL_DUMP

/* The interrupt hook that was in place before this extension chained itself in. */
static void (*meminfo_previous_interrupt_function)(zend_execute_data *execute_data) = NULL;

/**
 * Signal handler.
 *
 * Does the bare minimum that is async-signal-safe: records the request and
 * raises the VM's interrupt flag. The engine clears that flag and calls
 * zend_interrupt_function at the next safe point between opcodes, which is
 * where the dump actually happens — walking the heap from inside a signal
 * handler would be unsafe in every possible way.
 */
static void meminfo_signal_handler(int signal_number, siginfo_t *info, void *context)
{
    (void) signal_number;
    (void) context;

    // sigqueue() carries the id the trigger is waiting for; a plain kill()
    // carries nothing, and the file is then named after the pid and the time.
    MEMINFO_G(dump_id) = (info != NULL && info->si_code == SI_QUEUE)
        ? (sig_atomic_t) info->si_value.sival_int
        : 0;

    MEMINFO_G(dump_requested) = 1;

    EG(vm_interrupt) = 1;
}

/**
 * Build the name of the dump file for the pending request.
 *
 * A dump asked for with an id is named after it, so the trigger knows the exact
 * path to wait for and a resend of the same id is recognised as already served.
 */
static int meminfo_dump_file_name(char *buffer, size_t size, int dump_id)
{
    const char *dir = meminfo_dump_directory();
    int written;

    if (dir == NULL) {
        dir = "/tmp";
    }

    if (dump_id != 0) {
        written = snprintf(buffer, size, "%s/meminfo-%d.json", dir, dump_id);
    } else {
        struct timeval now;

        gettimeofday(&now, NULL);
        written = snprintf(buffer, size, "%s/meminfo-%d-%ld-%ld.json",
            dir, (int) getpid(), (long) now.tv_sec, (long) now.tv_usec);
    }

    return written > 0 && (size_t) written < size;
}

/**
 * Write the dump to a file, at a safe point in the VM.
 *
 * Nothing here may reach the output of the process being dumped: a stream
 * notice or a user error handler firing in the middle of someone's response is
 * exactly the kind of interference this feature must not cause. Failures go to
 * the error log only.
 */
static void meminfo_dump_to_file(zend_execute_data *execute_data, int dump_id)
{
    char final_path[MAXPATHLEN];
    char temp_path[MAXPATHLEN + 8];
    meminfo_writer *writer;
    int fd;
    int failed;
    int saved_errno;
#ifdef HAVE_SETITIMER
    struct itimerval no_timer;
    struct itimerval saved_timer;
    int timer_paused = 0;
#endif

    if (!meminfo_dump_file_name(final_path, sizeof(final_path), dump_id)) {
        php_log_err("meminfo: dump directory path is too long");
        return;
    }

    // An already finished dump for this id means the trigger resent the signal
    // while waiting; serving it twice would only waste the target's time.
    if (access(final_path, F_OK) == 0) {
        return;
    }

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", final_path) < 0) {
        return;
    }

#ifdef HAVE_SETITIMER
    // A dump of a large heap can take seconds of CPU. max_execution_time is
    // measured on ITIMER_PROF, and exceeding it hard-kills the process with
    // _exit(124) — the dump would take the process down with it.
    memset(&no_timer, 0, sizeof(no_timer));
    if (setitimer(ITIMER_PROF, &no_timer, &saved_timer) == 0) {
        timer_paused = 1;
    }
#endif

    // O_EXCL so two triggers cannot write the same file; O_NOFOLLOW so a
    // planted symlink cannot redirect the dump; 0600 because a dump exposes
    // array keys, class and property names and heap addresses.
    fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (fd < 0) {
        char message[MAXPATHLEN + 192];

        saved_errno = errno;

        // EEXIST is reported too. A repeated signal for a dump that already
        // finished was handled by the check above, and a second dump cannot be
        // started while one is running, so something is left over at this path
        // — a partial dump from a process that died, or a planted entry.
        // O_NOFOLLOW means a symlink lands here as well, rather than being
        // followed somewhere it should not go.
        snprintf(message, sizeof(message),
            "meminfo: cannot create %s: %s%s",
            temp_path, strerror(saved_errno),
            saved_errno == EEXIST ? " (left over from an earlier dump, or not created by meminfo)" : "");
        php_log_err(message);
    } else {
        writer = malloc(sizeof(*writer));

        if (writer == NULL) {
            close(fd);
            unlink(temp_path);
            php_log_err("meminfo: cannot allocate the dump buffer");
        } else {
            writer->stream = NULL;
            writer->fd = fd;
            writer->len = 0;
            writer->err = 0;

            // During shutdown the chain no longer reaches the main script's
            // frame, so the global scope is walked explicitly.
            meminfo_dump_to_writer(writer, execute_data, 1);

            failed = writer->err;
            free(writer);

            if (close(fd) != 0) {
                failed = 1;
            }

            if (failed) {
                char message[MAXPATHLEN + 128];

                snprintf(message, sizeof(message), "meminfo: dump to %s failed: %s",
                    temp_path, strerror(errno));
                php_log_err(message);
                unlink(temp_path);
            } else if (rename(temp_path, final_path) != 0) {
                char message[MAXPATHLEN + 128];

                snprintf(message, sizeof(message), "meminfo: cannot rename %s: %s",
                    temp_path, strerror(errno));
                php_log_err(message);
                unlink(temp_path);
            }
        }
    }

#ifdef HAVE_SETITIMER
    if (timer_paused) {
        setitimer(ITIMER_PROF, &saved_timer, NULL);
    }
#endif

    // Hand the pages the dump touched back, so the process does not keep its
    // resident size at the dump's peak for the rest of its life.
    zend_mm_gc(zend_mm_get_heap());
}

/**
 * Interrupt hook: runs between opcodes, with the engine in a consistent state.
 */
static void meminfo_interrupt_handler(zend_execute_data *execute_data)
{
    if (MEMINFO_G(dump_requested) && !MEMINFO_G(dumping)) {
        int dump_id;

        // Cleared before anything else: a signal that arrives from here on
        // leaves the flag set and produces one more dump afterwards, rather
        // than being swallowed. Reading the id afterwards means a signal in
        // that window is served by this dump instead of being lost.
        MEMINFO_G(dump_requested) = 0;
        dump_id = (int) MEMINFO_G(dump_id);

        MEMINFO_G(dumping) = 1;

        meminfo_dump_to_file(execute_data, dump_id);

        MEMINFO_G(dumping) = 0;
    }

    // Timeouts and pcntl's signal dispatch run through the same hook.
    if (meminfo_previous_interrupt_function != NULL) {
        meminfo_previous_interrupt_function(execute_data);
    }
}

/**
 * Install the handler for the duration of a request.
 *
 * Plain sigaction(), not zend_sigaction(): the latter keeps its own deferring
 * handler registered in the kernel, so /proc/<pid>/status would still report
 * the signal as caught after it had been "restored". The trigger relies on
 * that bit to tell whether the target is in a request at all.
 */
static void meminfo_install_signal_handler(void)
{
    struct sigaction action;

    if (MEMINFO_G(dump_signal) == 0
        || MEMINFO_G(handler_installed)
        || MEMINFO_G(install_failed)) {
        return;
    }

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = meminfo_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);

    if (sigaction(MEMINFO_G(dump_signal), &action, &MEMINFO_G(previous_action)) == 0) {
        MEMINFO_G(handler_installed) = 1;
    } else {
        char message[160];

        // Once only: this would otherwise be logged on every request.
        MEMINFO_G(install_failed) = 1;

        snprintf(message, sizeof(message),
            "meminfo: cannot handle signal %d: %s; signal dumps are disabled",
            MEMINFO_G(dump_signal), strerror(errno));
        php_log_err(message);
    }
}

static void meminfo_restore_signal_handler(void)
{
    if (!MEMINFO_G(handler_installed)) {
        return;
    }

    sigaction(MEMINFO_G(dump_signal), &MEMINFO_G(previous_action), NULL);
    MEMINFO_G(handler_installed) = 0;
}

#endif /* MEMINFO_HAVE_SIGNAL_DUMP */

/* ------------------------------------------------------------------------- *
 * Trigger side
 *
 * Asking another process for a dump. Works on every version and on ZTS: the
 * build doing the asking does not need to be able to serve a dump itself.
 * ------------------------------------------------------------------------- */

#ifndef PHP_WIN32

/* How often the trigger looks for the dump file. */
#define MEMINFO_POLL_INTERVAL_USEC 50000
/* How often the signal is resent while the target has not started writing. */
#define MEMINFO_RESEND_INTERVAL_USEC 1000000

static uint64_t meminfo_now_usec(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) (ts.tv_nsec / 1000);
    }
#endif
    {
        struct timeval tv;

        gettimeofday(&tv, NULL);

        return (uint64_t) tv.tv_sec * 1000000ULL + (uint64_t) tv.tv_usec;
    }
}

static void meminfo_sleep_usec(uint64_t usec)
{
    struct timespec requested;

    requested.tv_sec = (time_t) (usec / 1000000ULL);
    requested.tv_nsec = (long) ((usec % 1000000ULL) * 1000);

    while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
        /* interrupted — finish the remaining time */
    }
}

/**
 * Read a small pseudo-file whose size is not known in advance.
 */
static ssize_t meminfo_read_small_file(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t total = 0;

    if (fd < 0) {
        return -1;
    }

    while (total < size - 1) {
        ssize_t got = read(fd, buffer + total, size - 1 - total);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (got == 0) {
            break;
        }

        total += (size_t) got;
    }

    close(fd);
    buffer[total] = '\0';

    return (ssize_t) total;
}

/**
 * Value of a "Name:\tvalue" line of /proc/<pid>/status, or NULL.
 */
static const char *meminfo_status_field(const char *status, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = status;

    while (line != NULL && *line != '\0') {
        if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *value = line + name_len + 1;

            while (*value == ' ' || *value == '\t') {
                value++;
            }

            return value;
        }

        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }

    return NULL;
}

/**
 * Whether delivering this signal to a process that does not handle it is
 * harmless.
 *
 * These are the signals whose default action neither terminates nor stops the
 * receiver. Everything else — including SIGUSR1, SIGUSR2 and SIGIO, which look
 * like natural choices — would kill a target that is not listening.
 */
static int meminfo_signal_is_harmless(int signal_number)
{
    switch (signal_number) {
#ifdef SIGURG
        case SIGURG:        /* ignored by default */
#endif
#ifdef SIGCHLD
        case SIGCHLD:       /* ignored by default */
#endif
#ifdef SIGWINCH
        case SIGWINCH:      /* ignored by default */
#endif
#ifdef SIGCONT
        case SIGCONT:       /* resumes a stopped process; does not end it */
#endif
            return 1;
        default:
            return 0;
    }
}

/**
 * Whether the target has a handler installed for this signal.
 *
 * This is the one cheap way to tell, from outside, that the target has meminfo
 * loaded *and* is currently inside a request — the handler only exists for the
 * duration of one. Returns 1 caught, 0 not caught, -1 when it cannot be known
 * (no /proc, as on macOS).
 */
static int meminfo_signal_is_caught(pid_t pid, int signal_number)
{
    char path[64];
    char status[8192];
    const char *field;
    unsigned long long mask;

    if (signal_number < 1 || signal_number > 64) {
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);

    if (meminfo_read_small_file(path, status, sizeof(status)) < 0) {
        return -1;
    }

    field = meminfo_status_field(status, "SigCgt");
    if (field == NULL) {
        return -1;
    }

    mask = strtoull(field, NULL, 16);

    return ((mask >> (signal_number - 1)) & 1ULL) ? 1 : 0;
}

static int meminfo_process_uids(pid_t pid, uid_t *real_uid, uid_t *effective_uid)
{
    char path[64];
    char status[8192];
    const char *field;
    char *end;

    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);

    if (meminfo_read_small_file(path, status, sizeof(status)) < 0) {
        return 0;
    }

    field = meminfo_status_field(status, "Uid");
    if (field == NULL) {
        return 0;
    }

    *real_uid = (uid_t) strtoul(field, &end, 10);
    *effective_uid = (uid_t) strtoul(end, NULL, 10);

    return 1;
}

/**
 * Start time of a process, in clock ticks since boot.
 *
 * Used to notice that the pid has been reused by a different process while the
 * trigger was waiting, which would otherwise look like a dump that never
 * arrives.
 */
static int meminfo_process_start_time(pid_t pid, unsigned long long *out)
{
    char path[64];
    char buffer[4096];
    char *cursor;
    int field;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);

    if (meminfo_read_small_file(path, buffer, sizeof(buffer)) < 0) {
        return 0;
    }

    // The second field is the executable name in parentheses and may itself
    // contain spaces and parentheses, so fields are counted from the last ')'.
    cursor = strrchr(buffer, ')');
    if (cursor == NULL) {
        return 0;
    }
    cursor++;

    // Field 3 (state) starts here; starttime is field 22.
    for (field = 3; field < 22; field++) {
        while (*cursor == ' ') {
            cursor++;
        }
        while (*cursor != '\0' && *cursor != ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            return 0;
        }
    }

    while (*cursor == ' ') {
        cursor++;
    }

    *out = strtoull(cursor, NULL, 10);

    return 1;
}

/**
 * A non-zero 31-bit id naming this dump request.
 *
 * It only has to be unlikely to clash with another trigger running at the same
 * time; the target creates the file with O_EXCL, so a clash is noticed rather
 * than silently shared.
 */
static int meminfo_random_dump_id(void)
{
    int id = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        unsigned char bytes[4];

        if (read(fd, bytes, sizeof(bytes)) == (ssize_t) sizeof(bytes)) {
            id = (int) ((unsigned) bytes[0]
                | ((unsigned) bytes[1] << 8)
                | ((unsigned) bytes[2] << 16)
                | (((unsigned) bytes[3] & 0x7f) << 24));
        }

        close(fd);
    }

    if (id == 0) {
        struct timeval tv;

        gettimeofday(&tv, NULL);
        id = (int) ((((unsigned) tv.tv_usec << 8)
            ^ (unsigned) tv.tv_sec
            ^ ((unsigned) getpid() << 3)) & 0x7fffffff);
    }

    return id == 0 ? 1 : id;
}

/**
 * Check that a path really is the dump the target wrote.
 *
 * O_NOFOLLOW and the ownership check are what stop another user from parking a
 * symlink or a file of their own at the expected name and having it returned
 * as if it came from the target.
 */
static int meminfo_dump_file_is_trustworthy(const char *path, pid_t pid)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    uid_t real_uid = 0;
    uid_t effective_uid = 0;

    if (fd < 0) {
        return 0;
    }

    if (fstat(fd, &info) != 0) {
        close(fd);
        return 0;
    }

    close(fd);

    if (!S_ISREG(info.st_mode)) {
        return 0;
    }

    if (meminfo_process_uids(pid, &real_uid, &effective_uid)) {
        return info.st_uid == real_uid || info.st_uid == effective_uid;
    }

    // Without /proc, accept only our own files, or anything when running as
    // root (which can read them all anyway).
    return info.st_uid == geteuid() || geteuid() == 0;
}

#endif /* !PHP_WIN32 */

static PHP_GINIT_FUNCTION(meminfo)
{
#if defined(COMPILE_DL_MEMINFO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    memset(meminfo_globals, 0, sizeof(*meminfo_globals));
}

static PHP_MINIT_FUNCTION(meminfo)
{
    REGISTER_INI_ENTRIES();

#ifndef PHP_WIN32
    {
        int signal_number = 0;

        if (!meminfo_parse_signal(MEMINFO_G(dump_signal_setting), &signal_number)) {
            // Startup is the only place this can be reported: it happens before
            // any request, so it cannot corrupt a response body.
            php_error_docref(NULL, E_WARNING,
                "meminfo.dump_signal: unknown signal \"%s\"; signal dumps are disabled",
                MEMINFO_G(dump_signal_setting) ? MEMINFO_G(dump_signal_setting) : "");
            signal_number = 0;
        }

        MEMINFO_G(dump_signal) = signal_number;

# if !MEMINFO_HAVE_SIGNAL_DUMP
        // Only complain when an operator asked for a signal explicitly. The
        // setting has a default, so warning whenever it is non-zero would mean
        // warning on every single startup of an unsupported build.
        if (signal_number != 0
            && MEMINFO_G(dump_signal_setting) != NULL
            && strcmp(MEMINFO_G(dump_signal_setting), MEMINFO_DEFAULT_DUMP_SIGNAL) != 0) {

            php_error_docref(NULL, E_WARNING,
                "meminfo.dump_signal is set but this build cannot dump on a signal "
                "(needs PHP 7.1 or later, non-ZTS, POSIX); meminfo_dump_process() "
                "can still be used from here to dump other processes");
        }

        MEMINFO_G(dump_signal) = 0;
# endif
    }
#else
    MEMINFO_G(dump_signal) = 0;
#endif

#if MEMINFO_HAVE_SIGNAL_DUMP
    if (MEMINFO_G(dump_signal) != 0) {
        // Chained rather than replaced: the engine's own users of this hook
        // (execution timeouts, pcntl's signal dispatch) must keep working.
        meminfo_previous_interrupt_function = zend_interrupt_function;
        zend_interrupt_function = meminfo_interrupt_handler;
    }
#endif

    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(meminfo)
{
#if MEMINFO_HAVE_SIGNAL_DUMP
    if (zend_interrupt_function == meminfo_interrupt_handler) {
        zend_interrupt_function = meminfo_previous_interrupt_function;
    }
#endif

    UNREGISTER_INI_ENTRIES();

    return SUCCESS;
}

static PHP_RINIT_FUNCTION(meminfo)
{
#if defined(COMPILE_DL_MEMINFO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif

#if MEMINFO_HAVE_SIGNAL_DUMP
    // Installed per request on purpose. Outside a request there is no VM to
    // interrupt, and the trigger uses the "signal caught" bit in
    // /proc/<pid>/status to decide whether asking is worth anything.
    MEMINFO_G(dump_requested) = 0;
    MEMINFO_G(dump_id) = 0;
    MEMINFO_G(dumping) = 0;

    meminfo_install_signal_handler();
#endif

    return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(meminfo)
{
#if MEMINFO_HAVE_SIGNAL_DUMP
    meminfo_restore_signal_handler();
#endif

    return SUCCESS;
}

static PHP_MINFO_FUNCTION(meminfo)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "Version", MEMINFO_VERSION);
    php_info_print_table_row(2, "Signal dumps supported",
        MEMINFO_HAVE_SIGNAL_DUMP ? "yes" : "no (needs PHP 7.1+, non-ZTS, POSIX)");

    if (MEMINFO_G(dump_signal) != 0) {
        char signal_text[32];

        snprintf(signal_text, sizeof(signal_text), "%d (%s)",
            MEMINFO_G(dump_signal),
            MEMINFO_G(dump_signal_setting) ? MEMINFO_G(dump_signal_setting) : "");
        php_info_print_table_row(2, "Dump signal", signal_text);
    } else {
        php_info_print_table_row(2, "Dump signal", "disabled");
    }

    php_info_print_table_row(2, "Dump directory", meminfo_dump_directory());
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

static void meminfo_visit_exec_frame(meminfo_context *ctx, zend_execute_data *start_frame);
static void meminfo_browse_class_static_members(meminfo_context *ctx);
static void meminfo_browse_zvals_from_symbol_table(meminfo_context *ctx, char *frame_label, HashTable *symbol_table);
static void meminfo_zval_dump(meminfo_context *ctx, char *frame_label, zend_string *symbol_name, zval *zv);
static void meminfo_hash_write_children(meminfo_context *ctx, HashTable *ht, zend_bool is_object);

zend_module_entry meminfo_module_entry = {
    STANDARD_MODULE_HEADER,
    "meminfo",
    meminfo_functions,
    PHP_MINIT(meminfo),
    PHP_MSHUTDOWN(meminfo),
    PHP_RINIT(meminfo),
    PHP_RSHUTDOWN(meminfo),
    PHP_MINFO(meminfo),
    MEMINFO_VERSION,
    PHP_MODULE_GLOBALS(meminfo),
    PHP_GINIT(meminfo),
    NULL,                           /* globals shutdown */
    NULL,                           /* post deactivate */
    STANDARD_MODULE_PROPERTIES_EX
};


/**
 * Generate a JSON output of the list of items in memory (objects, arrays, string, etc...)
 * with their sizes and other information
 */
PHP_FUNCTION(meminfo_dump)
{
    zval *zval_stream;

    php_stream *stream;
    meminfo_writer *writer;
    zend_execute_data *caller_frame;
    int failed;

    // No throw here: PHP 8 has already thrown a TypeError or ArgumentCountError
    // of its own by this point, and 7.x has emitted a warning.
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zval_stream) == FAILURE) {
        return;
    }

    php_stream_from_zval(stream, zval_stream);

    // malloc, not emalloc: the staging buffer must not count towards the
    // memory_limit of the process being dumped.
    writer = malloc(sizeof(*writer));
    if (writer == NULL) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump(): cannot allocate the dump buffer");
        return;
    }

    writer->stream = stream;
    writer->fd = -1;
    writer->len = 0;
    writer->err = 0;

    // The frame of the meminfo_dump() call itself holds no user variables; the
    // dump starts at its caller.
    caller_frame = EG(current_execute_data) ? EG(current_execute_data)->prev_execute_data : NULL;

    // The global scope is reachable through the main script's frame here, so
    // walking EG(symbol_table) separately would only repeat work.
    meminfo_dump_to_writer(writer, caller_frame, 0);

    failed = writer->err;
    free(writer);

    if (failed) {
        // Reported rather than swallowed: a dump that stops halfway leaves
        // invalid JSON behind, and a caller that is told nothing will go on to
        // analyse it as if it were complete. Unlike the signal path, which
        // unlinks its temporary file, there is nothing to roll back here — the
        // bytes are already in the caller's stream.
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump(): writing the dump failed; the stream holds a partial dump");
    }
}

/**
 * Reserve one more level on the work stack.
 *
 * Returns NULL when the stack could not grow; the caller must then stop
 * descending, but the dump still has to be closed as valid JSON.
 */
static meminfo_level *meminfo_stack_push(meminfo_stack *stack)
{
    if (stack->len == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 64;
        meminfo_level *levels = realloc(stack->levels, capacity * sizeof(*levels));

        if (levels == NULL) {
            stack->exhausted = 1;
            return NULL;
        }

        stack->levels = levels;
        stack->capacity = capacity;
    }

    return &stack->levels[stack->len++];
}

/**
 * Finish a level once its whole subtree has been written.
 */
static void meminfo_level_close(meminfo_context *ctx, meminfo_level *level)
{
    free(level->slots);
    level->slots = NULL;

#if PHP_VERSION_ID >= 70400
    if (level->properties_to_release) {
        zend_release_properties(level->properties_to_release);
    }
#endif
#if PHP_VERSION_ID >= 80100
    if (level->pending_fiber) {
        meminfo_visit_exec_frame(ctx, level->pending_fiber);
    }
#else
    (void) ctx;
#endif
}

/**
 * Whether a level still owns work: children to visit, or something to close.
 */
static int meminfo_level_is_pending(const meminfo_level *level)
{
    if (level->ht) {
        return 1;
    }
    if (level->slots) {
        return 1;
    }
#if PHP_VERSION_ID >= 70400
    if (level->properties_to_release) {
        return 1;
    }
#endif
#if PHP_VERSION_ID >= 80100
    if (level->pending_fiber) {
        return 1;
    }
#endif
    return 0;
}

void meminfo_dump_to_writer(meminfo_writer *w, zend_execute_data *start_frame, int include_global_symbol_table)
{
    meminfo_context ctx;

    ctx.w = w;
    ctx.visited_items.slots = NULL;
    ctx.visited_items.capacity = 0;
    ctx.visited_items.count = 0;
    ctx.visited_items.failed = 0;
    ctx.first_element = 1;
    ctx.stack.levels = NULL;
    ctx.stack.len = 0;
    ctx.stack.capacity = 0;
    ctx.stack.exhausted = 0;
    ctx.aborted = 0;

    MEMINFO_WRITE_LIT(w, "{\n");

    MEMINFO_WRITE_LIT(w, "  \"header\" : {\n");
    MEMINFO_WRITE_LIT(w, "    \"memory_usage\" : ");
    meminfo_write_int(w, (int64_t) zend_memory_usage(0));
    MEMINFO_WRITE_LIT(w, ",\n    \"memory_usage_real\" : ");
    meminfo_write_int(w, (int64_t) zend_memory_usage(1));
    MEMINFO_WRITE_LIT(w, ",\n    \"peak_memory_usage\" : ");
    meminfo_write_int(w, (int64_t) zend_memory_peak_usage(0));
    MEMINFO_WRITE_LIT(w, ",\n    \"peak_memory_usage_real\" : ");
    meminfo_write_int(w, (int64_t) zend_memory_peak_usage(1));
    MEMINFO_WRITE_LIT(w, "\n  },\n");

    MEMINFO_WRITE_LIT(w, "  \"items\": {\n");
    meminfo_visit_exec_frame(&ctx, start_frame);

    if (include_global_symbol_table) {
        char global_label[] = "<GLOBAL>";

        // Harmless when the main frame was on the chain: those zvals are
        // already recorded and get skipped by address.
        meminfo_browse_zvals_from_symbol_table(&ctx, global_label, &EG(symbol_table));
    }

    meminfo_browse_class_static_members(&ctx);

    MEMINFO_WRITE_LIT(w, "\n    }\n");
    MEMINFO_WRITE_LIT(w, "}\n}\n");

    meminfo_writer_flush(w);

    free(ctx.stack.levels);
    free(ctx.visited_items.slots);
}

/**
 * Walk a chain of execution frames and record the variables each one declares.
 *
 * EG(current_execute_data) is deliberately never written here. Repointing it
 * corrupts the engine state when the chain being walked is not the running one
 * — most visibly for a suspended Fiber, whose frames are on a foreign stack.
 */
static void meminfo_visit_exec_frame(meminfo_context *ctx, zend_execute_data *start_frame)
{
    zend_execute_data *exec_frame;

    char frame_label[500];
    exec_frame = start_frame;

    while (exec_frame && !meminfo_aborted(ctx)) {
        // Internal frames (a call into a C function) declare no PHP variables.
        // Visiting them would attribute the variables of the surrounding user
        // function to the internal call, e.g. "outerVar@array_map()".
        if (!exec_frame->func || !ZEND_USER_CODE(exec_frame->func->common.type)) {
            goto handle_next;
        }

        if (meminfo_visit_item(&ctx->visited_items, exec_frame)) {
            goto handle_next;
        }

        if (exec_frame->prev_execute_data) {
            meminfo_build_frame_label(frame_label, sizeof(frame_label), exec_frame);
        } else {
            snprintf(frame_label, sizeof(frame_label), "<GLOBAL>");
        }

        // The global scope, include/eval and any frame reached by $$var or
        // extract() keep their variables in a real symbol table, which can hold
        // names beyond the compiled variables below.
        //
        // From 7.1 on, the presence of that table is recorded in the call info:
        // ex->symbol_table is left uninitialised when there is none, so the
        // pointer alone must not be trusted. PHP 7.0 has no such flag and
        // zeroes the pointer instead, which is what the engine checks there.
#if PHP_VERSION_ID >= 70100
        if (ZEND_CALL_INFO(exec_frame) & ZEND_CALL_HAS_SYMBOL_TABLE) {
#else
        if (exec_frame->symbol_table != NULL) {
#endif
            if (exec_frame->symbol_table) {
                meminfo_browse_zvals_from_symbol_table(ctx, frame_label, exec_frame->symbol_table);
            }
        } else {
            // Read the compiled variables straight out of the frame. Calling
            // zend_rebuild_symbol_table() instead would allocate a table per
            // frame and leave ZEND_CALL_HAS_SYMBOL_TABLE set on the process
            // being inspected, which the dump must not do.
            zend_op_array *op_array = &exec_frame->func->op_array;
            // last_var is a signed int in every supported version, and
            // ZEND_CALL_VAR_NUM casts its index to int as well.
            int i;

            for (i = 0; i < op_array->last_var; i++) {
                meminfo_zval_dump(ctx, frame_label, op_array->vars[i], ZEND_CALL_VAR_NUM(exec_frame, i));
            }
        }

handle_next:
        exec_frame = exec_frame->prev_execute_data;
    }
}

/**
 * Go through static members of classes
 */
static void meminfo_browse_class_static_members(meminfo_context *ctx)
{
    HashPosition ce_pos;
    HashPosition prop_pos;
    zend_class_entry *class_entry;
    zend_property_info * prop_info;

    char frame_label[500];
    char symbol_name[500];
    const char *prop_name, *class_name;
    zend_string * zstr_symbol_name;
    zval * prop;

    zend_hash_internal_pointer_reset_ex(CG(class_table), &ce_pos);
    while (!meminfo_aborted(ctx)
        && (class_entry = zend_hash_get_current_data_ptr_ex(CG(class_table), &ce_pos)) != NULL) {

#if PHP_VERSION_ID >= 70400
        if (class_entry->default_static_members_count > 0 && ZEND_MAP_PTR(class_entry->static_members_table) && CE_STATIC_MEMBERS(class_entry)) {
#else
        if (class_entry->static_members_table) {
#endif

            HashTable *properties_info = &(class_entry->properties_info);

            zend_hash_internal_pointer_reset_ex(properties_info, &prop_pos);

            while ((prop_info = zend_hash_get_current_data_ptr_ex(properties_info, &prop_pos)) != NULL) {

                if (prop_info->flags & ZEND_ACC_STATIC) {
                    snprintf(frame_label, sizeof(frame_label), "<CLASS_STATIC_MEMBER>");
#if PHP_VERSION_ID >= 70400
                    prop = CE_STATIC_MEMBERS(class_entry) + prop_info->offset;
#else
                    prop = &class_entry->static_members_table[prop_info->offset];
#endif

                    zend_unmangle_property_name(prop_info->name, &class_name, &prop_name);

                    if (class_name) {
                        snprintf(symbol_name, sizeof(symbol_name), "%s::%s",  class_name, prop_name);
                    } else {
                        snprintf(symbol_name, sizeof(symbol_name), "%s::%s",  ZSTR_VAL(class_entry->name), ZSTR_VAL(prop_info->name));
                    }

                    zstr_symbol_name = zend_string_init(symbol_name, strlen(symbol_name), 0);

                    meminfo_zval_dump(ctx, frame_label, zstr_symbol_name, prop);

                    zend_string_release(zstr_symbol_name);
                }

                zend_hash_move_forward_ex(properties_info, &prop_pos);
            }
        }

        zend_hash_move_forward_ex(CG(class_table), &ce_pos);
    }
}

static void meminfo_browse_zvals_from_symbol_table(meminfo_context *ctx, char *frame_label, HashTable *p_symbol_table)
{
    zval *zval_to_dump;
    HashPosition pos;

    zend_string *key;
    zend_long index;

    zend_hash_internal_pointer_reset_ex(p_symbol_table, &pos);

    while (!meminfo_aborted(ctx)
        && (zval_to_dump = zend_hash_get_current_data_ex(p_symbol_table, &pos)) != NULL) {

        zend_hash_get_current_key_ex(p_symbol_table, &key, &index, &pos);

        meminfo_zval_dump(ctx, frame_label, key, zval_to_dump);

        zend_hash_move_forward_ex(p_symbol_table, &pos);
    }
}

/* Multiplier and shift for the pointer hash, per pointer width. */
#if defined(UINTPTR_MAX) && UINTPTR_MAX > 0xFFFFFFFFU
# define MEMINFO_PTR_HASH_MULT ((uintptr_t) 0x9E3779B97F4A7C15ULL)
# define MEMINFO_PTR_HASH_SHIFT 29
#else
# define MEMINFO_PTR_HASH_MULT ((uintptr_t) 0x9E3779B9U)
# define MEMINFO_PTR_HASH_SHIFT 15
#endif

/**
 * Fibonacci hash of an item address.
 *
 * Allocator addresses are aligned, so their low bits are constant and would
 * collide in long probe chains if used directly. The alignment bits are dropped
 * and the result mixed so that the low bits taken by the mask vary.
 */
static size_t meminfo_ptr_slot(uintptr_t ptr, size_t mask)
{
    uintptr_t h = ptr >> 3;

    h *= MEMINFO_PTR_HASH_MULT;
    h ^= h >> MEMINFO_PTR_HASH_SHIFT;

    return (size_t) h & mask;
}

/**
 * Double the set, or allocate it on first use. Returns 0 on failure.
 */
static int meminfo_ptr_set_grow(meminfo_ptr_set *set)
{
    size_t capacity = set->capacity ? set->capacity * 2 : 1024;
    uintptr_t *slots;
    size_t i;

    if (capacity < set->capacity) {   /* size_t overflow, far beyond any real heap */
        set->failed = 1;
        return 0;
    }

    slots = calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        set->failed = 1;
        return 0;
    }

    for (i = 0; i < set->capacity; i++) {
        uintptr_t key = set->slots[i];

        if (key != 0) {
            size_t slot = meminfo_ptr_slot(key, capacity - 1);

            while (slots[slot] != 0) {
                slot = (slot + 1) & (capacity - 1);
            }
            slots[slot] = key;
        }
    }

    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;

    return 1;
}

/**
 * Record an item address as visited.
 *
 * Returns 1 when the address had already been recorded, which is also what is
 * reported once an allocation has failed, so that the traversal stops.
 */
static int meminfo_visit_item(meminfo_ptr_set *set, const void *ptr)
{
    uintptr_t key = (uintptr_t) ptr;
    size_t mask;
    size_t slot;

    if (set->failed || key == 0) {
        // Reporting "already visited" is what stops the traversal: nothing more
        // is written, and the caller still closes the JSON.
        return 1;
    }

    // Grow at half load; linear probing degrades sharply past that.
    if ((set->count + 1) * 2 > set->capacity) {
        if (!meminfo_ptr_set_grow(set)) {
            return 1;
        }
    }

    mask = set->capacity - 1;
    slot = meminfo_ptr_slot(key, mask);

    while (set->slots[slot] != 0) {
        if (set->slots[slot] == key) {
            return 1;
        }
        slot = (slot + 1) & mask;
    }

    set->slots[slot] = key;
    set->count++;

    return 0;
}

/**
 * Write the "children" block of an item: one key -> child identifier pair per
 * entry of ht. The children themselves are visited later, by the traversal
 * loop in meminfo_zval_dump().
 */
/**
 * The address a child is identified by in the dump.
 *
 * References and indirect slots are followed first, and an object is keyed by
 * its zend_object, so that several zvals pointing at the same object collapse
 * into one item.
 */
static const void *meminfo_child_identity(zval *value)
{
    if (Z_TYPE_P(value) == IS_INDIRECT) {
        value = Z_INDIRECT_P(value);
    }

    if (Z_ISREF_P(value)) {
        ZVAL_DEREF(value);
    }

    if (Z_TYPE_P(value) == IS_OBJECT) {
        return (const void *) Z_OBJ_P(value);
    }

    return (const void *) value;
}

/**
 * Begin one entry of a "children" block, up to the opening quote of its name.
 */
static void meminfo_child_entry_open(meminfo_context *ctx, int *first_child)
{
    meminfo_writer *w = ctx->w;

    if (!*first_child) {
        MEMINFO_WRITE_LIT(w, ",\n");
    } else {
        *first_child = 0;
    }

    MEMINFO_WRITE_LIT(w, "            \"");
}

/**
 * Finish one entry of a "children" block: the name is already written, this
 * adds the identifier it points at.
 */
static void meminfo_child_entry_close(meminfo_context *ctx, const void *child)
{
    meminfo_writer *w = ctx->w;

    MEMINFO_WRITE_LIT(w, "\":\"");
    meminfo_write_ptr(w, child);
    MEMINFO_WRITE_LIT(w, "\"");
}

static void meminfo_hash_write_children(meminfo_context *ctx, HashTable *ht, zend_bool is_object)
{
    meminfo_writer *w = ctx->w;
    zval *entry;

    zend_string *key;
    HashPosition pos;
    zend_ulong num_key;

    int first_child = 1;

    MEMINFO_WRITE_LIT(w, "        \"children\" : {\n");

    zend_hash_internal_pointer_reset_ex(ht, &pos);
    while ((entry = zend_hash_get_current_data_ex(ht, &pos)) != NULL) {
        const void *child = meminfo_child_identity(entry);

        meminfo_child_entry_open(ctx, &first_child);

        switch (zend_hash_get_current_key_ex(ht, &key, &num_key, &pos)) {
            case HASH_KEY_IS_STRING:

                if (is_object) {
                    const char *property_name, *class_name;

                    zend_unmangle_property_name(key, &class_name, &property_name);

                    meminfo_write_json_escaped(w, property_name);
                } else {
                    meminfo_write_json_escaped(w, ZSTR_VAL(key));
                }

                break;
            case HASH_KEY_IS_LONG:
                meminfo_write_int(w, (int64_t) (zend_long) num_key);
                break;
        }

        meminfo_child_entry_close(ctx, child);

        zend_hash_move_forward_ex(ht, &pos);
    }
    MEMINFO_WRITE_LIT(w, "\n        }\n");
}

/**
 * Whether an object's declared properties can be read without building its
 * property table.
 *
 * Only plain objects qualify. A class with its own get_properties handler
 * (ArrayObject, SplFixedArray, DateInterval, protobuf messages, …) may expose
 * something other than its declared slots, and an object that already has a
 * property table may additionally carry dynamic properties — both keep the
 * handler path.
 */
static int meminfo_object_has_plain_properties(zend_object *zobj)
{
    return zobj->properties == NULL
        && zobj->handlers->get_properties == zend_std_get_properties;
}

/**
 * List the declared properties of an object, in the order the engine itself
 * would have put them into the object's property table.
 *
 * Calling get_properties() instead would run rebuild_object_properties(),
 * which attaches that table to the object and leaves it there: tens of
 * megabytes of permanent growth over a large object graph, charged to the
 * process the dump is supposed to observe without disturbing.
 *
 * The order differs by version and must be reproduced exactly, because
 * unmangled private names can collide in the JSON and the last duplicate wins
 * when the dump is decoded. Returns 0 if the list could not be allocated.
 */
static int meminfo_collect_declared_properties(zend_object *zobj, meminfo_prop_slot **out_slots, uint32_t *out_count)
{
    zend_class_entry *ce = zobj->ce;
    meminfo_prop_slot *slots;
    uint32_t capacity;
    uint32_t count = 0;

    *out_slots = NULL;
    *out_count = 0;

    if (ce->default_properties_count <= 0) {
        return 1;
    }

#if PHP_VERSION_ID >= 80100
    // properties_info_table has exactly one entry per slot, so this is exact.
    capacity = (uint32_t) ce->default_properties_count;
#else
    // An upper bound derived from what the two passes below actually iterate,
    // rather than from the assumption that each emitted property owns a
    // distinct slot. That assumption holds, but if it ever stopped holding the
    // list would silently lose a child instead of saying so.
    {
        zend_class_entry *parent;

        capacity = zend_hash_num_elements(&ce->properties_info);

        for (parent = ce->parent; parent && parent->default_properties_count; parent = parent->parent) {
            capacity += zend_hash_num_elements(&parent->properties_info);
        }
    }
#endif

    if (capacity == 0) {
        return 1;
    }

    slots = malloc((size_t) capacity * sizeof(*slots));
    if (slots == NULL) {
        return 0;
    }

#if PHP_VERSION_ID >= 80100
    // 8.1 fills the property table straight from properties_info_table, i.e.
    // in slot order, parent classes first.
    {
        uint32_t i;

        for (i = 0; i < capacity; i++) {
            zend_property_info *info = ce->properties_info_table[i];

            if (info == NULL) {
                // Dead slot: left behind by a property a child class redeclared.
                continue;
            }

            slots[count].name = info->name;
            slots[count].value = OBJ_PROP(zobj, info->offset);
            count++;
        }
    }
#else
    // 7.0 - 8.0 walk ce->properties_info in hash order, which lists a class's
    // own properties before the inherited ones.
    //
    // That hash is keyed by the *unmangled* name, so when a class redeclares a
    // private property of a parent, the parent's entry is displaced and its
    // slot would be missed. The engine appends those separately, and so must
    // this: a shadowed parent private still exists as its own slot and still
    // appears in the dump.
    {
        zend_property_info *info;
        HashPosition pos;
        uint32_t flags_seen = 0;

        zend_hash_internal_pointer_reset_ex(&ce->properties_info, &pos);
        while (count < capacity
            && (info = zend_hash_get_current_data_ptr_ex(&ce->properties_info, &pos)) != NULL) {

            if (!(info->flags & ZEND_ACC_STATIC)) {
                slots[count].name = info->name;
                slots[count].value = OBJ_PROP(zobj, info->offset);
                count++;
                flags_seen |= info->flags;
            }

            zend_hash_move_forward_ex(&ce->properties_info, &pos);
        }

        // ZEND_ACC_CHANGED marks a property that redeclares an inherited one;
        // without it nothing can have been displaced.
        if (flags_seen & ZEND_ACC_CHANGED) {
            zend_class_entry *parent;

            for (parent = ce->parent; parent && parent->default_properties_count; parent = parent->parent) {
                HashPosition parent_pos;

                zend_hash_internal_pointer_reset_ex(&parent->properties_info, &parent_pos);
                while (count < capacity
                    && (info = zend_hash_get_current_data_ptr_ex(&parent->properties_info, &parent_pos)) != NULL) {

                    zend_string *key = NULL;
                    zend_ulong num_key;

                    zend_hash_get_current_key_ex(&parent->properties_info, &key, &num_key, &parent_pos);

                    if (key != NULL
                        && info->ce == parent
                        && (info->flags & ZEND_ACC_PRIVATE)
                        && !(info->flags & ZEND_ACC_STATIC)) {

                        zend_property_info *visible = zend_hash_find_ptr(&ce->properties_info, key);

                        // Emitted by the walk above already unless the name now
                        // resolves to a different slot, i.e. it was redeclared.
                        if (visible == NULL
                            || (visible->flags & ZEND_ACC_STATIC)
                            || visible->offset != info->offset) {

                            slots[count].name = info->name;
                            slots[count].value = OBJ_PROP(zobj, info->offset);
                            count++;
                        }
                    }

                    zend_hash_move_forward_ex(&parent->properties_info, &parent_pos);
                }
            }
        }
    }
#endif

    *out_slots = slots;
    *out_count = count;

    return 1;
}

/**
 * Write the "children" block from a list of declared property slots.
 *
 * Mirrors the object branch of meminfo_hash_write_children() so that both
 * sources of children produce the same bytes.
 */
static void meminfo_slots_write_children(meminfo_context *ctx, meminfo_prop_slot *slots, uint32_t count)
{
    meminfo_writer *w = ctx->w;
    uint32_t i;
    int first_child = 1;

    MEMINFO_WRITE_LIT(w, "        \"children\" : {\n");

    for (i = 0; i < count; i++) {
        const void *child = meminfo_child_identity(slots[i].value);
        const char *property_name, *class_name;

        meminfo_child_entry_open(ctx, &first_child);

        zend_unmangle_property_name(slots[i].name, &class_name, &property_name);
        meminfo_write_json_escaped(w, property_name);

        meminfo_child_entry_close(ctx, child);
    }

    MEMINFO_WRITE_LIT(w, "\n        }\n");
}


/**
 * Write one item and report what is left to visit underneath it.
 *
 * Returns 0 when the item had already been visited (or when the traversal has
 * been stopped), in which case nothing was written and *level is meaningless.
 * Otherwise the item and its "children" block are written and *level describes
 * the children to visit and the resources to release afterwards.
 */
static int meminfo_item_open(meminfo_context *ctx, char *frame_label, zend_string *symbol_name, zval *zv, meminfo_level *level)
{
    meminfo_writer *w = ctx->w;
    const void *item_ptr;

    level->ht = NULL;
    level->pos = 0;
    level->slots = NULL;
    level->slot_count = 0;
    level->slot_pos = 0;
#if PHP_VERSION_ID >= 70400
    level->properties_to_release = NULL;
#endif
#if PHP_VERSION_ID >= 80100
    level->pending_fiber = NULL;
#endif

    if (meminfo_aborted(ctx)) {
        return 0;
    }

    if (Z_TYPE_P(zv) == IS_INDIRECT) {
        zv = Z_INDIRECT_P(zv);
    }

    if (Z_ISREF_P(zv)) {
        ZVAL_DEREF(zv);
    }

    // Objects are identified by their zend_object, so that several zvals
    // pointing at the same object collapse into one item.
    if (Z_TYPE_P(zv) == IS_OBJECT) {
        item_ptr = Z_OBJ_P(zv);
    } else {
        item_ptr = zv;
    }

    if (meminfo_visit_item(&ctx->visited_items, item_ptr)) {
        return 0;
    }

    if (! ctx->first_element) {
        MEMINFO_WRITE_LIT(w, "\n    },\n");
    } else {
        ctx->first_element = 0;
    }

    const char *type_name = Z_ISUNDEF_P(zv) ? "null" : zend_get_type_by_const(Z_TYPE_P(zv));

    MEMINFO_WRITE_LIT(w, "    \"");
    meminfo_write_ptr(w, item_ptr);
    MEMINFO_WRITE_LIT(w, "\" : {\n        \"type\" : \"");
    // zend_get_type_by_const() has no name for engine-internal types
    meminfo_write_json_escaped(w, type_name ? type_name : "unknown");
    MEMINFO_WRITE_LIT(w, "\",\n        \"size\" : \"");
    meminfo_write_int(w, (int64_t) meminfo_get_element_size(zv));
    MEMINFO_WRITE_LIT(w, "\",\n");

    if (frame_label) {
        if (symbol_name) {
            MEMINFO_WRITE_LIT(w, "        \"symbol_name\" : \"");
            meminfo_write_json_escaped(w, ZSTR_VAL(symbol_name));
            MEMINFO_WRITE_LIT(w, "\",\n");
        }

        MEMINFO_WRITE_LIT(w, "        \"is_root\" : true,\n        \"frame\" : \"");
        meminfo_write_json_escaped(w, frame_label);
        MEMINFO_WRITE_LIT(w, "\"\n");
    } else {
        MEMINFO_WRITE_LIT(w, "        \"is_root\" : false\n");
    }

    if (Z_TYPE_P(zv) == IS_OBJECT) {
        HashTable *properties;
        zend_string *class_name;

        properties = NULL;
        class_name = Z_OBJCE_P(zv)->name;

        MEMINFO_WRITE_LIT(w, ",\n        \"class\" : \"");
        meminfo_write_json_escaped(w, ZSTR_VAL(class_name));
        MEMINFO_WRITE_LIT(w, "\",\n        \"object_handle\" : \"");
        meminfo_write_int(w, (int64_t) (int) Z_OBJ_HANDLE_P(zv));
        MEMINFO_WRITE_LIT(w, "\",\n");

        zend_object *zobj = Z_OBJ_P(zv);

        if (meminfo_object_has_plain_properties(zobj)) {
            // Read the declared slots directly: asking the handler would
            // materialise a property table on the inspected object and leave
            // it there.
            meminfo_prop_slot *slots;
            uint32_t slot_count;

            if (meminfo_collect_declared_properties(zobj, &slots, &slot_count)) {
                meminfo_slots_write_children(ctx, slots, slot_count);

                level->slots = slots;
                level->slot_count = slot_count;
            } else {
                ctx->aborted = 1;
                MEMINFO_WRITE_LIT(w, "        \"children\" : {\n        }\n");
            }
        } else {
#if PHP_VERSION_ID >= 80000
            properties = zobj->handlers->get_properties(zobj);
            if (properties) {
                GC_TRY_ADDREF(properties);
            }
#elif PHP_VERSION_ID >= 70400
            /* PHP 7.4: get_properties takes a zval and GC_TRY_ADDREF does not exist yet */
            properties = Z_OBJ_HANDLER_P(zv, get_properties) ? Z_OBJ_HANDLER_P(zv, get_properties)(zv) : NULL;
            if (properties &&!(GC_FLAGS(properties) & GC_IMMUTABLE)) {
                GC_ADDREF(properties);
            }
#else
            /* Borrowed reference: get_properties never calls __debugInfo and needs no release */
            properties = Z_OBJ_HANDLER_P(zv, get_properties) ? Z_OBJPROP_P(zv) : NULL;
#endif

            if (properties != NULL) {
                meminfo_hash_write_children(ctx, properties, 1);

                // The properties table has to stay alive — and, on 7.4+, stay
                // referenced — until the whole subtree below it has been visited.
                level->ht = properties;
                zend_hash_internal_pointer_reset_ex(properties, &level->pos);
#if PHP_VERSION_ID >= 70400
                level->properties_to_release = properties;
#endif
            } else {
                // Some internal classes (e.g. ext-protobuf messages) return NULL from get_properties:
                // an empty children list closes the item after the trailing comma above
                MEMINFO_WRITE_LIT(w, "        \"children\" : {\n        }\n");
            }
        }
#if PHP_VERSION_ID >= 80100
        if (strcmp(ZSTR_VAL(class_name), "Fiber") == 0) {
            zend_object *object = Z_OBJ_P(zv);
            zend_fiber *fiber = (zend_fiber *) object;
            if (fiber->execute_data) {
                level->pending_fiber = fiber->execute_data;
            }
        }
#endif
    } else if (Z_TYPE_P(zv) == IS_ARRAY) {
        MEMINFO_WRITE_LIT(w, ",\n");
        meminfo_hash_write_children(ctx, Z_ARRVAL_P(zv), 0);

        level->ht = Z_ARRVAL_P(zv);
        zend_hash_internal_pointer_reset_ex(level->ht, &level->pos);
    } else {
        MEMINFO_WRITE_LIT(w, "\n");
    }

    return 1;
}

/**
 * Write the item behind zv and everything reachable from it.
 *
 * The object graph is walked with an explicit stack rather than by recursing:
 * a linked structure a few tens of thousands of levels deep used to overflow
 * the C stack and kill the process being inspected.
 *
 * Visiting order is identical to the recursive version — an item, then its
 * "children" block, then each child's subtree in table order — so the dump is
 * byte for byte the same.
 */
static void meminfo_zval_dump(meminfo_context *ctx, char *frame_label, zend_string *symbol_name, zval *zv)
{
    // Levels below this one belong to an outer call: the fiber hook re-enters
    // this function while levels are already stacked, and must not drain them.
    size_t base = ctx->stack.len;
    meminfo_level level;

    if (meminfo_item_open(ctx, frame_label, symbol_name, zv, &level)) {
        if (meminfo_level_is_pending(&level)) {
            meminfo_level *pushed = meminfo_stack_push(&ctx->stack);

            if (pushed) {
                *pushed = level;
            } else {
                meminfo_level_close(ctx, &level);
            }
        }
    }

    while (ctx->stack.len > base) {
        meminfo_level *top = &ctx->stack.levels[ctx->stack.len - 1];
        zval *child = NULL;

        // Advance before visiting: visiting may grow — and therefore move —
        // the stack, which would leave top dangling.
        if (top->slots) {
            if (top->slot_pos < top->slot_count) {
                child = top->slots[top->slot_pos].value;
                top->slot_pos++;
            }
        } else if (top->ht) {
            child = zend_hash_get_current_data_ex(top->ht, &top->pos);

            if (child) {
                zend_hash_move_forward_ex(top->ht, &top->pos);
            }
        }

        if (child == NULL) {
            meminfo_level done = *top;

            ctx->stack.len--;
            meminfo_level_close(ctx, &done);
            continue;
        }

        if (meminfo_item_open(ctx, NULL, NULL, child, &level)) {
            if (meminfo_level_is_pending(&level)) {
                meminfo_level *pushed = meminfo_stack_push(&ctx->stack);

                if (pushed) {
                    *pushed = level;
                } else {
                    meminfo_level_close(ctx, &level);
                }
            }
        }
    }
}

/**
 * Get size of an element
 *
 * @param zval *zv Zval of the element
 *
 * @return zend_ulong
 */
zend_ulong meminfo_get_element_size(zval *zv)
{
    zend_ulong size;

    size = sizeof(zval);

    switch (Z_TYPE_P(zv)) {
        case IS_STRING:
            size += Z_STRLEN_P(zv);
            break;

        // TODO: add size of the indexes
        case IS_ARRAY:
            size += sizeof(HashTable);
            break;

        // TODO: add size of the properties table, but without property content
        case IS_OBJECT:
            size += sizeof(zend_object);
            break;
    }

    return size;
}

/**
 * Build the current frame label based on function name and object class
 * if necessary.
 *
 * Most code comes from the debug_print_backtrace implementation.
 */
void meminfo_build_frame_label(char* frame_label, int frame_label_len, zend_execute_data* frame)
{
    zend_function *func;
    const char *function_name;
    char * call_type;
    zend_string *class_name = NULL;
    zend_object *object;
    zend_execute_data *ptr;

    object = Z_OBJ(frame->This);
    ptr = frame->prev_execute_data;

    if (frame->func) {
        func = frame->func;

#if PHP_VERSION_ID >= 80000
        if (func->common.function_name) {
            function_name = ZSTR_VAL(func->common.function_name);
        } else {
            function_name = NULL;
        }
#else
        function_name = (func->common.scope &&
                         func->common.scope->trait_aliases) ?
            ZSTR_VAL(zend_resolve_method_name(
                (object ? object->ce : func->common.scope), func)) :
            (func->common.function_name ?
                ZSTR_VAL(func->common.function_name) : NULL);
#endif
    } else {
        func = NULL;
        function_name = NULL;
    }

    if (function_name) {
        if (object) {
            if (func->common.scope) {
                class_name = func->common.scope->name;
            } else {
                class_name = object->ce->name;
            }

            call_type = "->";
        } else if (func->common.scope) {
            class_name = func->common.scope->name;
            call_type = "::";
        } else {
            class_name = NULL;
            call_type = NULL;
        }
    } else {

        if (!ptr->func || !ZEND_USER_CODE(ptr->func->common.type) || ptr->opline->opcode != ZEND_INCLUDE_OR_EVAL) {
            /* can happen when calling eval from a custom sapi */
            function_name = "unknown";
        } else
        switch (ptr->opline->extended_value) {
            case ZEND_EVAL:
                function_name = "eval";
                break;
            case ZEND_INCLUDE:
                function_name = "include";
                break;
            case ZEND_REQUIRE:
                function_name = "require";
                break;
            case ZEND_INCLUDE_ONCE:
                function_name = "include_once";
                break;
            case ZEND_REQUIRE_ONCE:
                function_name = "require_once";
                break;
            default:
                /* this can actually happen if you're in your error_handler and
                 * you're in the top-scope */
                function_name = "unknown";
                break;
        }
    }
    if (class_name) {
        snprintf(frame_label, frame_label_len, "%s%s%s()", ZSTR_VAL(class_name), call_type, function_name);
    } else {
        snprintf(frame_label, frame_label_len, "%s()", function_name);
    }
}

#ifndef PHP_WIN32

/**
 * One place the dump may turn up.
 *
 * A target in its own mount namespace (systemd PrivateTmp, a container) writes
 * to a /tmp the trigger cannot see under that name, but which is reachable
 * through /proc/<pid>/root for the same user or for root with CAP_SYS_PTRACE.
 */
typedef struct {
    char final_path[MAXPATHLEN];
    char temp_path[MAXPATHLEN + 8];
    int usable;
} meminfo_dump_location;

static void meminfo_dump_location_init(meminfo_dump_location *location, const char *prefix, const char *dir, int dump_id)
{
    int written;

    location->usable = 0;

    written = snprintf(location->final_path, sizeof(location->final_path),
        "%s%s/meminfo-%d.json", prefix, dir, dump_id);

    if (written <= 0 || (size_t) written >= sizeof(location->final_path)) {
        return;
    }

    written = snprintf(location->temp_path, sizeof(location->temp_path),
        "%s.tmp", location->final_path);

    if (written <= 0 || (size_t) written >= sizeof(location->temp_path)) {
        return;
    }

    location->usable = 1;
}

/**
 * Ask a running process to dump its memory, and wait for the result.
 *
 * The target does the work at the next safe point between opcodes, so this
 * returns as soon as the dump file has been completely written and renamed
 * into place — or false, with a warning explaining which step failed.
 */
PHP_FUNCTION(meminfo_dump_process)
{
    zend_long pid_arg;
    zval *options = NULL;

    pid_t pid;
    int signal_number;
    int dump_id;
    zend_long timeout_seconds = 30;
    int force = 0;
    const char *dir;
    char dir_buffer[MAXPATHLEN];
    char proc_root_prefix[64];

    meminfo_dump_location locations[2];
    int location_count = 1;
    int i;

    int caught;
    unsigned long long start_time = 0;
    int have_start_time;
    uint64_t began;
    uint64_t last_resend;
    uint64_t last_growth;
    off_t last_size = -1;
    int temp_seen = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l|a", &pid_arg, &options) == FAILURE) {
        return;
    }

    /* --- options ------------------------------------------------------- */

    signal_number = MEMINFO_G(dump_signal);
#if !MEMINFO_HAVE_SIGNAL_DUMP
    // This build cannot serve a dump, so the ini value was forced to 0; the
    // default signal is still the right thing to send to a target that can.
    if (signal_number == 0) {
        meminfo_parse_signal(MEMINFO_DEFAULT_DUMP_SIGNAL, &signal_number);
    }
#endif

    dir = meminfo_dump_directory();

    if (options != NULL) {
        zval *option;
        HashTable *option_table = Z_ARRVAL_P(options);

        option = zend_hash_str_find(option_table, "dir", sizeof("dir") - 1);
        if (option != NULL && Z_TYPE_P(option) == IS_STRING && Z_STRLEN_P(option) > 0) {
            if (Z_STRLEN_P(option) >= sizeof(dir_buffer)) {
                zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
                    "meminfo_dump_process(): dir is too long");
                return;
            }
            memcpy(dir_buffer, Z_STRVAL_P(option), Z_STRLEN_P(option) + 1);
            dir = dir_buffer;
        }

        option = zend_hash_str_find(option_table, "signal", sizeof("signal") - 1);
        if (option != NULL) {
            int parsed = 0;

            if (Z_TYPE_P(option) == IS_STRING) {
                if (!meminfo_parse_signal(Z_STRVAL_P(option), &parsed)) {
                    zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
                        "meminfo_dump_process(): unknown signal \"%s\"", Z_STRVAL_P(option));
                    return;
                }
            } else {
                parsed = (int) zval_get_long(option);
            }

            signal_number = parsed;
        }

        option = zend_hash_str_find(option_table, "timeout", sizeof("timeout") - 1);
        if (option != NULL) {
            timeout_seconds = zval_get_long(option);
        }

        option = zend_hash_str_find(option_table, "force", sizeof("force") - 1);
        if (option != NULL) {
            force = zend_is_true(option);
        }
    }

    if (pid_arg <= 0) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): pid must be greater than 0");
        return;
    }

    pid = (pid_t) pid_arg;

    if (pid == getpid()) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): cannot dump the calling process; use meminfo_dump()");
        return;
    }

    if (signal_number <= 0) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): no dump signal configured; set meminfo.dump_signal "
            "or pass the \"signal\" option");
        return;
    }

    if (timeout_seconds < 1) {
        timeout_seconds = 1;
    }

    /* --- is the target listening? -------------------------------------- */

    caught = meminfo_signal_is_caught(pid, signal_number);

    if (caught == 0 && !meminfo_signal_is_harmless(signal_number)) {
        // Deliberately not overridable by "force". The target is known not to
        // handle this signal, so delivering it cannot produce a dump — it can
        // only terminate or stop the target. Allowing it would turn a
        // diagnostic function into a way to kill arbitrary processes, which is
        // a capability this extension has no business providing.
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d does not handle signal %d, whose default "
            "action would terminate or stop it; refusing to send it. The target may not have "
            "meminfo loaded, may be a PHP 7.0 or ZTS build, or may not be running a request",
            (int) pid, signal_number);
        return;
    }

    if (caught == 0 && !force) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d does not handle signal %d: meminfo is not "
            "loaded, the target is PHP 7.0 or a ZTS build, or no request is running. "
            "Pass \"force\" => true to send it anyway",
            (int) pid, signal_number);
        return;
    }

    if (caught < 0 && !force) {
        int default_signal = 0;

        meminfo_parse_signal(MEMINFO_DEFAULT_DUMP_SIGNAL, &default_signal);

        // Without /proc there is no way to know whether the signal is handled.
        // Sending the default is safe because its default action is to ignore;
        // anything else could kill a target that is not listening.
        if (signal_number != default_signal) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): cannot tell whether process %d handles signal %d "
                "(no /proc on this system) and its default action may terminate the target. "
                "Pass \"force\" => true to send it anyway",
                (int) pid, signal_number);
            return;
        }
    }

    have_start_time = meminfo_process_start_time(pid, &start_time);

    /* --- where the dump will appear ------------------------------------ */

    dump_id = meminfo_random_dump_id();

    meminfo_dump_location_init(&locations[0], "", dir, dump_id);

    if (snprintf(proc_root_prefix, sizeof(proc_root_prefix), "/proc/%d/root", (int) pid) > 0) {
        meminfo_dump_location_init(&locations[1], proc_root_prefix, dir, dump_id);
        if (locations[1].usable) {
            location_count = 2;
        }
    }

    if (!locations[0].usable) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): dump path is too long");
        return;
    }

    /* --- ask ----------------------------------------------------------- */

#ifdef HAVE_SIGQUEUE
    {
        union sigval value;

        memset(&value, 0, sizeof(value));
        value.sival_int = dump_id;

        if (sigqueue(pid, signal_number, value) != 0) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): cannot signal process %d: %s",
                (int) pid, strerror(errno));
            return;
        }
    }
#else
    // Without sigqueue() the id cannot be delivered, so the target names the
    // dump after its pid and the time instead. Nothing here can distinguish
    // two concurrent requests, so the first matching file wins.
    if (kill(pid, signal_number) != 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): cannot signal process %d: %s",
            (int) pid, strerror(errno));
        return;
    }
#endif

    /* --- wait ---------------------------------------------------------- */

    began = meminfo_now_usec();
    last_resend = began;
    last_growth = began;

    for (;;) {
        struct stat info;

        // The finished dump, under either name.
        for (i = 0; i < location_count; i++) {
            if (locations[i].usable
                && stat(locations[i].final_path, &info) == 0
                && S_ISREG(info.st_mode)) {

                if (!meminfo_dump_file_is_trustworthy(locations[i].final_path, pid)) {
                    zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                        "meminfo_dump_process(): %s is not a regular file owned by process %d; "
                        "refusing to use it",
                        locations[i].final_path, (int) pid);
                    return;
                }

                RETURN_STRING(locations[i].final_path);
            }
        }

        // A partial dump means the target is working; from here on the wait is
        // bounded by progress rather than by the timeout, because a large heap
        // on a slow disk can legitimately take minutes.
        for (i = 0; i < location_count; i++) {
            if (locations[i].usable && stat(locations[i].temp_path, &info) == 0) {
                temp_seen = 1;

                if (info.st_size != last_size) {
                    last_size = info.st_size;
                    last_growth = meminfo_now_usec();
                }
                break;
            }
        }

        if (temp_seen) {
            if (meminfo_now_usec() - last_growth > (uint64_t) timeout_seconds * 1000000ULL) {
                zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                    "meminfo_dump_process(): the dump of process %d stopped growing for %ld "
                    "seconds; giving up", (int) pid, (long) timeout_seconds);
                return;
            }
        } else if (meminfo_now_usec() - began > (uint64_t) timeout_seconds * 1000000ULL) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): process %d did not start a dump within %ld seconds",
                (int) pid, (long) timeout_seconds);
            return;
        }

        /* --- has the target gone away? --- */

        if (kill(pid, 0) != 0 && errno == ESRCH) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): process %d exited during the dump", (int) pid);
            goto failed_remove_partial;
        }

        if (have_start_time) {
            unsigned long long current_start_time = 0;

            if (meminfo_process_start_time(pid, &current_start_time)
                && current_start_time != start_time) {

                zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                    "meminfo_dump_process(): process %d exited during the dump and its pid "
                    "was reused", (int) pid);
                goto failed_remove_partial;
            }
        }

        if (!temp_seen) {
            // The handler is only installed while a request runs. If it has
            // gone and nothing was written, the request finished before
            // reaching a safe point.
            if (meminfo_signal_is_caught(pid, signal_number) == 0) {
                zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                    "meminfo_dump_process(): the request in process %d ended before the dump "
                    "started", (int) pid);
                return;
            }

            // Standard signals coalesce, so one sent while the target was busy
            // may be the only one it ever sees; resending is harmless because
            // the id — and therefore the file name — stays the same.
            if (meminfo_now_usec() - last_resend >= MEMINFO_RESEND_INTERVAL_USEC) {
#ifdef HAVE_SIGQUEUE
                union sigval value;

                memset(&value, 0, sizeof(value));
                value.sival_int = dump_id;
                sigqueue(pid, signal_number, value);
#else
                kill(pid, signal_number);
#endif
                last_resend = meminfo_now_usec();
            }
        }

        meminfo_sleep_usec(MEMINFO_POLL_INTERVAL_USEC);
    }

failed_remove_partial:
    // Reached with an exception already pending; the partial dump the target
    // left behind is of no use to anyone and is removed before unwinding.
    for (i = 0; i < location_count; i++) {
        if (locations[i].usable) {
            unlink(locations[i].temp_path);
        }
    }
}

#endif /* !PHP_WIN32 */

#ifdef COMPILE_DL_MEMINFO
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(meminfo)
#endif
