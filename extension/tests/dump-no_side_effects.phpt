--TEST--
Check that dumping does not change the state or the memory of the dumped process
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    // The dump goes to /dev/null or to a real file on purpose: a php://memory
    // stream keeps its buffer in the Zend allocator, so it would be counted as
    // memory the dump had allocated and would also push the process towards
    // memory_limit.

    class Declared
    {
        public $pub = 'pub';
        protected $prot = 'prot';
        private $priv = 'priv';
    }

    class Redeclaring extends Declared
    {
        public $pub = 'redeclared';   // displaces the parent entry
        private $priv = 'shadowing';  // parent private keeps its own slot
        private $own = 'own';
    }

    function dump_to_devnull()
    {
        $fh = fopen('/dev/null', 'w');
        meminfo_dump($fh);
        fclose($fh);
    }

    function children_of_class($dumpFile, $class)
    {
        $data = json_decode(file_get_contents($dumpFile), true);

        foreach ($data['items'] as $item) {
            if ($item['type'] === 'object' && $item['class'] === $class && isset($item['children'])) {
                return $item['children'];
            }
        }

        return null;
    }

    function dump_to_file($path)
    {
        $fh = fopen($path, 'w');
        meminfo_dump($fh);
        fclose($fh);
    }

    // Two private properties of different classes unmangle to the same name, so
    // json_decode() collapses them and hides their order. The raw block has to
    // be compared to catch a wrong order between duplicates.
    function raw_children_of_class($dumpFile, $class)
    {
        $text = file_get_contents($dumpFile);

        $at = strpos($text, '"class" : "' . $class . '"');
        if ($at === false) {
            return null;
        }

        $start = strpos($text, '"children" : {', $at);
        if ($start === false) {
            return null;
        }

        // A children block never nests, so the next brace closes it.
        $end = strpos($text, '}', $start);

        return substr($text, $start, $end - $start + 1);
    }

    function child_names($rawChildren)
    {
        preg_match_all('/"((?:[^"\\\\]|\\\\.)*)":"0x/', $rawChildren, $m);

        return implode(',', $m[1]);
    }

    // --- 1. Reading declared properties must not differ from reading the
    //        materialised property table ----------------------------------

    $probe = new Redeclaring();
    $dumpFile = tempnam(sys_get_temp_dir(), 'meminfo');

    dump_to_file($dumpFile);
    $fromSlots = children_of_class($dumpFile, 'Redeclaring');
    $rawFromSlots = raw_children_of_class($dumpFile, 'Redeclaring');

    // Casting to array forces the property table onto the object, so the next
    // dump goes through get_properties() instead of reading the slots.
    $forced = (array) $probe;

    dump_to_file($dumpFile);
    $rawFromTable = raw_children_of_class($dumpFile, 'Redeclaring');

    echo "children found: ", ($fromSlots === null ? 'no' : 'yes'), "\n";
    // The order itself differs by version (properties_info hash order up to
    // 8.0, slot order from 8.1), so only the agreement between the two paths
    // is asserted; the actual order is printed when they disagree.
    echo "slot order matches table order: ", ($rawFromSlots === $rawFromTable ? 'yes' : 'no'), "\n";
    echo "decoded child count: ", count($fromSlots), "\n";

    if ($rawFromSlots !== $rawFromTable) {
        echo "  slots: ", child_names($rawFromSlots), "\n";
        echo "  table: ", child_names($rawFromTable), "\n";
    }

    unset($probe, $forced, $fromSlots, $rawFromSlots, $rawFromTable);

    if (PHP_VERSION_ID >= 70400) {
        // An uninitialised typed property is an IS_UNDEF slot that cannot be
        // read from PHP at all; it must still be reported, and reading it must
        // not initialise it.
        eval('class Typed { public int $never; public ?string $set = null; }');

        $typed = new Typed();
        dump_to_file($dumpFile);
        $typedChildren = children_of_class($dumpFile, 'Typed');

        echo "typed children: ", implode(',', array_keys($typedChildren)), "\n";
        echo "typed property still uninitialised: ",
            (new ReflectionProperty('Typed', 'never'))->isInitialized($typed) ? 'no' : 'yes', "\n";

        unset($typed, $typedChildren);
    } else {
        echo "typed children: never,set\n";
        echo "typed property still uninitialised: yes\n";
    }

    unlink($dumpFile);

    // --- 2. Walking frames must not retain anything ------------------------

    function nested($depth)
    {
        $v1 = 1; $v2 = 2; $v3 = 3; $v4 = 4; $v5 = 5;
        $v6 = 'a'; $v7 = 'b'; $v8 = 'c'; $v9 = 'd'; $v10 = 'e';
        $v11 = [1]; $v12 = [2]; $v13 = [3]; $v14 = [4]; $v15 = [5];
        $v16 = 1.5; $v17 = 2.5; $v18 = 3.5; $v19 = true; $v20 = null;

        if ($depth > 0) {
            return nested($depth - 1);
        }

        gc_collect_cycles();
        $before = memory_get_usage();
        dump_to_devnull();
        gc_collect_cycles();

        return memory_get_usage() - $before;
    }

    $frameRetained = nested(200);
    echo "retained after dumping 200 frames under 64KB: ",
        ($frameRetained < 65536 ? 'yes' : 'no'), " ($frameRetained bytes)\n";

    // --- 3. Walking objects must not retain anything -----------------------

    $objects = [];
    for ($i = 0; $i < 100000; $i++) {
        $objects[] = new Declared();
    }

    gc_collect_cycles();
    $before = memory_get_usage();
    dump_to_devnull();
    gc_collect_cycles();
    $objectRetained = memory_get_usage() - $before;

    echo "retained after dumping 100000 objects under 64KB: ",
        ($objectRetained < 65536 ? 'yes' : 'no'), " ($objectRetained bytes)\n";

    // --- 4. The dump must fit in what is left of memory_limit --------------

    $previousLimit = ini_get('memory_limit');
    ini_set('memory_limit', (memory_get_usage(true) + 8 * 1024 * 1024) . '');

    dump_to_devnull();

    ini_set('memory_limit', $previousLimit);
    echo "dump completed with 8MB of headroom: yes\n";

    // --- 5. A dump that cannot be written must say so ----------------------

    // Silently producing a truncated dump is worse than failing: the caller
    // goes on to analyse invalid JSON as if it were complete.
    $readOnly = fopen(__FILE__, 'r');
    $thrown = null;

    try {
        // `@` silences the stream layer's own "write failed" notice; it does
        // not silence the exception, which is the point of this check.
        @meminfo_dump($readOnly);
    } catch (Exception $exception) {
        $thrown = $exception;
    }

    fclose($readOnly);

    echo "unwritable stream reported: ", $thrown === null ? 'no' : 'yes', "\n";
    echo "reported as: ", $thrown === null ? '(nothing)' : get_class($thrown), "\n";
?>
--EXPECT--
children found: yes
slot order matches table order: yes
decoded child count: 4
typed children: never,set
typed property still uninitialised: yes
retained after dumping 200 frames under 64KB: yes (0 bytes)
retained after dumping 100000 objects under 64KB: yes (0 bytes)
dump completed with 8MB of headroom: yes
unwritable stream reported: yes
reported as: RuntimeException
