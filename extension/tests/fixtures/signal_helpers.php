<?php
/**
 * Helpers shared by the signal-triggered dump tests.
 *
 * PHP 7.0-compatible syntax: these run on every version in the matrix, even
 * though the signal tests themselves are skipped below 7.1.
 */

/**
 * Path of the meminfo extension to load into a child process.
 *
 * run-tests.php starts children with -n, so the extension has to be named
 * explicitly.
 *
 * The freshly built module takes precedence over an installed copy: when the
 * suite runs from a build tree, that build is what is under test, and silently
 * loading an older installed extension into the child makes these tests report
 * on something else entirely.
 *
 * @return string
 */
function meminfo_test_extension_path()
{
    // This file lives in extension/tests/fixtures/, so the build output is
    // three levels up.
    $built = dirname(dirname(dirname(__FILE__))) . '/modules/meminfo.so';

    if (file_exists($built)) {
        return $built;
    }

    return ini_get('extension_dir') . '/meminfo.so';
}

/**
 * @return string
 */
function meminfo_test_php_binary()
{
    $binary = getenv('TEST_PHP_EXECUTABLE');

    if ($binary !== false && $binary !== '' && file_exists($binary)) {
        return $binary;
    }

    return PHP_BINARY;
}

/**
 * A private directory for dump files, so concurrent tests cannot see each
 * other's dumps.
 *
 * @return string
 */
function meminfo_test_make_dump_dir()
{
    $dir = sys_get_temp_dir() . '/meminfo-test-' . getmypid() . '-' . mt_rand(100000, 999999);

    mkdir($dir, 0700, true);

    return $dir;
}

/**
 * @param string $dir
 */
function meminfo_test_remove_dir($dir)
{
    if (!is_dir($dir)) {
        return;
    }

    foreach (glob($dir . '/*') as $entry) {
        if (is_dir($entry) && !is_link($entry)) {
            meminfo_test_remove_dir($entry);
        } else {
            @unlink($entry);
        }
    }

    @chmod($dir, 0700);
    @rmdir($dir);
}

/**
 * Send a signal to this very process.
 *
 * ext-posix may be built shared and therefore absent under -n, so `kill` is
 * used as a fallback.
 *
 * @param int $signal
 *
 * @return bool
 */
function meminfo_test_raise_signal($signal = 23)
{
    if (function_exists('posix_kill')) {
        return posix_kill(getmypid(), $signal);
    }

    $process = proc_open('kill -' . (int) $signal . ' ' . getmypid(), array(), $pipes);

    if (!is_resource($process)) {
        return false;
    }

    proc_close($process);

    return true;
}

/**
 * Burn a little CPU so the engine reaches a safe point between opcodes, which
 * is where a requested dump is actually taken.
 *
 * @param int $iterations
 *
 * @return int
 */
function meminfo_test_spin($iterations = 200000)
{
    $total = 0;

    for ($i = 0; $i < $iterations; $i++) {
        $total += $i;
    }

    return $total;
}

/**
 * Wait for a dump matching $pattern to appear in $dir.
 *
 * Spins instead of sleeping: the dump is taken between opcodes, so the process
 * has to keep executing them.
 *
 * @param string $dir
 * @param string $pattern glob pattern, relative to $dir
 * @param float  $timeoutSeconds
 *
 * @return string|null
 */
function meminfo_test_wait_for_dump($dir, $pattern = 'meminfo-*.json', $timeoutSeconds = 10.0)
{
    $deadline = microtime(true) + $timeoutSeconds;

    while (microtime(true) < $deadline) {
        $files = glob($dir . '/' . $pattern);

        if (!empty($files)) {
            return $files[0];
        }

        meminfo_test_spin();
    }

    return null;
}

/**
 * Glob matching only the dumps this process produced for itself.
 *
 * A dump asked for with plain kill() carries no id, so the target names it
 * after its own pid — which is what makes it distinguishable from dumps other
 * tests are writing into the same shared temporary directory.
 *
 * @return string
 */
function meminfo_test_self_dump_pattern()
{
    return 'meminfo-' . getmypid() . '-*.json';
}

/**
 * Start a child PHP process with meminfo loaded, running $code.
 *
 * The child writes its pid into a file so the caller can signal it; the caller
 * must wait for that file before using the pid.
 *
 * @param string $code          PHP source, without the opening tag
 * @param array  $iniOptions    name => value, passed with -d
 * @param bool   $withExtension load meminfo into the child
 *
 * @return array
 */
function meminfo_test_spawn($code, array $iniOptions = array(), $withExtension = true)
{
    $prefix = sys_get_temp_dir() . '/meminfo-child-' . getmypid() . '-' . mt_rand(100000, 999999);

    $scriptFile = $prefix . '.php';
    $pidFile    = $prefix . '.pid';
    $stdoutFile = $prefix . '.out';
    $stderrFile = $prefix . '.err';
    $errorLog   = $prefix . '.log';

    file_put_contents(
        $scriptFile,
        "<?php\nfile_put_contents(" . var_export($pidFile, true) . ", getmypid());\n" . $code
    );

    $command = escapeshellarg(meminfo_test_php_binary()) . ' -n';

    if ($withExtension) {
        $command .= ' -d ' . escapeshellarg('extension=' . meminfo_test_extension_path());
    }

    $iniOptions['error_log'] = $errorLog;

    foreach ($iniOptions as $name => $value) {
        $command .= ' -d ' . escapeshellarg($name . '=' . $value);
    }

    $command .= ' ' . escapeshellarg($scriptFile);

    $descriptors = array(
        0 => array('file', '/dev/null', 'r'),
        1 => array('file', $stdoutFile, 'w'),
        2 => array('file', $stderrFile, 'w'),
    );

    $process = proc_open($command, $descriptors, $pipes);

    return array(
        'process'    => $process,
        'pid_file'   => $pidFile,
        'script'     => $scriptFile,
        'stdout'     => $stdoutFile,
        'stderr'     => $stderrFile,
        'error_log'  => $errorLog,
    );
}

/**
 * Start a child that is not a child of this process.
 *
 * A process started with proc_open() stays a zombie after it dies until it is
 * reaped, and a zombie still answers kill(pid, 0) and still has a /proc entry.
 * Going through a shell that exits immediately leaves the target parented to
 * init, so its death is observable the way a real worker's would be.
 *
 * @param string $code
 * @param array  $iniOptions
 *
 * @return array
 */
function meminfo_test_spawn_detached($code, array $iniOptions = array())
{
    $prefix = sys_get_temp_dir() . '/meminfo-detached-' . getmypid() . '-' . mt_rand(100000, 999999);

    $scriptFile = $prefix . '.php';
    $pidFile    = $prefix . '.pid';
    $stdoutFile = $prefix . '.out';
    $stderrFile = $prefix . '.err';
    $errorLog   = $prefix . '.log';

    file_put_contents(
        $scriptFile,
        "<?php\nfile_put_contents(" . var_export($pidFile, true) . ", getmypid());\n" . $code
    );

    $command = escapeshellarg(meminfo_test_php_binary())
        . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path());

    $iniOptions['error_log'] = $errorLog;

    foreach ($iniOptions as $name => $value) {
        $command .= ' -d ' . escapeshellarg($name . '=' . $value);
    }

    $command .= ' ' . escapeshellarg($scriptFile);
    $command .= ' > ' . escapeshellarg($stdoutFile) . ' 2> ' . escapeshellarg($stderrFile) . ' < /dev/null &';

    exec('sh -c ' . escapeshellarg($command));

    return array(
        'process'   => null,
        'pid_file'  => $pidFile,
        'script'    => $scriptFile,
        'stdout'    => $stdoutFile,
        'stderr'    => $stderrFile,
        'error_log' => $errorLog,
    );
}

/**
 * Send a signal to another process, without depending on ext-posix.
 *
 * @param int $pid
 * @param int|string $signal number, or a name `kill` understands
 */
function meminfo_test_signal_pid($pid, $signal)
{
    if (is_int($signal) && function_exists('posix_kill')) {
        @posix_kill($pid, $signal);

        return;
    }

    // Constrained rather than escaped: this value ends up in a shell command,
    // and the helper only ever needs these few signals.
    $allowed = array('STOP' => 'STOP', 'CONT' => 'CONT', 'KILL' => 'KILL', 'URG' => 'URG', 9 => '9');

    if (!isset($allowed[$signal])) {
        return;
    }

    exec('kill -' . $allowed[$signal] . ' ' . (int) $pid . ' 2>/dev/null');
}

/**
 * @param int $pid
 *
 * @return bool
 */
function meminfo_test_process_exists($pid)
{
    return $pid > 0 && file_exists('/proc/' . (int) $pid . '/status');
}

/**
 * Wait for a spawned child to report its pid.
 *
 * @param array $child
 * @param float $timeoutSeconds
 *
 * @return int 0 when the child never reported one
 */
function meminfo_test_child_pid(array $child, $timeoutSeconds = 10.0)
{
    $deadline = microtime(true) + $timeoutSeconds;

    while (microtime(true) < $deadline) {
        if (file_exists($child['pid_file'])) {
            $pid = (int) file_get_contents($child['pid_file']);

            if ($pid > 0) {
                // Give the child a moment to get past startup and into the
                // request, where the signal handler is installed.
                usleep(300000);

                return $pid;
            }
        }

        usleep(50000);
    }

    return 0;
}

/**
 * @param array $child
 * @param int   $pid
 */
function meminfo_test_cleanup(array $child, $pid = 0)
{
    if ($pid > 0) {
        // CONT first: a stopped process would never act on the KILL queued
        // behind it being stopped in some kernels' reporting.
        meminfo_test_signal_pid($pid, 'CONT');
        meminfo_test_signal_pid($pid, 9);
    }

    if (isset($child['process']) && is_resource($child['process'])) {
        proc_close($child['process']);
    }

    foreach (array('pid_file', 'script', 'stdout', 'stderr', 'error_log') as $key) {
        @unlink($child[$key]);
    }
}

/**
 * Call meminfo_dump_process() and report what happened either way.
 *
 * Every failure throws, so a test that wants to assert about a refusal needs
 * the exception in hand rather than having it unwind the test. Note that `@`
 * is no help here: it suppresses diagnostics, not exceptions.
 *
 * @param int   $pid
 * @param array $options
 *
 * @return array with 'path' set on success and 'exception' set on failure
 */
function meminfo_test_try_dump_process($pid, array $options = array())
{
    try {
        return array('path' => meminfo_dump_process($pid, $options), 'exception' => null);
    } catch (Exception $exception) {
        return array('path' => null, 'exception' => $exception);
    }
}

/**
 * @param string $dumpFile
 *
 * @return array decoded items, or an empty array
 */
function meminfo_test_items($dumpFile)
{
    $data = json_decode(file_get_contents($dumpFile), true);

    return isset($data['items']) ? $data['items'] : array();
}

/**
 * @param array  $items
 * @param string $class
 *
 * @return bool
 */
function meminfo_test_has_class(array $items, $class)
{
    foreach ($items as $item) {
        if (isset($item['class']) && $item['class'] === $class) {
            return true;
        }
    }

    return false;
}

/**
 * @param array  $items
 * @param string $symbol
 *
 * @return string|null the frame the symbol was found in
 */
function meminfo_test_frame_of(array $items, $symbol)
{
    foreach ($items as $item) {
        if (isset($item['symbol_name']) && $item['symbol_name'] === $symbol) {
            return isset($item['frame']) ? $item['frame'] : '(no frame)';
        }
    }

    return null;
}
