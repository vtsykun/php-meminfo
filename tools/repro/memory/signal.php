<?php
/**
 * A dump taken from a signal rather than from a call.
 *
 * Checks the properties that only apply to that path: the dump lands in a file
 * of its own with restrictive permissions, nothing reaches the process's own
 * output, and the CPU the dump costs is not charged to max_execution_time.
 *
 * Usage:
 *   php -d extension=.../meminfo.so -d meminfo.dump_dir=DIR signal.php [objects]
 */

$count = isset($argv[1]) ? (int) $argv[1] : 200000;

class ReproWide
{
    public $p0 = 0; public $p1 = 1; public $p2 = 2; public $p3 = 3; public $p4 = 4;
    public $p5 = 5; public $p6 = 6; public $p7 = 7; public $p8 = 8; public $p9 = 9;
}

$dir = ini_get('meminfo.dump_dir');
if ($dir === false || $dir === '') {
    $dir = sys_get_temp_dir();
}

$signal = 23;   // SIGURG
$pattern = $dir . '/meminfo-' . getmypid() . '-*.json';

function repro_raise($signal)
{
    if (function_exists('posix_kill')) {
        return posix_kill(getmypid(), $signal);
    }

    $process = proc_open('kill -' . (int) $signal . ' ' . getmypid(), array(), $pipes);
    if (is_resource($process)) {
        proc_close($process);

        return true;
    }

    return false;
}

function repro_wait($pattern, $seconds = 30.0)
{
    $deadline = microtime(true) + $seconds;

    while (microtime(true) < $deadline) {
        $files = glob($pattern);
        if (!empty($files)) {
            return $files[0];
        }

        // Spin: the dump is taken between opcodes, so the process has to keep
        // executing them.
        $total = 0;
        for ($i = 0; $i < 200000; $i++) {
            $total += $i;
        }
    }

    return null;
}

$objects = array();
for ($i = 0; $i < $count; $i++) {
    $objects[] = new ReproWide();
}

// One second of CPU is far less than this dump costs, so the hard timeout
// would kill the process if the dump's CPU were charged to the request.
set_time_limit(1);

$started = microtime(true);
repro_raise($signal);
$dumpFile = repro_wait($pattern);
$elapsed = microtime(true) - $started;

if ($dumpFile === null) {
    echo "no dump appeared\n";
    exit(1);
}

printf(
    "signal dump objects=%d seconds=%.2f dump_bytes=%d mode=%o survived=yes\n",
    $count,
    $elapsed,
    filesize($dumpFile),
    fileperms($dumpFile) & 0777
);

echo "dump kept at: ", $dumpFile, "\n";
