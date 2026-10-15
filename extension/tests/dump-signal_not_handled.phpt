--TEST--
Check that a target which does not handle the dump signal is refused, not killed
--SKIPIF--
<?php
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (!file_exists('/proc/self/status')) die('skip /proc is required for the handled-signal check');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    // A plain PHP process with no meminfo loaded: nothing is listening for the
    // signal. Refusing to send it is the whole point — the default action of
    // several otherwise plausible signals is to terminate the process.
    $targetCode = <<<'CODE'
$deadline = microtime(true) + 30;
$total = 0;
while (microtime(true) < $deadline) {
    for ($i = 0; $i < 100000; $i++) {
        $total += $i;
    }
}
CODE;

    $child = meminfo_test_spawn($targetCode, array(), false);
    $pid = meminfo_test_child_pid($child);

    echo "target started: ", $pid > 0 ? 'yes' : 'no', "\n";

    if ($pid > 0) {
        $started = microtime(true);
        $result = meminfo_dump_process($pid, array('timeout' => 5));
        $elapsed = microtime(true) - $started;

        echo "result: ", var_export($result, true), "\n";
        echo "refused without waiting: ", $elapsed < 2 ? 'yes' : 'no', "\n";
        echo "target still alive: ", file_exists('/proc/' . $pid . '/status') ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
?>
--EXPECTF--
target started: yes

Warning: meminfo_dump_process(): process %d does not handle signal %d: meminfo is not loaded, the target is PHP 7.0 or a ZTS build, or no request is running. Pass "force" => true to send it anyway in %s on line %d
result: false
refused without waiting: yes
target still alive: yes
