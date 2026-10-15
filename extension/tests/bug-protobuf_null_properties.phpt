--TEST--
Check that objects whose get_properties handler returns NULL (ext-protobuf) produce valid JSON
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (!extension_loaded('protobuf')) die('skip protobuf ext not loaded');
?>
--FILE--
<?php
    $timestamp = new Google\Protobuf\Timestamp();
    $timestamp->setSeconds(1700000000);

    $value = new Google\Protobuf\Value();
    $value->setStringValue('hello');

    $struct = new Google\Protobuf\Struct();
    $structFields = $struct->getFields();
    $structFields['key'] = $value;

    $listValue = new Google\Protobuf\ListValue();
    $listValues = $listValue->getValues();

    $dump = fopen('php://memory', 'rw');

    meminfo_dump($dump);

    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    if (is_array($meminfoData)) {
        echo "meminfo_dump JSON decode ok\n";
    } else {
        echo "meminfo_dump JSON decode fail: ".json_last_error_msg()."\n";
        exit;
    }

    $classes = [];
    foreach ($meminfoData['items'] as $item) {
        if ($item['type'] === 'object' && strpos($item['class'], 'Google\\Protobuf\\') === 0) {
            $classes[] = $item['class'].': '.(isset($item['children']) && $item['children'] === [] ? 'empty children' : 'unexpected children');
        }
    }
    sort($classes);

    echo implode("\n", $classes)."\n";
?>
--EXPECT--
meminfo_dump JSON decode ok
Google\Protobuf\Internal\MapField: empty children
Google\Protobuf\Internal\RepeatedField: empty children
Google\Protobuf\ListValue: empty children
Google\Protobuf\Struct: empty children
Google\Protobuf\Timestamp: empty children
Google\Protobuf\Value: empty children
