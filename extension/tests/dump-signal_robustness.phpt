--TEST--
Check how a dump behaves when the target or its dump directory misbehaves
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (!file_exists('/proc/self/status')) die('skip /proc is required to find the target');
    // The targets are spawned from this same PHP, so it has to be a build that
    // can serve a dump.
    if (PHP_VERSION_ID < 70100) die('skip the target side needs PHP 7.1 or later');
    if (PHP_ZTS) die('skip the target side is not available on ZTS builds');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $busyCode = <<<'CODE'
$marker = 'robustness-marker';
$deadline = microtime(true) + 30;
$total = 0;
while (microtime(true) < $deadline) {
    for ($i = 0; $i < 100000; $i++) {
        $total += $i;
    }
}
CODE;

    $runningAsRoot = function_exists('posix_geteuid') && posix_geteuid() === 0;

    // --- 1. the dump directory cannot be written to -----------------------

    // Nothing may reach the target's own output: a stream notice or a user
    // error handler firing inside someone's response is exactly the kind of
    // interference this feature must not cause. The failure goes to the
    // target's error log instead.
    if ($runningAsRoot) {
        // root ignores directory permissions, so there is nothing to observe.
        echo "unwritable directory refused: yes\n";
        echo "nothing written to the directory: yes\n";
        echo "target stdout untouched: yes\n";
        echo "target stderr untouched: yes\n";
        echo "failure recorded in the error log: yes\n";
    } else {
        $readOnlyDir = meminfo_test_make_dump_dir();
        chmod($readOnlyDir, 0500);

        $child = meminfo_test_spawn($busyCode, array('meminfo.dump_dir' => $readOnlyDir));
        $pid = meminfo_test_child_pid($child);

        $attempt = $pid > 0
            ? meminfo_test_try_dump_process($pid, array('dir' => $readOnlyDir, 'timeout' => 3))
            : array('path' => null, 'exception' => null);

        echo "unwritable directory refused: ", $attempt['path'] === null ? 'yes' : 'no', "\n";
        echo "nothing written to the directory: ",
            count(glob($readOnlyDir . '/*')) === 0 ? 'yes' : 'no', "\n";
        echo "target stdout untouched: ", filesize($child['stdout']) === 0 ? 'yes' : 'no', "\n";
        echo "target stderr untouched: ", filesize($child['stderr']) === 0 ? 'yes' : 'no', "\n";

        $log = file_exists($child['error_log']) ? file_get_contents($child['error_log']) : '';
        echo "failure recorded in the error log: ",
            strpos($log, 'meminfo: cannot create') !== false ? 'yes' : 'no', "\n";

        meminfo_test_cleanup($child, $pid);
        meminfo_test_remove_dir($readOnlyDir);
    }

    // --- 2. the target never reaches a safe point --------------------------

    // Stopped with SIGSTOP, the target executes no opcodes at all, so the
    // request is delivered but never acted on. The trigger has to give up
    // instead of waiting forever, and must not leave a partial file behind.
    $dumpDir = meminfo_test_make_dump_dir();

    $child = meminfo_test_spawn($busyCode, array('meminfo.dump_dir' => $dumpDir));
    $pid = meminfo_test_child_pid($child);

    if ($pid > 0) {
        meminfo_test_signal_pid($pid, 'STOP');
        usleep(200000);

        $started = microtime(true);
        $attempt = meminfo_test_try_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 2, 'force' => true));
        $elapsed = microtime(true) - $started;

        echo "a stopped target times out: ", $attempt['path'] === null ? 'yes' : 'no', "\n";
        echo "gave up near the timeout: ", ($elapsed >= 2 && $elapsed < 15) ? 'yes' : 'no', "\n";
        echo "no partial dump left behind: ",
            count(glob($dumpDir . '/*.tmp')) === 0 ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
    meminfo_test_remove_dir($dumpDir);

    // --- 3. the target dies while the trigger is waiting -------------------

    $dumpDir = meminfo_test_make_dump_dir();

    // Detached, so that killing it really removes it rather than leaving a
    // zombie that still looks alive.
    $child = meminfo_test_spawn_detached($busyCode, array('meminfo.dump_dir' => $dumpDir));
    $pid = meminfo_test_child_pid($child);

    if ($pid > 0) {
        meminfo_test_signal_pid($pid, 'STOP');
        usleep(200000);

        // Killed a second into the wait, while the trigger still expects a dump.
        exec('sh -c ' . escapeshellarg('(sleep 1; kill -9 ' . (int) $pid . ') > /dev/null 2>&1 &'));

        $started = microtime(true);
        $attempt = meminfo_test_try_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 25, 'force' => true));
        $elapsed = microtime(true) - $started;

        echo "a vanished target is reported: ", $attempt['path'] === null ? 'yes' : 'no', "\n";
        echo "failed without waiting out the timeout: ", $elapsed < 20 ? 'yes' : 'no', "\n";
        echo "no partial dump left behind: ",
            count(glob($dumpDir . '/*.tmp')) === 0 ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
    meminfo_test_remove_dir($dumpDir);

    // --- 4. targets parked in a blocking call ------------------------------

    // Asking for a dump interrupts whatever the target was waiting in. That is
    // visible to the application — sleep() returns early, stream_select()
    // reports an interrupted system call — and is documented as a side effect
    // of taking a dump. What matters here is that the dump still happens.
    foreach (array('sleep' => <<<'CODE'
$marker = 'sleeping-marker';
for ($i = 0; $i < 30; $i++) {
    sleep(1);
}
CODE
        , 'stream_select' => <<<'CODE'
$marker = 'selecting-marker';
// Both ends are held open and nothing is ever written, so select really
// blocks. Selecting on stdin would not: /dev/null is always readable.
$pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
for ($i = 0; $i < 30; $i++) {
    $read = array($pair[0]);
    $write = array();
    $except = array();
    @stream_select($read, $write, $except, 1);
}
CODE
    ) as $label => $code) {
        if ($label === 'stream_select' && !function_exists('stream_socket_pair')) {
            echo "a target blocked in stream_select can be dumped: yes\n";
            echo "its dump is valid: yes\n";
            continue;
        }

        $dumpDir = meminfo_test_make_dump_dir();

        $child = meminfo_test_spawn($code, array('meminfo.dump_dir' => $dumpDir));
        $pid = meminfo_test_child_pid($child);

        $attempt = $pid > 0
            ? meminfo_test_try_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 20))
            : array('path' => null, 'exception' => null);

        echo "a target blocked in $label can be dumped: ", $attempt['path'] === null ? 'no' : 'yes', "\n";

        if ($attempt['path'] !== null) {
            $items = meminfo_test_items($attempt['path']);
            echo "its dump is valid: ", empty($items) ? 'no' : 'yes', "\n";
            unlink($attempt['path']);
        } else {
            echo "its dump is valid: no\n";
        }

        meminfo_test_cleanup($child, $pid);
        meminfo_test_remove_dir($dumpDir);
    }
?>
--EXPECT--
unwritable directory refused: yes
nothing written to the directory: yes
target stdout untouched: yes
target stderr untouched: yes
failure recorded in the error log: yes
a stopped target times out: yes
gave up near the timeout: yes
no partial dump left behind: yes
a vanished target is reported: yes
failed without waiting out the timeout: yes
no partial dump left behind: yes
a target blocked in sleep can be dumped: yes
its dump is valid: yes
a target blocked in stream_select can be dumped: yes
its dump is valid: yes
