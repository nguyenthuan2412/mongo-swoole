<?php
/**
 * End-to-end test suite (API mirrors ext-mongodb + mongodb/mongodb).
 *   php -d extension=modules/swmongo.so testsuite/run.php
 * Env: SWM_RS_URI   (replica set, default mongodb://127.0.0.1:27119/?replicaSet=rs0)
 *      SWM_AUTH_URI (auth enabled standalone, default mongodb://root:secret@127.0.0.1:27118/?authSource=admin)
 *
 * Kept outside tests/: PHP's "make clean" deletes tests/*.php (phpt artifacts).
 */
declare(strict_types=1);

use SwooleMongo\BSON\{Binary, Decimal128, Document, Int64, ObjectId, PackedArray, Persistable, UTCDateTime};
use SwooleMongo\Client;
use SwooleMongo\Driver\{Command, Cursor, ReadPreference, Session, WriteConcern};
use SwooleMongo\Driver\Exception\{AuthenticationException, BulkWriteException, CommandException, ConnectionTimeoutException, RuntimeException};
use SwooleMongo\Driver\Monitoring\{CommandFailedEvent, CommandStartedEvent, CommandSubscriber, CommandSucceededEvent};
use SwooleMongo\Exception\InvalidArgumentException;
use SwooleMongo\GridFS\Exception\FileNotFoundException;
use SwooleMongo\Model\{BSONArray, BSONDocument, IndexInfo};
use SwooleMongo\Operation\{Find, FindOneAndUpdate};
use function Swoole\Coroutine\run;

ini_set('memory_limit', '1G');
Swoole\Runtime::enableCoroutine(SWOOLE_HOOK_ALL);

$RS = getenv('SWM_RS_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
$AUTH = getenv('SWM_AUTH_URI') ?: 'mongodb://root:secret@127.0.0.1:27118/?authSource=admin';
$pass = 0;
$fail = 0;

function check(string $name, callable $fn): void
{
    global $pass, $fail;
    try {
        $fn();
        $pass++;
        echo "  \033[32m✓\033[0m $name\n";
    } catch (Throwable $e) {
        $fail++;
        echo "  \033[31m✗ $name\033[0m\n    ", get_class($e), ': ', $e->getMessage(), ' @ ', basename($e->getFile()), ':', $e->getLine(), "\n";
    }
}

function eq(mixed $exp, mixed $act, string $msg = ''): void
{
    if ($exp != $act) throw new \RuntimeException("$msg expected " . var_export($exp, true) . ' got ' . var_export($act, true));
}

function throwsT(string $class, callable $fn): Throwable
{
    try {
        $fn();
    } catch (Throwable $e) {
        if ($e instanceof $class) return $e;
        throw $e;
    }
    throw new \RuntimeException("expected $class");
}

final class Person implements Persistable
{
    public function __construct(public string $name = '', public int $age = 0) {}
    public function bsonSerialize(): array { return ['name' => $this->name, 'age' => $this->age]; }
    public function bsonUnserialize(array $data): void { $this->name = $data['name']; $this->age = $data['age']; }
}

run(function () use ($RS, $AUTH) {
    $client = new Client($RS, ['appName' => 'swm-tests', 'compressors' => 'zlib']);
    $db = $client->selectDatabase('swm_test');
    $db->drop();
    $users = $db->selectCollection('users');

    echo "CRUD (mongodb/mongodb API)\n";
    check('insertOne / getInsertedId / BSONDocument results', function () use ($users) {
        $r = $users->insertOne(['name' => 'Ann', 'age' => 30, 'tags' => ['a', 'b']]);
        eq(1, $r->getInsertedCount());
        eq(true, $r->getInsertedId() instanceof ObjectId);
        $doc = $users->findOne(['_id' => $r->getInsertedId()]);
        eq(true, $doc instanceof BSONDocument);
        eq('Ann', $doc->name);
        eq(true, $doc['tags'] instanceof BSONArray);
    });
    check('insertMany / getInsertedIds', function () use ($users) {
        $r = $users->insertMany([['name' => 'Bob', 'age' => 25], ['name' => 'Cid', 'age' => 35], (object) ['name' => 'Dee', 'age' => 40]]);
        eq(3, $r->getInsertedCount());
        eq([0, 1, 2], array_keys($r->getInsertedIds()));
    });
    check('find with options + typeMap', function () use ($users) {
        $cursor = $users->find(['age' => ['$gte' => 25]], ['sort' => ['age' => -1], 'skip' => 1, 'limit' => 2, 'projection' => ['_id' => 0, 'name' => 1],
            'typeMap' => ['root' => 'array']]);
        eq(true, $cursor instanceof Cursor);
        eq([['name' => 'Cid'], ['name' => 'Ann']], $cursor->toArray());
        $names = [];
        foreach ($users->find([], ['sort' => ['age' => 1], 'batchSize' => 1]) as $d) $names[] = $d->name;
        eq(['Bob', 'Ann', 'Cid', 'Dee'], $names);
    });
    check('updateOne / updateMany / upsert / replaceOne', function () use ($users) {
        $r = $users->updateOne(['name' => 'Ann'], ['$inc' => ['age' => 1]]);
        eq([1, 1], [$r->getMatchedCount(), $r->getModifiedCount()]);
        eq(3, $users->updateMany(['age' => ['$gt' => 30]], ['$set' => ['senior' => true]])->getMatchedCount());
        $r = $users->updateOne(['name' => 'Eve'], ['$set' => ['age' => 22]], ['upsert' => true]);
        eq(1, $r->getUpsertedCount());
        eq(true, $r->getUpsertedId() instanceof ObjectId);
        eq(1, $users->replaceOne(['name' => 'Eve'], ['name' => 'Eve', 'age' => 23])->getModifiedCount());
        throwsT(InvalidArgumentException::class, fn () => $users->updateOne([], ['name' => 'x']));
        throwsT(InvalidArgumentException::class, fn () => $users->replaceOne([], ['$set' => ['a' => 1]]));
    });
    check('pipeline update', function () use ($users) {
        $users->updateOne(['name' => 'Eve'], [['$set' => ['ageNext' => ['$add' => ['$age', 1]]]]]);
        eq(24, $users->findOne(['name' => 'Eve'])->ageNext);
    });
    check('findOneAndUpdate / Replace / Delete', function () use ($users) {
        $after = $users->findOneAndUpdate(['name' => 'Bob'], ['$set' => ['age' => 26]], ['returnDocument' => FindOneAndUpdate::RETURN_DOCUMENT_AFTER]);
        eq(26, $after->age);
        eq(26, $users->findOneAndReplace(['name' => 'Bob'], ['name' => 'Bob', 'age' => 27])->age);
        eq('Bob', $users->findOneAndDelete(['name' => 'Bob'], ['typeMap' => ['root' => 'array']])['name']);
        eq(null, $users->findOneAndDelete(['name' => 'nobody']));
    });
    check('countDocuments / estimatedDocumentCount / distinct', function () use ($users) {
        eq(4, $users->countDocuments());
        eq(3, $users->countDocuments(['age' => ['$gt' => 30]]));
        eq(1, $users->countDocuments([], ['skip' => 3]));
        eq(4, $users->estimatedDocumentCount());
        $d = $users->distinct('name');
        sort($d);
        eq(['Ann', 'Cid', 'Dee', 'Eve'], $d);
    });
    check('deleteOne / deleteMany', function () use ($users) {
        eq(1, $users->deleteOne(['name' => 'Eve'])->getDeletedCount());
        $users->insertMany([['tmp' => 1], ['tmp' => 1]]);
        eq(2, $users->deleteMany(['tmp' => 1])->getDeletedCount());
    });
    check('aggregate + $out', function () use ($users, $db) {
        $r = $users->aggregate([['$group' => ['_id' => null, 'total' => ['$sum' => '$age']]]])->toArray();
        eq(31 + 35 + 40, $r[0]->total);
        $users->aggregate([['$match' => ['senior' => true]], ['$out' => 'seniors']])->toArray();
        eq(3, $db->selectCollection('seniors')->countDocuments());
    });
    check('explain(Find)', function () use ($users) {
        $e = $users->explain(new Find('swm_test', 'users', ['age' => 30]));
        eq(true, isset($e->queryPlanner));
    });
    check('bulkWrite (library format)', function () use ($db) {
        $c = $db->selectCollection('bulk');
        $r = $c->bulkWrite([
            ['insertOne' => [['_id' => 1, 'v' => 1]]],
            ['insertOne' => [['_id' => 2, 'v' => 2]]],
            ['updateOne' => [['_id' => 1], ['$set' => ['v' => 10]]]],
            ['updateMany' => [[], ['$inc' => ['v' => 1]]]],
            ['replaceOne' => [['_id' => 3], ['v' => 3], ['upsert' => true]]],
            ['deleteOne' => [['_id' => 2]]],
        ]);
        eq([2, 3, 3, 1, 1], [$r->getInsertedCount(), $r->getMatchedCount(), $r->getModifiedCount(), $r->getDeletedCount(), $r->getUpsertedCount()]);
        eq([4 => 3], $r->getUpsertedIds());
        eq([0 => 1, 1 => 2], $r->getInsertedIds());
    });
    check('duplicate key -> BulkWriteException', function () use ($db) {
        $e = throwsT(BulkWriteException::class, fn () => $db->selectCollection('bulk')->insertOne(['_id' => 1]));
        eq(11000, $e->getCode());
        eq(11000, $e->getWriteResult()->getWriteErrors()[0]->getCode());
    });
    check('unordered insertMany continues after error', function () use ($db) {
        $e = throwsT(BulkWriteException::class, fn () => $db->selectCollection('bulk')->insertMany([['_id' => 1], ['_id' => 100], ['_id' => 3], ['_id' => 101]], ['ordered' => false]));
        eq(2, $e->getWriteResult()->getInsertedCount());
        eq([0, 2], array_map(fn ($we) => $we->getIndex(), $e->getWriteResult()->getWriteErrors()));
    });
    check('large insertMany batches (120k docs)', function () use ($db) {
        $c = $db->selectCollection('big');
        $docs = [];
        for ($i = 0; $i < 120000; $i++) $docs[] = ['i' => $i];
        eq(120000, $c->insertMany($docs)->getInsertedCount());
        eq(120000, $c->countDocuments());
        $n = 0;
        foreach ($c->find([], ['projection' => ['i' => 1], 'typeMap' => ['root' => 'array']]) as $_) $n++;
        eq(120000, $n);
    });

    echo "Indexes & management\n";
    check('createIndex / listIndexes(IndexInfo) / dropIndex', function () use ($users, $db) {
        eq('age_1', $users->createIndex(['age' => 1]));
        eq(['name_1_age_-1'], $users->createIndexes([['key' => ['name' => 1, 'age' => -1], 'unique' => true]]));
        $names = [];
        foreach ($users->listIndexes() as $idx) {
            eq(true, $idx instanceof IndexInfo);
            $names[] = $idx->getName();
            if ($idx->getName() === 'name_1_age_-1') eq(true, $idx->isUnique());
        }
        eq(['_id_', 'age_1', 'name_1_age_-1'], $names);
        $users->dropIndex('age_1');
        eq(2, iterator_count($users->listIndexes()));
        eq(0, iterator_count($db->selectCollection('nonexistent')->listIndexes()));
    });
    check('createCollection / listCollections / modify / rename / drop', function () use ($db) {
        $db->createCollection('capped', ['capped' => true, 'size' => 4096]);
        $info = null;
        foreach ($db->listCollections(['filter' => ['name' => 'capped']]) as $ci) $info = $ci;
        eq(true, $info->isCapped());
        eq(4096, $info->getCappedSize());
        $db->modifyCollection('capped', ['validationLevel' => 'off']);
        $db->renameCollection('capped', 'capped2');
        eq(true, in_array('capped2', iterator_to_array($db->listCollectionNames()), true));
        $db->dropCollection('capped2');
        eq(false, in_array('capped2', iterator_to_array($db->listCollectionNames()), true));
    });
    check('Database::command / Client::listDatabases', function () use ($client, $db) {
        eq(1, (int) $db->command(['ping' => 1])->toArray()[0]->ok);
        eq(true, in_array('swm_test', iterator_to_array($client->listDatabaseNames()), true));
        $found = false;
        foreach ($client->listDatabases() as $info) if ($info->getName() === 'swm_test') $found = true;
        eq(true, $found);
    });

    echo "BSON\n";
    check('types round trip', function () use ($db) {
        $c = $db->selectCollection('types');
        $doc = ['_id' => 1, 'dec' => new Decimal128('12.340'), 'i64' => new Int64(5), 'big' => PHP_INT_MAX,
            'bin' => new Binary(random_bytes(16), Binary::TYPE_UUID), 'date' => new UTCDateTime(1700000000123),
            'dt' => new DateTimeImmutable('2024-02-03T04:05:06.789Z'), 'nested' => ['a' => [1, ['b' => null]]], 'f' => 1.25];
        $c->insertOne($doc);
        $got = $c->findOne(['_id' => 1], ['typeMap' => ['root' => 'array', 'document' => 'array', 'array' => 'array']]);
        eq('12.340', (string) $got['dec']);
        eq(PHP_INT_MAX, $got['big']);
        eq($doc['bin'], $got['bin']);
        eq('1706933106789', (string) $got['dt']);
        eq($doc['nested'], $got['nested']);
        eq(1.25, $got['f']);
    });
    check('Persistable __pclass round trip', function () use ($db) {
        $c = $db->selectCollection('people');
        $c->insertOne(new Person('Zoe', 9));
        $p = $c->findOne([], ['typeMap' => []]);
        eq(true, $p instanceof Person);
        eq('Zoe', $p->name);
    });
    check('Document / PackedArray / Extended JSON', function () {
        $d = Document::fromJSON('{"a": {"$numberLong": "5"}, "o": {"$oid": "65f0a1b2c3d4e5f6a7b8c9d0"}, "arr": [1, 2]}');
        eq(5, $d->get('a'));
        eq(true, $d['arr'] instanceof PackedArray);
        eq('{ "a" : 5, "o" : { "$oid" : "65f0a1b2c3d4e5f6a7b8c9d0" }, "arr" : [ 1, 2 ] }', $d->toRelaxedExtendedJSON());
        eq((string) $d, (string) Document::fromBSON((string) $d));
        eq([1, 2], PackedArray::fromPHP([1, 2])->toPHP());
    });

    echo "Sessions & transactions\n";
    check('Session::withTransaction commits', function () use ($client, $db) {
        $acc = $db->selectCollection('accounts');
        $acc->insertMany([['_id' => 'a', 'bal' => 100], ['_id' => 'b', 'bal' => 0]]);
        $s = $client->startSession();
        $s->withTransaction(function (Session $s) use ($acc) {
            $acc->updateOne(['_id' => 'a'], ['$inc' => ['bal' => -30]], ['session' => $s]);
            $acc->updateOne(['_id' => 'b'], ['$inc' => ['bal' => 30]], ['session' => $s]);
        });
        $s->endSession();
        eq([70, 30], array_map(fn ($d) => $d->bal, $acc->find([], ['sort' => ['_id' => 1]])->toArray()));
    });
    check('abort rolls back / isolation / Functions::withTransaction', function () use ($client, $db) {
        $acc = $db->selectCollection('accounts');
        $s = $client->startSession();
        $s->startTransaction(['writeConcern' => new WriteConcern(WriteConcern::MAJORITY)]);
        $acc->updateOne(['_id' => 'a'], ['$set' => ['bal' => 0]], ['session' => $s]);
        eq(0, $acc->findOne(['_id' => 'a'], ['session' => $s])->bal);
        eq(70, $acc->findOne(['_id' => 'a'])->bal);
        $s->abortTransaction();
        eq(70, $acc->findOne(['_id' => 'a'])->bal);
        eq(Session::TRANSACTION_ABORTED, $s->getTransactionState());
        SwooleMongo\Functions::withTransaction($s, fn (Session $s) => $acc->updateOne(['_id' => 'b'], ['$inc' => ['bal' => 1]], ['session' => $s]));
        eq(31, $acc->findOne(['_id' => 'b'])->bal);
        $s->endSession();
    });

    echo "Change streams\n";
    check('watch + resume token', function () use ($db) {
        $c = $db->selectCollection('stream');
        $c->insertOne(['init' => 1]);
        $cs = $c->watch([], ['maxAwaitTimeMS' => 200, 'fullDocument' => 'updateLookup']);
        $cs->rewind();
        eq(false, $cs->valid());
        go(function () use ($c) {
            Swoole\Coroutine::sleep(0.2);
            $c->insertOne(['n' => 1]);
            $c->updateOne(['n' => 1], ['$set' => ['n' => 2]]);
        });
        $events = [];
        for ($i = 0; $i < 40 && count($events) < 2; $i++) {
            $cs->next();
            if ($cs->valid()) $events[] = $cs->current();
        }
        eq(['insert', 'update'], array_map(fn ($e) => $e->operationType, $events));
        eq(2, $events[1]->fullDocument->n);
        $token = $cs->getResumeToken();
        $c->insertOne(['n' => 3]);
        $cs2 = $c->watch([], ['resumeAfter' => $token, 'maxAwaitTimeMS' => 200]);
        for ($cs2->rewind(), $i = 0; !$cs2->valid() && $i < 20; $i++) $cs2->next();
        eq(3, $cs2->current()->fullDocument->n);
    });
    check('client watch', function () use ($client, $db) {
        $cs = $client->watch([['$match' => ['ns.coll' => 'clientwatch']]], ['maxAwaitTimeMS' => 200]);
        $cs->rewind();
        $db->selectCollection('clientwatch')->insertOne(['z' => 1]);
        for ($i = 0; !$cs->valid() && $i < 20; $i++) $cs->next();
        eq('swm_test', $cs->current()->ns->db);
    });

    echo "GridFS (stream wrapper)\n";
    check('Bucket upload / download / byName / rename / delete', function () use ($db) {
        $bucket = $db->selectGridFSBucket(['bucketName' => 'files', 'chunkSizeBytes' => 1000]);
        $data = random_bytes(5500);
        $src = fopen('php://temp', 'w+');
        fwrite($src, $data);
        rewind($src);
        $id = $bucket->uploadFromStream('a.bin', $src, ['metadata' => ['k' => 'v']]);
        $dst = fopen('php://temp', 'w+');
        $bucket->downloadToStream($id, $dst);
        rewind($dst);
        eq($data, stream_get_contents($dst));
        $stream = $bucket->openDownloadStream($id);
        fseek($stream, 1500);
        eq(substr($data, 1500, 2100), fread($stream, 2100));
        eq('v', $bucket->getFileDocumentForStream($stream)->metadata->k);
        $up = $bucket->openUploadStream('a.bin');
        fwrite($up, 'v2');
        fclose($up);
        eq('v2', stream_get_contents($bucket->openDownloadStreamByName('a.bin')));
        eq($data, stream_get_contents($bucket->openDownloadStreamByName('a.bin', ['revision' => 0])));
        $bucket->rename($id, 'renamed.bin');
        eq(1, count($bucket->find(['filename' => 'renamed.bin'])->toArray()));
        $bucket->delete($id);
        throwsT(FileNotFoundException::class, fn () => $bucket->openDownloadStream($id));
        $bucket->drop();
    });

    echo "Driver & coroutines\n";
    check('100 concurrent coroutines', function () use ($db) {
        $c = $db->selectCollection('conc');
        $wg = new Swoole\Coroutine\WaitGroup();
        for ($i = 0; $i < 100; $i++) {
            $wg->add();
            go(function () use ($c, $i, $wg) { $c->insertOne(['i' => $i]); $c->findOne(['i' => $i]); $wg->done(); });
        }
        $wg->wait();
        eq(100, $c->countDocuments());
    });
    check('non-blocking: 20 x 200ms server sleeps run in parallel', function () use ($db) {
        $c = $db->selectCollection('conc');
        $t = microtime(true);
        $wg = new Swoole\Coroutine\WaitGroup();
        for ($i = 0; $i < 20; $i++) {
            $wg->add();
            go(function () use ($c, $wg) { $c->findOne(['$where' => 'sleep(200) || true']); $wg->done(); });
        }
        $wg->wait();
        $elapsed = microtime(true) - $t;
        if ($elapsed > 1.5) throw new \RuntimeException("took {$elapsed}s");
        printf("    (%.2fs for 4s of serial work)\n", $elapsed);
    });
    check('fresh client hit by 50 coroutines at once', function () use ($RS) {
        $coll = (new Client($RS))->selectCollection('swm_test', 'conc');
        $wg = new Swoole\Coroutine\WaitGroup();
        $errors = [];
        for ($i = 0; $i < 50; $i++) {
            $wg->add();
            go(function () use ($coll, $wg, &$errors) {
                try { $coll->findOne(['$where' => 'sleep(50) || true']); } catch (Throwable $e) { $errors[] = $e->getMessage(); }
                $wg->done();
            });
        }
        $wg->wait();
        eq([], array_unique($errors));
    });
    check('CommandSubscriber monitoring', function () use ($client, $db) {
        $sub = new class implements CommandSubscriber {
            public array $events = [];
            public function commandStarted(CommandStartedEvent $e): void { $this->events[] = 'S:' . $e->getCommandName(); }
            public function commandSucceeded(CommandSucceededEvent $e): void { $this->events[] = 'OK:' . $e->getCommandName(); }
            public function commandFailed(CommandFailedEvent $e): void { $this->events[] = 'F:' . $e->getCommandName(); }
        };
        $client->addSubscriber($sub);
        $db->selectCollection('users')->findOne();
        try { $db->command(['nope' => 1]); } catch (CommandException) {}
        $client->removeSubscriber($sub);
        $db->selectCollection('users')->findOne();
        eq(['S:find', 'OK:find', 'S:nope', 'F:nope'], $sub->events);
    });
    check('Manager / Server / ReadPreference', function () use ($client, $RS) {
        $server = $client->getManager()->selectServer(new ReadPreference(ReadPreference::PRIMARY));
        eq(true, $server->isPrimary());
        eq(SwooleMongo\Driver\Server::TYPE_RS_PRIMARY, $server->getType());
        $c2 = new Client($RS . '&readPreference=secondaryPreferred');
        eq('secondaryPreferred', $c2->getReadPreference()->getModeString());
        eq(1, $c2->selectCollection('swm_test', 'users')->countDocuments(['name' => 'Ann']));
        $cur = $client->getManager()->executeCommand('admin', new Command(['ping' => 1]));
        eq(1, (int) $cur->toArray()[0]->ok);
    });

    echo "Auth & errors\n";
    check('SCRAM-SHA-256 / SHA-1', function () use ($AUTH) {
        $c = new Client($AUTH);
        $c->selectCollection('authdb', 'x')->insertOne(['a' => 1]);
        eq(1, $c->selectCollection('authdb', 'x')->countDocuments(['a' => 1]));
        $c->dropDatabase('authdb');
        eq(1, (int) (new Client($AUTH . '&authMechanism=SCRAM-SHA-1'))->selectDatabase('admin')->command(['ping' => 1])->toArray()[0]->ok);
    });
    check('bad password -> AuthenticationException', function () use ($AUTH) {
        throwsT(AuthenticationException::class, fn () => (new Client(str_replace('secret', 'nope', $AUTH)))->selectDatabase('admin')->command(['ping' => 1]));
    });
    check('server selection timeout -> ConnectionTimeoutException', function () {
        $t = microtime(true);
        throwsT(ConnectionTimeoutException::class, fn () => (new Client('mongodb://127.0.0.1:1/?serverSelectionTimeoutMS=500&connectTimeoutMS=200'))
            ->selectDatabase('admin')->command(['ping' => 1]));
        $el = microtime(true) - $t;
        if ($el < 0.4 || $el > 3) throw new \RuntimeException("selection took {$el}s");
    });

    $db->drop();
});

echo "\n$pass passed, $fail failed\n";
exit($fail ? 1 : 0);
