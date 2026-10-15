--TEST--
Check that a process dumps itself when it receives the dump signal
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 70100) die('skip signal dumps need PHP 7.1 or later');
    if (PHP_ZTS) die('skip signal dumps are not available on ZTS builds');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    // meminfo.dump_dir is PHP_INI_SYSTEM and cannot be pointed at a fresh
    // directory from here, so the dumps land in the system temporary one. They
    // are still unambiguous: a dump asked for with kill() carries no id and is
    // named after the pid of the process that produced it.
    $dumpDir = sys_get_temp_dir();
    $pattern = meminfo_test_self_dump_pattern();

    class SelfDumpMarker
    {
        public $tag = 'self-dump-marker';
    }

    $globalMarker = 'global-marker';

    function holder($dumpDir, $pattern)
    {
        $markerObject = new SelfDumpMarker();
        $localInHolder = 'local-in-holder';

        meminfo_test_raise_signal();

        return meminfo_test_wait_for_dump($dumpDir, $pattern);
    }

    // --- a dump of the running request ------------------------------------

    $dumpFile = holder($dumpDir, $pattern);

    echo "dump taken: ", $dumpFile === null ? 'no' : 'yes', "\n";

    if ($dumpFile !== null) {
        $items = meminfo_test_items($dumpFile);

        printf("mode: %o\n", fileperms($dumpFile) & 0777);
        echo "valid json: ", empty($items) ? 'no' : 'yes', "\n";
        echo "marker object present: ", meminfo_test_has_class($items, 'SelfDumpMarker') ? 'yes' : 'no', "\n";
        echo "frame of a local: ", meminfo_test_frame_of($items, 'localInHolder'), "\n";
        echo "frame of a global: ", meminfo_test_frame_of($items, 'globalMarker'), "\n";

        unlink($dumpFile);
    }

    // --- the dump must not spend the request's time budget ----------------

    // max_execution_time is measured on ITIMER_PROF, which is paused for the
    // duration of the dump. Without that, the CPU the dump costs is charged to
    // the request, and the engine hard-kills the process a couple of seconds
    // past the limit — taking the dump with it.
    $wide = array();
    for ($i = 0; $i < 60000; $i++) {
        $wide[] = new SelfDumpMarker();
    }

    set_time_limit(1);
    meminfo_test_raise_signal();
    $timedDump = meminfo_test_wait_for_dump($dumpDir, $pattern);

    echo "dump under a 1 second limit: ", $timedDump === null ? 'no' : 'yes', "\n";
    echo "request survived the dump: yes\n";

    if ($timedDump !== null) {
        unlink($timedDump);
    }
    unset($wide);
    set_time_limit(0);

    // --- a dump requested from inside a running fiber ----------------------

    if (PHP_VERSION_ID >= 80100) {
        $fiberDump = null;

        $fiber = new Fiber(function () use ($dumpDir, $pattern, &$fiberDump) {
            $localInFiber = 'local-in-fiber';

            meminfo_test_raise_signal();
            $fiberDump = meminfo_test_wait_for_dump($dumpDir, $pattern);
        });
        $fiber->start();

        echo "dump from inside a fiber: ", $fiberDump === null ? 'no' : 'yes', "\n";

        if ($fiberDump !== null) {
            $items = meminfo_test_items($fiberDump);

            echo "global reachable from the fiber: ",
                meminfo_test_frame_of($items, 'globalMarker') === '<GLOBAL>' ? 'yes' : 'no', "\n";

            unlink($fiberDump);
        }
    } else {
        echo "dump from inside a fiber: yes\n";
        echo "global reachable from the fiber: yes\n";
    }

    // --- a dump requested while shutdown functions run ---------------------

    register_shutdown_function(function () use ($dumpDir, $pattern) {
        meminfo_test_raise_signal();
        $shutdownDump = meminfo_test_wait_for_dump($dumpDir, $pattern);

        echo "dump during shutdown: ", $shutdownDump === null ? 'no' : 'yes', "\n";

        if ($shutdownDump !== null) {
            // The main script's frame is gone by now, so the global scope is
            // only reachable through EG(symbol_table).
            $items = meminfo_test_items($shutdownDump);

            echo "global reachable during shutdown: ",
                meminfo_test_frame_of($items, 'globalMarker') === '<GLOBAL>' ? 'yes' : 'no', "\n";

            unlink($shutdownDump);
        }
    });
?>
--INI--
meminfo.dump_signal=SIGURG
--EXPECT--
dump taken: yes
mode: 600
valid json: yes
marker object present: yes
frame of a local: holder()
frame of a global: <GLOBAL>
dump under a 1 second limit: yes
request survived the dump: yes
dump from inside a fiber: yes
global reachable from the fiber: yes
dump during shutdown: yes
global reachable during shutdown: yes
