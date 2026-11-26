<?php
/** Same workload as bench/node/bench.mjs, using swmongo inside Swoole coroutines. Prints a JSON array. */
use SwooleMongo\Client;
use SwooleMongo\BSON\UTCDateTime;

Swoole\Runtime::enableCoroutine(SWOOLE_HOOK_ALL);
$uri = getenv('BENCH_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
$N = (int) (getenv('BENCH_N') ?: 5000);
$CONC = (int) (getenv('BENCH_CONC') ?: 100);
$results = [];

Swoole\Coroutine\run(function () use ($uri, $N, $CONC, &$results) {
    $doc = fn (int $i) => ['i' => $i, 'name' => 'user' . $i, 'email' => "user$i@example.com", 'age' => $i % 90, 'active' => $i % 2 === 0,
        'tags' => ['a', 'b', 'c'], 'address' => ['city' => 'Hanoi', 'zip' => '100000', 'geo' => [105.8, 21.0]], 'score' => $i * 1.5, 'created' => new UTCDateTime()];
    $client = new Client($uri . "&maxPoolSize=$CONC");
    $col = $client->selectCollection('bench', 'php');
    $col->drop();
    $bench = function (string $name, int $ops, callable $fn) use (&$results) {
        $lat = [];
        $t0 = hrtime(true);
        $fn($lat);
        $ms = (hrtime(true) - $t0) / 1e6;
        sort($lat);
        $p = fn ($q) => $lat ? number_format($lat[min(count($lat) - 1, (int) floor(count($lat) * $q))], 3, '.', '') : '-';
        $results[] = ['name' => $name, 'ops' => $ops, 'ms' => $ms, 'opsSec' => $ops / $ms * 1000, 'p50' => $p(0.5), 'p99' => $p(0.99)];
    };
    $timed = function (array &$lat, callable $f) { $t = hrtime(true); $f(); $lat[] = (hrtime(true) - $t) / 1e6; };

    $bench('insertOne (sequential)', $N, function (&$lat) use ($N, $col, $doc, $timed) { for ($i = 0; $i < $N; $i++) $timed($lat, fn () => $col->insertOne($doc($i))); });
    $bench('findOne by field (sequential)', $N, function (&$lat) use ($N, $col, $timed) { for ($i = 0; $i < $N; $i++) $timed($lat, fn () => $col->findOne(['i' => $i])); });
    $col->createIndex(['i' => 1]);
    $bench('findOne indexed (sequential)', $N, function (&$lat) use ($N, $col, $timed) { for ($i = 0; $i < $N; $i++) $timed($lat, fn () => $col->findOne(['i' => $i])); });
    $bench('updateOne indexed (sequential)', $N, function (&$lat) use ($N, $col, $timed) { for ($i = 0; $i < $N; $i++) $timed($lat, fn () => $col->updateOne(['i' => $i], ['$inc' => ['age' => 1]])); });
    $bench('find 100 docs toArray', 500, function (&$lat) use ($col, $timed) { for ($k = 0; $k < 500; $k++) $timed($lat, fn () => $col->find(['i' => ['$gte' => $k]], ['limit' => 100])->toArray()); });
    $bench('find all 5000 docs (iterate)', 20, function (&$lat) use ($col, $timed) { for ($k = 0; $k < 20; $k++) $timed($lat, function () use ($col) { $n = 0; foreach ($col->find([]) as $d) $n++; }); });
    $bench('insertMany 1000 docs', 20, function (&$lat) use ($col, $doc, $timed) { for ($k = 0; $k < 20; $k++) $timed($lat, fn () => $col->insertMany(array_map(fn ($j) => $doc(100000 + $k * 1000 + $j), range(0, 999)))); });
    $bench('aggregate $group', 200, function (&$lat) use ($col, $timed) { for ($k = 0; $k < 200; $k++) $timed($lat, fn () => $col->aggregate([['$match' => ['age' => ['$lt' => 50]]], ['$group' => ['_id' => '$active', 'n' => ['$sum' => 1], 'avg' => ['$avg' => '$score']]]])->toArray()); });
    $CN = $N * 4;
    $concurrent = function (string $name, callable $op) use ($bench, $CN, $CONC, $timed) {
        $bench("$name concurrent x$CONC", $CN, function (&$lat) use ($CN, $CONC, $op, $timed) {
            $next = 0;
            $wg = new Swoole\Coroutine\WaitGroup();
            for ($w = 0; $w < $CONC; $w++) {
                $wg->add();
                go(function () use (&$next, &$lat, $CN, $op, $timed, $wg) {
                    while ($next < $CN) { $i = $next++; $timed($lat, fn () => $op($i)); }
                    $wg->done();
                });
            }
            $wg->wait();
        });
    };
    $concurrent('findOne', fn ($i) => $col->findOne(['i' => $i % $N]));
    $concurrent('insertOne', fn ($i) => $col->insertOne($doc(200000 + $i)));
});
echo json_encode($results), "\n";
