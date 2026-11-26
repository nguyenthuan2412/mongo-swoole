<?php
Swoole\Coroutine\run(function () {
    $t = microtime(true); $codes = []; $bodies = [];
    $wg = new Swoole\Coroutine\WaitGroup();
    for ($i = 0; $i < 40; $i++) { $wg->add(); go(function () use (&$codes, &$bodies, $wg) {
        $c = new Swoole\Coroutine\Http\Client('127.0.0.1', 9616); $c->set(['timeout' => 20]); $c->get('/slow');
        $codes[] = $c->statusCode; if ($c->statusCode != 200) $bodies[] = $c->body; $wg->done(); }); }
    $wg->wait();
    printf("40 x /slow (100ms each) on 2 workers: %.2fs codes=%s\n", microtime(true) - $t, json_encode(array_count_values($codes)));
    if ($bodies) echo implode("\n", array_unique($bodies)), "\n";
});
