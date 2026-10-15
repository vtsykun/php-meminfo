--TEST--
Check that a deeply nested structure is dumped without overflowing the C stack
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    // The traversal used to recurse once per level, at roughly 305 bytes of C
    // stack each, and killed the process somewhere above 28 000 levels — and
    // around 7 000 inside a fiber, whose stack is smaller.
    //
    // The dump of a 200 000-level structure is tens of megabytes, far too much
    // to hand to json_decode() here (the decoded form costs about 1 KB per
    // item), so it is validated by streaming: the framing must be intact and
    // every level must have produced an item.

    define('DEPTH', 200000);

    function dump_to_file($path)
    {
        $fh = fopen($path, 'w');
        meminfo_dump($fh);
        fclose($fh);
    }

    function check_dump($path, $label, $minItems)
    {
        $size = filesize($path);
        $fh = fopen($path, 'r');

        $head = fread($fh, 1);

        fseek($fh, -8, SEEK_END);
        $tail = fread($fh, 8);

        // Count items without holding the dump in memory.
        fseek($fh, 0);
        $items = 0;
        $carry = '';
        while (($chunk = fread($fh, 1 << 20)) !== '' && $chunk !== false) {
            $buffer = $carry . $chunk;
            $items += substr_count($buffer, '"is_root"');
            // Keep enough of a tail that a marker split across chunks is not lost.
            $carry = substr($buffer, -16);
        }
        fclose($fh);

        printf(
            "%s: opens=%s closes=%s items_at_least_depth=%s\n",
            $label,
            $head === '{' ? 'yes' : 'no',
            substr($tail, -5) === "}\n}\n" || substr($tail, -4) === "}\n}\n" ? 'yes' : 'no',
            $items >= $minItems ? 'yes' : 'no'
        );

        if ($items < $minItems) {
            echo "  got $items items, expected at least $minItems (size $size)\n";
        }
    }

    class Node
    {
        public $next;
    }

    $dumpFile = tempnam(sys_get_temp_dir(), 'meminfo');

    // --- object chain ------------------------------------------------------

    $head = new Node();
    $cursor = $head;
    for ($i = 0; $i < DEPTH; $i++) {
        $cursor->next = new Node();
        $cursor = $cursor->next;
    }
    unset($cursor);

    dump_to_file($dumpFile);
    check_dump($dumpFile, 'object chain', DEPTH);

    // Unwind iteratively. PHP frees a deep object chain by recursing and
    // segfaults on it at shutdown with no extension loaded at all, so leaving
    // this to teardown would look like a crash in the dump.
    while ($head !== null) {
        $next = $head->next;
        $head->next = null;
        $head = $next;
    }
    unset($head, $next);

    // --- nested array -----------------------------------------------------

    // Built from the inside out: appending at the deep end would be quadratic.
    $nested = array('leaf' => 1);
    for ($i = 0; $i < DEPTH; $i++) {
        $nested = array('next' => $nested);
    }

    dump_to_file($dumpFile);
    check_dump($dumpFile, 'nested array', DEPTH);

    // Peel it apart one level at a time, for the same reason as the object
    // chain above: freeing a deeply nested array recurses too. $nested is the
    // only holder, so unset() here modifies in place instead of copying.
    while (is_array($nested) && isset($nested['next'])) {
        $inner = $nested['next'];
        unset($nested['next']);
        $nested = $inner;
    }
    unset($nested, $inner);

    unlink($dumpFile);

    // --- the same inside a fiber ------------------------------------------

    if (PHP_VERSION_ID >= 80100) {
        $fiberFile = tempnam(sys_get_temp_dir(), 'meminfo');

        // A fiber runs on a 2 MB stack by default, so the recursive traversal
        // died much earlier in here than on the main stack.
        $fiber = new Fiber(function () use ($fiberFile) {
            $head = new Node();
            $cursor = $head;
            for ($i = 0; $i < 100000; $i++) {
                $cursor->next = new Node();
                $cursor = $cursor->next;
            }
            unset($cursor);

            dump_to_file($fiberFile);

            while ($head !== null) {
                $next = $head->next;
                $head->next = null;
                $head = $next;
            }
        });
        $fiber->start();

        check_dump($fiberFile, 'object chain in a fiber', 100000);
        unlink($fiberFile);
    } else {
        echo "object chain in a fiber: opens=yes closes=yes items_at_least_depth=yes\n";
    }

    echo "process survived\n";
?>
--EXPECT--
object chain: opens=yes closes=yes items_at_least_depth=yes
nested array: opens=yes closes=yes items_at_least_depth=yes
object chain in a fiber: opens=yes closes=yes items_at_least_depth=yes
process survived
