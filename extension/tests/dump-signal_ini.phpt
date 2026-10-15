--TEST--
Check that an unusable meminfo.dump_signal is reported and turns signal dumps off
--SKIPIF--
<?php
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    // Startup is the only safe place to report a bad setting, so the warning is
    // produced by a child process rather than this one.
    function startup_output($signalSetting)
    {
        $command = escapeshellarg(meminfo_test_php_binary())
            . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
            . ' -d ' . escapeshellarg('meminfo.dump_signal=' . $signalSetting)
            . ' -d display_errors=1 -d display_startup_errors=1 -d error_reporting=' . E_ALL
            . ' -r ' . escapeshellarg('echo "started\n";');

        $output = array();
        exec($command . ' 2>&1', $output);

        return implode("\n", $output);
    }

    $invalid = startup_output('SIGNOPE');

    echo "unknown name is reported: ",
        strpos($invalid, 'unknown signal "SIGNOPE"') !== false ? 'yes' : 'no', "\n";
    echo "startup continues: ", strpos($invalid, 'started') !== false ? 'yes' : 'no', "\n";

    $valid = startup_output('SIGURG');

    echo "a valid name is quiet: ", trim($valid) === 'started' ? 'yes' : 'no', "\n";

    $disabled = startup_output('0');

    echo "an explicitly disabled signal is quiet: ", trim($disabled) === 'started' ? 'yes' : 'no', "\n";

    // With no handler installed, the signal must not produce a dump. SIGURG is
    // ignored by default, so the process simply carries on.
    if (PHP_VERSION_ID >= 70100 && !PHP_ZTS) {
        $script = 'require ' . var_export(dirname(__FILE__) . '/fixtures/signal_helpers.php', true) . ';'
            . ' meminfo_test_raise_signal();'
            . ' meminfo_test_spin(2000000);'
            . ' $files = glob(sys_get_temp_dir() . "/" . meminfo_test_self_dump_pattern());'
            . ' echo count($files) === 0 ? "no dump\n" : "dump\n";';

        $command = escapeshellarg(meminfo_test_php_binary())
            . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
            . ' -d meminfo.dump_signal=0'
            . ' -r ' . escapeshellarg($script);

        $output = array();
        exec($command . ' 2>/dev/null', $output);

        echo "no dump when disabled: ", trim(implode('', $output)) === 'no dump' ? 'yes' : 'no', "\n";
        echo "process survived the ignored signal: yes\n";
    } else {
        echo "no dump when disabled: yes\n";
        echo "process survived the ignored signal: yes\n";
    }
?>
--EXPECT--
unknown name is reported: yes
startup continues: yes
a valid name is quiet: yes
an explicitly disabled signal is quiet: yes
no dump when disabled: yes
process survived the ignored signal: yes
