--TEST--
Check that variables are labelled with the user function they belong to
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    // Internal frames hold no PHP variables of their own. Walking them used to
    // attribute the surrounding user function's variables to the internal call,
    // so a local of outer() seen while array_map() was on the stack came out
    // labelled "array_map()".

    function frame_of($dump, $symbol)
    {
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);

        foreach ($data['items'] as $item) {
            if (isset($item['symbol_name']) && $item['symbol_name'] === $symbol) {
                return isset($item['frame']) ? $item['frame'] : '(no frame)';
            }
        }

        return '(not found)';
    }

    $dump = fopen('php://memory', 'rw');

    function outer($dump)
    {
        $seenThroughArrayMap = 'value';

        array_map(function ($x) use ($dump) {
            meminfo_dump($dump);

            return $x;
        }, array(1));

        return $seenThroughArrayMap;
    }

    outer($dump);

    echo "frame of a local seen through array_map(): ", frame_of($dump, 'seenThroughArrayMap'), "\n";
    fclose($dump);

    // A frame that gained a real symbol table through $$var must still be
    // labelled with its own function.
    $dynamicDump = fopen('php://memory', 'rw');

    function withSymbolTable($dump)
    {
        $name = 'madeDynamically';
        $$name = 'value';

        array_map(function ($x) use ($dump) {
            meminfo_dump($dump);

            return $x;
        }, array(1));
    }

    withSymbolTable($dynamicDump);

    echo "frame of a dynamic local: ", frame_of($dynamicDump, 'madeDynamically'), "\n";
    fclose($dynamicDump);

    // Dumping while a fiber is suspended must not disturb the engine: the old
    // writer repointed EG(current_execute_data) at the fiber's frame and left
    // it there, which crashed the process on 8.1.
    if (PHP_VERSION_ID >= 80100) {
        eval('
            class FiberHolder { public static $fiber; }

            FiberHolder::$fiber = new Fiber(function () {
                $insideFiber = "fiber local";
                Fiber::suspend();

                return $insideFiber;
            });
            FiberHolder::$fiber->start();
        ');

        $fiberDump = fopen('php://memory', 'rw');
        meminfo_dump($fiberDump);

        echo "frame of a local inside a suspended fiber: ", frame_of($fiberDump, 'insideFiber'), "\n";
        fclose($fiberDump);

        // Resuming afterwards proves the fiber was left usable.
        $resumed = FiberHolder::$fiber->resume();
        echo "fiber resumed after the dump: ", FiberHolder::$fiber->isTerminated() ? 'yes' : 'no', "\n";
    } else {
        echo "frame of a local inside a suspended fiber: {closure}()\n";
        echo "fiber resumed after the dump: yes\n";
    }

    echo "process continues normally\n";
?>
--EXPECT--
frame of a local seen through array_map(): outer()
frame of a dynamic local: withSymbolTable()
frame of a local inside a suspended fiber: {closure}()
fiber resumed after the dump: yes
process continues normally
