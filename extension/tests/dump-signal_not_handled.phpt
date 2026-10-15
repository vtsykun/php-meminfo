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
        $thrown = null;

        try {
            meminfo_dump_process($pid, array('timeout' => 5));
        } catch (Exception $exception) {
            $thrown = $exception;
        }

        $elapsed = microtime(true) - $started;

        echo "exception class: ", $thrown === null ? '(none thrown)' : get_class($thrown), "\n";
        echo "message names the signal: ",
            ($thrown !== null && strpos($thrown->getMessage(), 'does not handle signal') !== false)
                ? 'yes' : 'no', "\n";
        echo "message names the pid: ",
            ($thrown !== null && strpos($thrown->getMessage(), (string) $pid) !== false)
                ? 'yes' : 'no', "\n";
        echo "refused without waiting: ", $elapsed < 2 ? 'yes' : 'no', "\n";
        echo "target still alive: ", file_exists('/proc/' . $pid . '/status') ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
?>
--EXPECT--
target started: yes
exception class: RuntimeException
message names the signal: yes
message names the pid: yes
refused without waiting: yes
target still alive: yes
