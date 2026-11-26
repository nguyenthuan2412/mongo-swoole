<?php
// Verifies that the negotiated compressor is really used (zstd when ext-zstd is loaded, otherwise zlib).
use SwooleMongo\Client;
$failed = true;
Swoole\Coroutine\run(function () use (&$failed) {
    $uri = getenv('SWM_RS_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
    $want = extension_loaded('zstd') ? 'zstd' : 'zlib';
    $c = new Client($uri . (str_contains($uri, '?') ? '&' : '?') . 'compressors=' . $want);
    $stats = function () use ($c, $want) {
        $s = $c->selectDatabase('admin')->command(['serverStatus' => 1])->toArray()[0];
        return (int) ($s->network->compression->{$want}->decompressor->bytesOut ?? 0);
    };
    $coll = $c->selectCollection('swm_comp', 'c');
    $coll->drop();
    $before = $stats();
    $big = str_repeat('compress me ', 20000);
    $coll->insertOne(['_id' => 1, 'big' => $big]);
    $ok = $coll->findOne(['_id' => 1], ['typeMap' => ['root' => 'array']])['big'] === $big;
    $delta = $stats() - $before;
    printf("compressor=%s round trip=%s, server decompressed %d bytes from this client\n", $want, $ok ? 'ok' : 'FAILED', $delta);
    $coll->drop();
    $failed = !$ok || $delta < strlen($big);
});
exit($failed ? 1 : 0);
