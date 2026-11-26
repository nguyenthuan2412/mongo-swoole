<?php
// Memory stability + abandoned cursor cleanup with the mongodb/mongodb style API.
use SwooleMongo\Client;
use SwooleMongo\Driver\Session;
Swoole\Runtime::enableCoroutine(SWOOLE_HOOK_ALL);
$failed = false;
Swoole\Coroutine\run(function () use (&$failed) {
    $client = new Client(getenv('SWM_RS_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0');
    $coll = $client->selectCollection('swm_leak', 'x');
    $coll->drop();
    $coll->insertMany(array_map(fn ($i) => ['i' => $i, 's' => str_repeat('x', 100)], range(1, 50)));
    $round = function () use ($client, $coll) {
        for ($i = 0; $i < 200; $i++) {
            $coll->insertOne(['k' => $i]);
            $coll->find([], ['batchSize' => 7])->toArray();
            $cur = $coll->find([], ['batchSize' => 5]);
            $cur->rewind();
            unset($cur); // abandoned cursor -> killCursors queue
            $coll->updateOne(['k' => $i], ['$set' => ['v' => 1]]);
            try { $coll->insertOne(['_id' => 1]); $coll->insertOne(['_id' => 1]); } catch (Throwable) {}
            $s = $client->startSession();
            $s->withTransaction(fn (Session $s) => $coll->deleteOne(['k' => $i], ['session' => $s]));
            $s->endSession();
        }
    };
    $round();
    gc_collect_cycles();
    $m1 = memory_get_usage();
    $round();
    $round();
    gc_collect_cycles();
    $m2 = memory_get_usage();
    printf("memory before=%d after=%d delta=%d bytes\n", $m1, $m2, $m2 - $m1);
    $client->selectDatabase('admin')->command(['ping' => 1]); // flushes queued killCursors
    $open = $client->selectDatabase('admin')->command(['serverStatus' => 1])->toArray()[0]->metrics->cursor->open->total;
    echo "open server cursors: $open\n";
    $coll->drop();
    $failed = ($m2 - $m1) > 64 * 1024 || $open > 0;
});
exit($failed ? 1 : 0);
