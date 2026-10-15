--TEST--
Check that meminfo_dump_process() dumps another running process
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (!file_exists('/proc/self/status')) die('skip /proc is required to find the target');
    // The target is spawned from this same PHP, so it has to be a build that
    // can serve a dump. Refusing a target that cannot is covered by
    // dump-signal_not_handled.phpt instead.
    if (PHP_VERSION_ID < 70100) die('skip the target side needs PHP 7.1 or later');
    if (PHP_ZTS) die('skip the target side is not available on ZTS builds');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $dumpDir = meminfo_test_make_dump_dir();

    // The target holds a recognisable object and stays busy executing opcodes:
    // the dump is taken between two of them, so a target parked in a blocking
    // call would only react once it returned.
    $targetCode = <<<'CODE'
class TriggerTargetMarker
{
    public $tag = 'trigger-target-marker';
}

$markerObject = new TriggerTargetMarker();
$localInTarget = 'local-in-target';

$deadline = microtime(true) + 30;
$total = 0;
while (microtime(true) < $deadline) {
    for ($i = 0; $i < 100000; $i++) {
        $total += $i;
    }
}
CODE;

    $child = meminfo_test_spawn($targetCode, array(
        'meminfo.dump_dir' => $dumpDir,
    ));
    $pid = meminfo_test_child_pid($child);

    echo "target started: ", $pid > 0 ? 'yes' : 'no', "\n";

    if ($pid > 0) {
        // --- the trigger itself -------------------------------------------

        // No try/catch: on these paths a failure should fail the test loudly
        // rather than be reported as a polite "no".
        $path = meminfo_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 20));

        echo "returned a path: ", is_string($path) ? 'yes' : 'no', "\n";

        if (is_string($path)) {
            printf("mode: %o\n", fileperms($path) & 0777);

            $items = meminfo_test_items($path);

            echo "valid json: ", empty($items) ? 'no' : 'yes', "\n";
            echo "target marker present: ",
                meminfo_test_has_class($items, 'TriggerTargetMarker') ? 'yes' : 'no', "\n";
            echo "target local present: ",
                meminfo_test_frame_of($items, 'localInTarget') === null ? 'no' : 'yes', "\n";

            unlink($path);
        }

        // --- the same thing through the command -----------------------------

        $command = escapeshellarg(meminfo_test_php_binary())
            . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
            . ' ' . escapeshellarg(dirname(dirname(__FILE__)) . '/dump-meminfo')
            . ' ' . (int) $pid
            . ' --dir=' . escapeshellarg($dumpDir)
            . ' --timeout=20';

        $output = array();
        $exitCode = 0;
        exec($command . ' 2>/dev/null', $output, $exitCode);

        echo "dump-meminfo exit code: ", $exitCode, "\n";
        echo "dump-meminfo printed one path: ", count($output) === 1 ? 'yes' : 'no', "\n";

        if (count($output) === 1 && is_file($output[0])) {
            $items = meminfo_test_items($output[0]);

            echo "dump-meminfo dump has the marker: ",
                meminfo_test_has_class($items, 'TriggerTargetMarker') ? 'yes' : 'no', "\n";

            unlink($output[0]);
        } else {
            echo "dump-meminfo dump has the marker: no\n";
        }

        // --- two triggers must not share a file -----------------------------

        $first = meminfo_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 20));
        $second = meminfo_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 20));

        echo "two triggers got separate files: ",
            ($first !== $second) ? 'yes' : 'no', "\n";

        foreach (array($first, $second) as $extra) {
            @unlink($extra);
        }

        echo "target still running: ",
            file_exists('/proc/' . $pid . '/status') ? 'yes' : 'no', "\n";
        echo "target wrote nothing to stdout: ",
            filesize($child['stdout']) === 0 ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
    meminfo_test_remove_dir($dumpDir);
?>
--EXPECT--
target started: yes
returned a path: yes
mode: 600
valid json: yes
target marker present: yes
target local present: yes
dump-meminfo exit code: 0
dump-meminfo printed one path: yes
dump-meminfo dump has the marker: yes
two triggers got separate files: yes
target still running: yes
target wrote nothing to stdout: yes
