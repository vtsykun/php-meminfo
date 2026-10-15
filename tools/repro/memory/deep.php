<?php
/**
 * A structure deep enough to overflow the C stack.
 *
 * The traversal used to recurse once per level, at roughly 305 bytes of C stack
 * each, so it killed the process somewhere above 28 000 levels on an 8 MB stack
 * — and around 7 000 inside a fiber, which gets 2 MB.
 *
 * Usage:
 *   php -d memory_limit=-1 -d extension=.../meminfo.so deep.php [depth] [object|array]
 *
 * Note: PHP frees a deep *object* chain by recursing and segfaults on it at
 * shutdown with no extension loaded at all, so the structure is unwound
 * iteratively here. Without that, the engine's own limit looks like a crash in
 * the dump.
 */

$depth = isset($argv[1]) ? (int) $argv[1] : 200000;
$kind  = isset($argv[2]) ? $argv[2] : 'object';

class ReproNode
{
    public $next;
}

if ($kind === 'object') {
    $head = new ReproNode();
    $cursor = $head;
    for ($i = 0; $i < $depth; $i++) {
        $cursor->next = new ReproNode();
        $cursor = $cursor->next;
    }
    unset($cursor);
} else {
    // Built from the inside out: appending at the deep end would be quadratic.
    $head = array('leaf' => 1);
    for ($i = 0; $i < $depth; $i++) {
        $head = array('next' => $head);
    }
}

$dumpFile = tempnam(sys_get_temp_dir(), 'meminfo-repro');

$started = microtime(true);
$handle = fopen($dumpFile, 'w');
meminfo_dump($handle);
fclose($handle);
$elapsed = microtime(true) - $started;

$size = filesize($dumpFile);

// Count items by streaming: decoding a dump of this size costs about a
// kilobyte of PHP memory per item.
$handle = fopen($dumpFile, 'r');
$items = 0;
$carry = '';
while (($chunk = fread($handle, 1 << 20)) !== '' && $chunk !== false) {
    $buffer = $carry . $chunk;
    $items += substr_count($buffer, '"is_root"');
    $carry = substr($buffer, -16);
}
fclose($handle);

printf(
    "depth=%d kind=%s seconds=%.2f dump_bytes=%d items=%d\n",
    $depth,
    $kind,
    $elapsed,
    $size,
    $items
);

unlink($dumpFile);

if ($kind === 'object') {
    while ($head !== null) {
        $next = $head->next;
        $head->next = null;
        $head = $next;
    }
} else {
    while (is_array($head) && isset($head['next'])) {
        $inner = $head['next'];
        unset($head['next']);
        $head = $inner;
    }
}

echo "unwound cleanly\n";
