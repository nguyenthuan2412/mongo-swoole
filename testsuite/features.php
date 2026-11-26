<?php
/**
 * Tests for features beyond basic CRUD. Optional environments are skipped when the env var is empty:
 *   SWM_RS_URI     replica set (default mongodb://127.0.0.1:27119/?replicaSet=rs0)
 *   SWM_RS8_URI    MongoDB 8 replica set, for Client::bulkWrite (e.g. mongodb://127.0.0.1:27120/?replicaSet=rs8)
 *   SWM_LB_URI     load balanced mongos (e.g. mongodb://127.0.0.1:27200/?loadBalanced=true)
 *   SWM_SOCKS_URI  URI through a SOCKS5 proxy
 */
declare(strict_types=1);

use SwooleMongo\{Client, ClientBulkWrite};
use SwooleMongo\Driver\{Command, ServerApi, Session, WriteConcern};
use SwooleMongo\Driver\Exception\{BulkWriteCommandException, ExecutionTimeoutException, InvalidArgumentException};
use SwooleMongo\Driver\Monitoring as M;

Swoole\Runtime::enableCoroutine(SWOOLE_HOOK_ALL);
$pass = $fail = $skip = 0;
function t(string $name, ?string $env, callable $fn): void
{
    global $pass, $fail, $skip;
    if ($env !== null && !getenv($env)) { $skip++; echo "  - $name (skipped, set $env)\n"; return; }
    try { $fn(); $pass++; echo "  \033[32m✓\033[0m $name\n"; }
    catch (Throwable $e) { $fail++; echo "  \033[31m✗ $name\033[0m: ", get_class($e), ': ', $e->getMessage(), ' @', $e->getLine(), "\n"; }
}
function ok(bool $c, string $m = 'assertion failed'): void { if (!$c) throw new RuntimeException($m); }

Swoole\Coroutine\run(function () {
    $RS = getenv('SWM_RS_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
    $coll = (new Client($RS))->selectCollection('swm_feat', 'c');
    $coll->drop();
    $coll->insertOne(['a' => 1]);

    t('serverApi strict', null, function () use ($RS) {
        $c = new Client($RS, [], ['serverApi' => new ServerApi(ServerApi::V1, true)]);
        ok($c->selectCollection('swm_feat', 'c')->countDocuments() >= 1);
        $s = $c->startSession();
        $s->withTransaction(fn (Session $s) => $c->selectCollection('swm_feat', 'c')->insertOne(['tx' => 1], ['session' => $s]));
        try { $c->selectDatabase('swm_feat')->command(['collStats' => 'c']); ok(false, 'collStats allowed'); }
        catch (SwooleMongo\Driver\Exception\CommandException $e) { ok($e->getCode() === 323); }
    });
    t('w:0 uses moreToCome (no reply)', null, function () use ($coll) {
        $r = $coll->insertOne(['unack' => 1], ['writeConcern' => new WriteConcern(0)]);
        ok(!$r->isAcknowledged());
        Swoole\Coroutine::sleep(0.2);
        ok($coll->countDocuments(['unack' => 1]) === 1);
    });
    t('timeoutMS (operation and client)', null, function () use ($coll, $RS) {
        $t = microtime(true);
        try { $coll->findOne(['$where' => 'sleep(2000) || true'], ['timeoutMS' => 300]); ok(false); } catch (ExecutionTimeoutException) {}
        ok(microtime(true) - $t < 1.0, 'timeout too slow');
        try { (new Client($RS . '&timeoutMS=300'))->selectCollection('swm_feat', 'c')->findOne(['$where' => 'sleep(2000) || true']); ok(false); }
        catch (ExecutionTimeoutException) {}
    });
    t('snapshot session', null, function () use ($coll, $RS) {
        $s = $coll->getManager()->startSession(['snapshot' => true]);
        $n1 = $coll->countDocuments([], ['session' => $s]);
        $coll->insertOne(['later' => 1]);
        ok($n1 === $coll->countDocuments([], ['session' => $s]));
        try { $coll->insertOne(['x' => 1], ['session' => $s]); ok(false); } catch (InvalidArgumentException) {}
    });
    t('SDAM events', null, function () use ($RS) {
        $sub = new class implements M\SDAMSubscriber {
            public array $e = [];
            public function serverChanged(M\ServerChangedEvent $e): void { $this->e[] = 'serverChanged'; }
            public function serverClosed(M\ServerClosedEvent $e): void { $this->e[] = 'serverClosed'; }
            public function serverOpening(M\ServerOpeningEvent $e): void { $this->e[] = 'serverOpening'; }
            public function serverHeartbeatFailed(M\ServerHeartbeatFailedEvent $e): void { $this->e[] = 'hbFailed'; }
            public function serverHeartbeatStarted(M\ServerHeartbeatStartedEvent $e): void { $this->e[] = 'hbStarted'; }
            public function serverHeartbeatSucceeded(M\ServerHeartbeatSucceededEvent $e): void { $this->e[] = 'hbSucceeded'; }
            public function topologyChanged(M\TopologyChangedEvent $e): void { $this->e[] = 'topologyChanged:' . $e->getNewDescription()->getType(); }
            public function topologyClosed(M\TopologyClosedEvent $e): void { $this->e[] = 'topologyClosed'; }
            public function topologyOpening(M\TopologyOpeningEvent $e): void { $this->e[] = 'topologyOpening'; }
        };
        $m = new SwooleMongo\Driver\Manager($RS);
        $m->addSubscriber($sub);
        $m->executeCommand('admin', new Command(['ping' => 1]));
        $m->close();
        ok($sub->e === ['topologyOpening', 'serverOpening', 'hbStarted', 'hbSucceeded', 'serverChanged', 'topologyChanged:ReplicaSetWithPrimary', 'topologyClosed'], json_encode($sub->e));
    });
    t('maxStalenessSeconds', null, function () use ($RS) {
        ok((new Client($RS . '&readPreference=secondaryPreferred&maxStalenessSeconds=90'))->selectCollection('swm_feat', 'c')->countDocuments() >= 1);
    });
    t('Client::bulkWrite (MongoDB 8)', 'SWM_RS8_URI', function () {
        $c = new Client(getenv('SWM_RS8_URI'));
        $a = $c->selectCollection('swm_feat', 'a'); $b = $c->selectCollection('swm_feat', 'b');
        $a->drop(); $b->drop();
        $r = $c->bulkWrite(ClientBulkWrite::createWithCollection($a, ['verboseResults' => true])
            ->insertOne(['x' => 1], $id)->withCollection($b)->insertOne(['y' => 1])->updateMany(['y' => 1], ['$set' => ['z' => 1]]));
        ok($r->getInsertedCount() === 2 && $r->getModifiedCount() === 1);
        ok($r->getInsertResults()->toPHP()->{'0'}->insertedId == $id);
        try { $c->bulkWrite(ClientBulkWrite::createWithCollection($a)->insertOne(['_id' => 1])->insertOne(['_id' => 1])); ok(false); }
        catch (BulkWriteCommandException $e) { ok(array_keys($e->getWriteErrors()) === [1]); }
    });
    t('load balanced mode (pinned cursors & transactions)', 'SWM_LB_URI', function () {
        $c = new Client(getenv('SWM_LB_URI'));
        $col = $c->selectCollection('swm_feat', 'lb');
        $col->drop();
        $col->insertMany(array_map(fn ($i) => ['i' => $i], range(1, 300)));
        $n = 0;
        foreach ($col->find([], ['batchSize' => 25]) as $_) $n++;
        ok($n === 300);
        $s = $c->startSession();
        $s->withTransaction(fn (Session $s) => $col->insertOne(['tx' => 1], ['session' => $s]));
        ok($col->countDocuments(['tx' => 1]) === 1);
    });
    t('SOCKS5 proxy', 'SWM_SOCKS_URI', function () {
        ok((int) (new Client(getenv('SWM_SOCKS_URI')))->selectDatabase('admin')->command(['ping' => 1])->toArray()[0]->ok === 1);
    });
    $coll->drop();
});
echo "\n$pass passed, $fail failed, $skip skipped\n";
exit($fail ? 1 : 0);
