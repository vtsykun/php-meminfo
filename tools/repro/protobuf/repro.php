<?php
/**
 * Dumps memory holding ext-protobuf (and ext-grpc, when loaded) objects and checks the result
 * is valid JSON. Exit code 1 if a plain json_decode() of the dump fails, 0 otherwise.
 *
 * Usage: php repro.php [dump-file]
 */

use Google\Protobuf\Any;
use Google\Protobuf\ListValue;
use Google\Protobuf\Struct;
use Google\Protobuf\Timestamp;
use Google\Protobuf\Value;

$timestamp = new Timestamp();
$timestamp->setSeconds(1700000000);

$value = new Value();
$value->setStringValue('hello');

$struct = new Struct();
$structFields = $struct->getFields();
$structFields['key'] = $value;

$any = new Any();
$any->setTypeUrl('type.googleapis.com/google.protobuf.Timestamp');
$any->setValue($timestamp->serializeToString());

$parsedTimestamp = new Timestamp();
$parsedTimestamp->mergeFromString($timestamp->serializeToString());

$listValue = new ListValue();
$listValues = $listValue->getValues();

if (class_exists('Grpc\Channel')) {
    $channel = new Grpc\Channel('localhost:50051', [
        'credentials' => Grpc\ChannelCredentials::createInsecure(),
    ]);
}

$stream = fopen('php://memory', 'w+');
meminfo_dump($stream);
rewind($stream);
$json = stream_get_contents($stream);
fclose($stream);

if (isset($argv[1])) {
    file_put_contents($argv[1], $json);
}

$dump = json_decode($json, true);
$decodeError = json_last_error();

echo 'json_decode: ', json_last_error_msg(), "\n";
echo 'valid UTF-8: ', mb_check_encoding($json, 'UTF-8') ? 'yes' : 'no', "\n";

if ($decodeError === JSON_ERROR_NONE) {
    foreach ($dump['items'] as $itemId => $item) {
        if ($item['type'] === 'object' && !isset($item['children'])) {
            echo 'object without children: ', $itemId, ' ', $item['class'], "\n";
        }
    }
}

exit($decodeError === JSON_ERROR_NONE ? 0 : 1);
