<?php

namespace BitOne\PhpMemInfo;

use Symfony\Component\Serializer\Encoder\JsonEncoder;
use Symfony\Component\Filesystem\Filesystem;

/**
 * Load a dump file as an array.
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @copyright 2016 Benoit Jacquemont
 * @license   http://opensource.org/licenses/MIT MIT
 */
class Loader
{
    /**
     * Load the items from a file and return them.
     *
     * @param string $filename
     *
     * @return array
     */
    public function load($filename)
    {
        $filesystem = new FileSystem();

        if (!$filesystem->exists($filename)) {
            throw new \InvalidArgumentException(
                sprintf('Provided filename is not accessible: %s', $filename)
            );
        }

        $jsonEncoder = new JsonEncoder();

        $data = file_get_contents($filename);

        try {
            $allData = $jsonEncoder->decode($data, JsonEncoder::FORMAT);
        } catch (\Throwable $e) {
            try {
                $regex = <<<'END'
/
  (
    (?: [\x00-\x7F]                 # single-byte sequences   0xxxxxxx
    |   [\xC0-\xDF][\x80-\xBF]      # double-byte sequences   110xxxxx 10xxxxxx
    |   [\xE0-\xEF][\x80-\xBF]{2}   # triple-byte sequences   1110xxxx 10xxxxxx * 2
    |   [\xF0-\xF7][\x80-\xBF]{3}   # quadruple-byte sequence 11110xxx 10xxxxxx * 3
    ){1,100}                        # ...one or more times
  )
| .                                 # anything else
/x
END;

                $data = preg_replace($regex, '$1', $data);
                $allData = $jsonEncoder->decode($data, JsonEncoder::FORMAT);
            } catch (\Throwable) {
                throw $e;
            }
        }


        return $allData['items'];
    }
}
