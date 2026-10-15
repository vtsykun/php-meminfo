<?php
/**
 * What a dump costs the process being dumped.
 *
 *   retained   memory still held after the dump returns. A writer that asks
 *              objects for their property table, or rebuilds a symbol table per
 *              frame, leaves those behind — growth proportional to the size of
 *              the object graph, in a process already being investigated for
 *              using too much memory.
 *   transient  peak during the dump. This is what runs into memory_limit.
 *   rss        resident size, which the Zend allocator does not return to the
 *              OS on its own.
 *
 * Usage:
 *   php -d extension=.../meminfo.so memory.php objects [count]
 *   php -d extension=.../meminfo.so memory.php frames  [depth]
 *
 * The dump is written to a real file, never to php://memory: that stream keeps
 * its buffer in the Zend allocator and would be counted as the dump's own cost.
 */

$mode  = isset($argv[1]) ? $argv[1] : 'objects';
$count = isset($argv[2]) ? (int) $argv[2] : 100000;

class ReproDeclared
{
    public $a = 1;
    protected $b = 2;
    private $c = 3;
    public $d = 'four';
}

/**
 * @return int resident size in bytes, or 0 where it cannot be read
 */
function repro_rss()
{
    if (!is_readable('/proc/self/status')) {
        return 0;
    }

    if (preg_match('/^VmRSS:\s+(\d+)\s+kB/m', file_get_contents('/proc/self/status'), $match)) {
        return (int) $match[1] * 1024;
    }

    return 0;
}

function repro_measure($label, $count)
{
    $dumpFile = tempnam(sys_get_temp_dir(), 'meminfo-repro');

    gc_collect_cycles();
    $usageBefore = memory_get_usage();
    $peakBefore  = memory_get_peak_usage();
    $rssBefore   = repro_rss();

    $started = microtime(true);
    $handle = fopen($dumpFile, 'w');
    meminfo_dump($handle);
    fclose($handle);
    $elapsed = microtime(true) - $started;

    gc_collect_cycles();
    $usageAfter = memory_get_usage();
    $peakAfter  = memory_get_peak_usage();
    $rssAfter   = repro_rss();

    printf(
        "%s=%d seconds=%.2f dump_bytes=%d retained=%+.2fMB transient_peak=%+.2fMB rss=%+.2fMB\n",
        $label,
        $count,
        $elapsed,
        filesize($dumpFile),
        ($usageAfter - $usageBefore) / 1048576,
        ($peakAfter - $peakBefore) / 1048576,
        ($rssAfter - $rssBefore) / 1048576
    );

    unlink($dumpFile);
}

/**
 * Recurse to $depth, each frame holding twenty locals, and dump from the
 * innermost one so every frame on the chain is walked.
 */
function repro_nest($depth, $count)
{
    $v1 = 1; $v2 = 2; $v3 = 3; $v4 = 4; $v5 = 5;
    $v6 = 'a'; $v7 = 'b'; $v8 = 'c'; $v9 = 'd'; $v10 = 'e';
    $v11 = array(1); $v12 = array(2); $v13 = array(3); $v14 = array(4); $v15 = array(5);
    $v16 = 1.5; $v17 = 2.5; $v18 = 3.5; $v19 = true; $v20 = null;

    if ($depth > 0) {
        return repro_nest($depth - 1, $count);
    }

    repro_measure('frames', $count);

    return $v1;
}

if ($mode === 'objects') {
    $objects = array();
    for ($i = 0; $i < $count; $i++) {
        $objects[] = new ReproDeclared();
    }
    repro_measure('objects', $count);
} elseif ($mode === 'frames') {
    repro_nest($count, $count);
} elseif ($mode === 'limit') {
    // What is left of memory_limit has to be enough: a writer that allocates
    // per object cannot dump a process that is already near its ceiling, which
    // is exactly when a dump is wanted.
    $objects = array();
    for ($i = 0; $i < $count; $i++) {
        $objects[] = new ReproDeclared();
    }

    ini_set('memory_limit', (memory_get_usage(true) + 8 * 1048576) . '');

    $dumpFile = tempnam(sys_get_temp_dir(), 'meminfo-repro');
    $handle = fopen($dumpFile, 'w');
    meminfo_dump($handle);
    fclose($handle);

    printf("limit objects=%d completed dump_bytes=%d\n", $count, filesize($dumpFile));
    unlink($dumpFile);
} else {
    fwrite(STDERR, "usage: memory.php objects|frames|limit [count]\n");
    exit(2);
}
