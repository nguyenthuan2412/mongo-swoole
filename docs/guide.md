# swoole-mongo user guide

**English** · [Tiếng Việt](guide.vi.md) · [Back to README](../README.md)

This guide walks through everyday use of swoole-mongo, from the first connection to transactions, change streams, GridFS,
error handling and production patterns for Swoole servers. Every example uses the high level `SwooleMongo\Client` API,
which matches [mongodb/mongodb](https://www.mongodb.com/docs/php-library/current/). Code written for that library
usually only needs its namespace changed from `MongoDB\` to `SwooleMongo\`.

1. [Running code in coroutines](#1-running-code-in-coroutines)
2. [Connecting](#2-connecting)
3. [Databases and collections](#3-databases-and-collections)
4. [Inserting documents](#4-inserting-documents)
5. [Querying](#5-querying)
6. [Updating and replacing](#6-updating-and-replacing)
7. [Deleting](#7-deleting)
8. [Bulk writes](#8-bulk-writes)
9. [Aggregation, counting and distinct](#9-aggregation-counting-and-distinct)
10. [Indexes](#10-indexes)
11. [BSON types and type maps](#11-bson-types-and-type-maps)
12. [Sessions and transactions](#12-sessions-and-transactions)
13. [Change streams](#13-change-streams)
14. [GridFS (files)](#14-gridfs-files)
15. [Read and write concerns, read preference, timeouts](#15-read-and-write-concerns-read-preference-timeouts)
16. [Error handling](#16-error-handling)
17. [Running many operations at once](#17-running-many-operations-at-once)
18. [Swoole servers and connection pools](#18-swoole-servers-and-connection-pools)
19. [Authentication examples](#19-authentication-examples)
20. [Monitoring and logging](#20-monitoring-and-logging)
21. [The low level driver API](#21-the-low-level-driver-api)
22. [Troubleshooting](#22-troubleshooting)

---

## 1. Running code in coroutines

swoole-mongo is only non-blocking inside a Swoole coroutine with the runtime hooks enabled:

```php
// CLI script: Co\run() creates a coroutine and enables SWOOLE_HOOK_ALL
Co\run(function () {
    $client = new SwooleMongo\Client('mongodb://localhost:27017');
    // ...
});
```

```php
// Swoole server: enable the hooks in the server settings
$server->set(['hook_flags' => SWOOLE_HOOK_ALL]);
```

Without Swoole (php-fpm, plain CLI) the same code works synchronously, like any other driver.
If you call the driver inside a coroutine while the hooks are disabled, swoole-mongo prints a warning, because each call would block the whole worker.
To silence it, set `swoole_mongo.warn_blocking=0`.

## 2. Connecting

```php
use SwooleMongo\Client;

$client = new Client('mongodb://localhost:27017');                        // standalone
$client = new Client('mongodb://h1:27017,h2:27017/?replicaSet=rs0');      // replica set
$client = new Client('mongodb+srv://user:pass@cluster0.example.net/');    // Atlas / DNS seed list
$client = new Client('mongodb://user:pass@localhost/?authSource=admin');  // with credentials
```

The constructor takes three arguments: `new Client(?string $uri, array $uriOptions = [], array $driverOptions = [])`.

```php
$client = new Client('mongodb://localhost', [
    // URI options, the same keys you can put in the connection string
    'appname'                  => 'billing-api',
    'maxPoolSize'              => 50,
    'serverSelectionTimeoutMS' => 5000,
    'retryWrites'              => true,
    'compressors'              => 'zstd,zlib',
    'tls'                      => true,
    'tlsCAFile'                => '/etc/ssl/mongo-ca.pem',
], [
    // driver options
    'typeMap'   => ['root' => 'array', 'document' => 'array', 'array' => 'array'],
    'serverApi' => new SwooleMongo\Driver\ServerApi(SwooleMongo\Driver\ServerApi::V1),
]);
```

Connecting is lazy: nothing is sent until the first operation. A `Client` is cheap to keep and safe to share between coroutines,
so create it once and reuse it. Do not create one per request.

| Common URI option | Default | Meaning |
|---|---|---|
| `maxPoolSize` | 100 | Maximum connections per server (= maximum concurrent operations per server) |
| `serverSelectionTimeoutMS` | 30000 | How long to wait for a suitable server |
| `connectTimeoutMS` | 10000 | TCP / TLS connect timeout |
| `socketTimeoutMS` | 0 (none) | Per read/write timeout |
| `timeoutMS` | none | Total time limit for each operation (CSOT) |
| `heartbeatFrequencyMS` | 10000 | Topology refresh interval |
| `readPreference` | primary | `primaryPreferred`, `secondary`, `secondaryPreferred`, `nearest` |
| `w`, `journal`, `wtimeoutMS` | server default | Write concern |
| `readConcernLevel` | server default | `local`, `majority`, `snapshot`… |
| `retryReads`, `retryWrites` | true | Retry once after a network error or failover |
| `compressors` | none | `zstd`, `zlib`, `snappy` |
| `directConnection` | false | Talk to one host only, without discovering the replica set |
| `loadBalanced` | false | Connect through a load balancer to mongos |
| `proxyHost`, `proxyPort`, `proxyUsername`, `proxyPassword` | none | SOCKS5 proxy |

## 3. Databases and collections

```php
$db    = $client->selectDatabase('shop');          // or $client->shop
$items = $db->selectCollection('items');           // or $db->items, or $client->selectCollection('shop', 'items')

$db->createCollection('logs', ['capped' => true, 'size' => 10 * 1024 * 1024]);
foreach ($db->listCollectionNames() as $name) echo $name, PHP_EOL;
foreach ($client->listDatabaseNames() as $name) echo $name, PHP_EOL;

$db->renameCollection('logs', 'logs_old');
$db->dropCollection('logs_old');
$db->command(['ping' => 1])->toArray();           // any database command
```

`withOptions()` returns a copy with other defaults. Nothing changes on the original object:

```php
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\WriteConcern;

$fastWrites = $items->withOptions(['writeConcern' => new WriteConcern(1)]);
$fromReplica = $items->withOptions(['readPreference' => new ReadPreference(ReadPreference::SECONDARY_PREFERRED)]);
```

## 4. Inserting documents

```php
$result = $items->insertOne(['sku' => 'A-1', 'name' => 'Pen', 'price' => 1.5, 'tags' => ['office']]);
$id = $result->getInsertedId();                    // SwooleMongo\BSON\ObjectId, generated by the driver

$result = $items->insertMany([
    ['sku' => 'A-2', 'name' => 'Pencil'],
    ['sku' => 'A-3', 'name' => 'Eraser'],
]);
$result->getInsertedCount();                        // 2
$result->getInsertedIds();                          // [0 => ObjectId, 1 => ObjectId]
```

- Set `_id` yourself to use your own identifier: `['_id' => 'A-1', ...]`.
- `insertMany` splits large inputs into batches automatically, so 100 000 documents in a single call work.
- `['ordered' => false]` keeps inserting after an error, such as a duplicate key, and reports every failure at the end.

## 5. Querying

```php
$pen = $items->findOne(['sku' => 'A-1']);           // document or null
echo $pen->name;                                    // default results are BSONDocument (object + array access)
echo $pen['price'];

$cursor = $items->find(
    ['price' => ['$gte' => 1, '$lt' => 10], 'tags' => 'office'],
    [
        'projection' => ['name' => 1, 'price' => 1, '_id' => 0],
        'sort'       => ['price' => -1],
        'skip'       => 20,
        'limit'      => 10,
        'batchSize'  => 100,
        'collation'  => ['locale' => 'en', 'strength' => 2],
        'hint'       => ['price' => 1],
        'maxTimeMS'  => 2000,
        'comment'    => 'product list page',
    ],
);

foreach ($cursor as $doc) {
    echo $doc->name, ' ', $doc->price, PHP_EOL;
}
```

- A cursor fetches the next batch from the server only when you iterate past the current one, and it yields to other coroutines while it waits.
- `$cursor->toArray()` reads everything into an array. Only use it when the result is small.
- A cursor can be iterated once. To read again, run the query again.

**Query operators**: the usual MongoDB operators all work, for example `$eq`, `$in`, `$regex`, `$exists`, `$elemMatch`, `$text` and `$geoWithin`.

```php
use SwooleMongo\BSON\Regex;
use SwooleMongo\BSON\UTCDateTime;

$items->find(['name' => new Regex('^pen', 'i')]);
$items->find(['createdAt' => ['$gte' => new UTCDateTime(strtotime('-1 day') * 1000)]]);
$items->find(['$or' => [['stock' => 0], ['discontinued' => true]]]);
```

**Pagination**: for large collections, filter on an indexed field rather than using `skip`:

```php
$page = $items->find(['_id' => ['$gt' => $lastSeenId]], ['sort' => ['_id' => 1], 'limit' => 50]);
```

## 6. Updating and replacing

```php
$r = $items->updateOne(['sku' => 'A-1'], ['$set' => ['price' => 1.75], '$inc' => ['views' => 1]]);
$r->getMatchedCount();   // 1
$r->getModifiedCount();  // 1 (0 if nothing changed)

$items->updateMany(['tags' => 'office'], ['$addToSet' => ['tags' => 'sale']]);

// insert when nothing matches
$r = $items->updateOne(['sku' => 'B-9'], ['$set' => ['name' => 'Stapler']], ['upsert' => true]);
$r->getUpsertedId();

// replace the whole document (except _id)
$items->replaceOne(['sku' => 'A-1'], ['sku' => 'A-1', 'name' => 'Blue pen', 'price' => 2]);

// update with an aggregation pipeline
$items->updateMany([], [['$set' => ['total' => ['$multiply' => ['$price', '$qty']]]]]);

// update an array element
$items->updateOne(
    ['sku' => 'A-1'],
    ['$set' => ['variants.$[v].stock' => 0]],
    ['arrayFilters' => [['v.color' => 'red']]],
);
```

**Read and modify in one atomic step**:

```php
use SwooleMongo\Operation\FindOneAndUpdate;

$counter = $db->counters->findOneAndUpdate(
    ['_id' => 'invoice'],
    ['$inc' => ['seq' => 1]],
    ['upsert' => true, 'returnDocument' => FindOneAndUpdate::RETURN_DOCUMENT_AFTER],
);
$nextInvoiceNumber = $counter->seq;
```

`findOneAndReplace()` and `findOneAndDelete()` work the same way.

## 7. Deleting

```php
$items->deleteOne(['sku' => 'A-3'])->getDeletedCount();     // 0 or 1
$items->deleteMany(['discontinued' => true])->getDeletedCount();
```

## 8. Bulk writes

Send many different writes in as few round trips as possible:

```php
$result = $items->bulkWrite([
    ['insertOne'  => [['sku' => 'C-1', 'name' => 'Ruler']]],
    ['updateOne'  => [['sku' => 'A-1'], ['$set' => ['price' => 2.5]]]],
    ['updateMany' => [['tags' => 'sale'], ['$unset' => ['discount' => '']]]],
    ['replaceOne' => [['sku' => 'B-9'], ['sku' => 'B-9', 'name' => 'Big stapler'], ['upsert' => true]]],
    ['deleteOne'  => [['sku' => 'C-0']]],
    ['deleteMany' => [['stock' => 0]]],
], ['ordered' => false]);

$result->getInsertedCount();
$result->getModifiedCount();
$result->getDeletedCount();
$result->getUpsertedIds();
```

**MongoDB 8.0+**: a single bulk write can span several collections and databases:

```php
use SwooleMongo\ClientBulkWrite;

$bulk = ClientBulkWrite::createWithCollection($db->orders, ['ordered' => true]);
$bulk->insertOne(['_id' => 1001, 'total' => 25], $orderId);
$bulk->withCollection($db->stock)->updateOne(['sku' => 'A-1'], ['$inc' => ['qty' => -1]]);
$result = $client->bulkWrite($bulk);
```

## 9. Aggregation, counting and distinct

```php
$cursor = $db->orders->aggregate([
    ['$match' => ['status' => 'paid', 'createdAt' => ['$gte' => new UTCDateTime(strtotime('first day of this month') * 1000)]]],
    ['$group' => ['_id' => '$customerId', 'total' => ['$sum' => '$amount'], 'orders' => ['$sum' => 1]]],
    ['$sort' => ['total' => -1]],
    ['$limit' => 10],
    ['$lookup' => ['from' => 'customers', 'localField' => '_id', 'foreignField' => '_id', 'as' => 'customer']],
], ['allowDiskUse' => true]);

foreach ($cursor as $row) {
    printf("%s: %.2f (%d orders)\n", $row->customer[0]->name ?? $row->_id, $row->total, $row->orders);
}
```

```php
$items->countDocuments(['tags' => 'office']);   // exact count of a filter
$items->estimatedDocumentCount();               // fast count from collection metadata
$items->distinct('tags', ['price' => ['$gt' => 1]]);
```

## 10. Indexes

```php
$items->createIndex(['sku' => 1], ['unique' => true]);
$items->createIndex(['name' => 'text']);
$items->createIndex(['createdAt' => 1], ['expireAfterSeconds' => 86400]);   // TTL: delete after 1 day
$items->createIndexes([
    ['key' => ['category' => 1, 'price' => -1]],
    ['key' => ['location' => '2dsphere'], 'name' => 'geo'],
]);

foreach ($items->listIndexes() as $index) {
    echo $index->getName(), ' ', json_encode($index->getKey()), PHP_EOL;
}
$items->dropIndex('sku_1');
```

Atlas Search indexes use `createSearchIndex()`, `listSearchIndexes()`, `updateSearchIndex()` and `dropSearchIndex()`.

## 11. BSON types and type maps

| PHP value | Stored as |
|---|---|
| `int`, `float`, `bool`, `string`, `null` | int32 / int64, double, bool, string, null |
| list `[1, 2]` | array |
| associative array, `stdClass` | document |
| `SwooleMongo\BSON\ObjectId` | ObjectId |
| `UTCDateTime` | date (milliseconds) |
| `Decimal128` | decimal128 (exact money values) |
| `Int64`, `Binary`, `Regex`, `Timestamp`, `Javascript`, `MinKey`, `MaxKey` | matching BSON type |

```php
use SwooleMongo\BSON\{ObjectId, UTCDateTime, Decimal128, Binary};

$id = new ObjectId();
$id->getTimestamp();                            // creation time
$date = new UTCDateTime();                      // now
$date->toDateTime();                            // \DateTime
$money = new Decimal128('19.99');
$uuid = Binary::fromUuid('1b4e28ba-2fa1-11d2-883f-0016d3cca427');   // (subtype 4)
```

**Type maps** decide what documents become when they are read:

```php
// default for Client: SwooleMongo\Model\BSONDocument / BSONArray (ArrayObject)
$client = new Client($uri);

// plain PHP arrays everywhere (the fastest option)
$client = new Client($uri, [], ['typeMap' => ['root' => 'array', 'document' => 'array', 'array' => 'array']]);

// per operation
$items->findOne([], ['typeMap' => ['root' => 'array']]);
```

**Mapping to your own classes**: implement `SwooleMongo\BSON\Persistable`. The class name is stored in `__pclass`, and documents read with an empty typeMap come back as that class:

```php
class Product implements SwooleMongo\BSON\Persistable
{
    public function __construct(public string $sku = '', public float $price = 0) {}

    public function bsonSerialize(): array
    {
        return ['sku' => $this->sku, 'price' => $this->price];
    }

    public function bsonUnserialize(array $data): void
    {
        $this->sku = $data['sku'];
        $this->price = $data['price'];
    }
}

$items->insertOne(new Product('P-1', 9.5));
// an empty typeMap lets __pclass decide the class (the Client's default typeMap would return BSONDocument)
$product = $items->findOne(['sku' => 'P-1'], ['typeMap' => []]);     // instance of Product
```

**JSON**: use `SwooleMongo\BSON\Document::fromPHP($doc)->toRelaxedExtendedJSON()` or `Document::fromJSON($json)`.

## 12. Sessions and transactions

Transactions need a replica set or a sharded cluster.

**Recommended**: `withTransaction()` commits for you and retries the whole callback on transient errors:

```php
$session = $client->startSession();

$session->withTransaction(function (SwooleMongo\Driver\Session $session) use ($db) {
    $db->accounts->updateOne(['_id' => 'alice'], ['$inc' => ['balance' => -100]], ['session' => $session]);
    $db->accounts->updateOne(['_id' => 'bob'],   ['$inc' => ['balance' => 100]],  ['session' => $session]);
    $db->transfers->insertOne(['from' => 'alice', 'to' => 'bob', 'amount' => 100], ['session' => $session]);
});
```

- Pass `['session' => $session]` to **every** operation inside the transaction.
- Throwing an exception from the callback aborts the transaction.

**Manual control**:

```php
$session = $client->startSession();
$session->startTransaction(['writeConcern' => new WriteConcern(WriteConcern::MAJORITY)]);
try {
    $db->accounts->updateOne(['_id' => 'alice'], ['$inc' => ['balance' => -100]], ['session' => $session]);
    $session->commitTransaction();
} catch (Throwable $e) {
    $session->abortTransaction();
    throw $e;
} finally {
    $session->endSession();
}
```

**Other kinds of session**:
- **Causal consistency** (the default for explicit sessions): a read in the session sees the session's earlier writes, even on a secondary.
- **Snapshot reads**: `$client->startSession(['snapshot' => true])` reads every query from the same point in time.

One session belongs to one coroutine at a time. Do not use the same session from two coroutines concurrently.

## 13. Change streams

Change streams let you react to changes as they happen. They need a replica set or a sharded cluster.

```php
$stream = $db->orders->watch(
    [['$match' => ['operationType' => ['$in' => ['insert', 'update']]]]],
    ['fullDocument' => 'updateLookup'],
);

for ($stream->rewind(); true; $stream->next()) {
    if (!$stream->valid()) {
        continue;                                   // no event yet; next() waits again (non-blocking)
    }
    $event = $stream->current();
    echo $event->operationType, ' ', json_encode($event->fullDocument), PHP_EOL;

    $resumeToken = $stream->getResumeToken();       // save it to continue after a restart
}
```

- To continue after a restart, pass `['resumeAfter' => $savedToken]` (or `startAfter`).
- The stream also resumes on its own after network errors and failovers.
- Run each change stream in its own coroutine (`go(fn () => ...)`). The rest of the worker keeps serving requests while it waits.
- `$client->watch()` and `$db->watch()` watch the whole deployment or one database.

## 14. GridFS (files)

GridFS stores files larger than the 16 MB document limit by splitting them into chunks.

```php
$bucket = $db->selectGridFSBucket();               // ['bucketName' => 'fs', 'chunkSizeBytes' => 261120]

// upload
$fileId = $bucket->uploadFromStream('report.pdf', fopen('/tmp/report.pdf', 'rb'), ['metadata' => ['owner' => 'alice']]);

$upload = $bucket->openUploadStream('log.txt');
fwrite($upload, "line 1\n");
fclose($upload);

// download
$bucket->downloadToStream($fileId, fopen('/tmp/copy.pdf', 'wb'));
$stream = $bucket->openDownloadStreamByName('report.pdf');   // latest revision
echo stream_get_contents($stream);

// manage
foreach ($bucket->find(['metadata.owner' => 'alice']) as $file) {
    echo $file->filename, ' ', $file->length, PHP_EOL;
}
$bucket->rename($fileId, 'report-2026.pdf');
$bucket->delete($fileId);
```

## 15. Read and write concerns, read preference, timeouts

```php
use SwooleMongo\Driver\{ReadConcern, ReadPreference, WriteConcern};

// durable write: acknowledged by the majority, journaled, wait at most 5 s
$items->insertOne($doc, ['writeConcern' => new WriteConcern(WriteConcern::MAJORITY, 5000, true)]);

// fire and forget (no reply; fastest, no error reporting)
$items->insertOne($doc, ['writeConcern' => new WriteConcern(0)]);

// read from a secondary in a given data centre, at most 90 s behind the primary
$items->find([], ['readPreference' => new ReadPreference(ReadPreference::SECONDARY, [['dc' => 'sg']], ['maxStalenessSeconds' => 90])]);

// only read data that is committed on a majority
$items->find([], ['readConcern' => new ReadConcern(ReadConcern::MAJORITY)]);
```

**Timeouts**:
- `timeoutMS` (client or operation option) is the total time limit for an operation. It covers server selection, connecting, retries and the server-side `maxTimeMS`.
- When it expires, an `ExecutionTimeoutException` or `ConnectionTimeoutException` is thrown.

```php
$client = new Client('mongodb://localhost/?timeoutMS=2000');
$items->find($filter, ['timeoutMS' => 500]);
```

## 16. Error handling

All exceptions implement `SwooleMongo\Driver\Exception\Exception`.

| Exception | When |
|---|---|
| `ConnectionTimeoutException` | No suitable server within `serverSelectionTimeoutMS` / `timeoutMS` |
| `ConnectionException` | Network error while talking to the server |
| `AuthenticationException` | Wrong credentials or mechanism |
| `ExecutionTimeoutException` | The operation exceeded `maxTimeMS` / `timeoutMS` |
| `BulkWriteException` | A write failed (duplicate key, validation…); details in `getWriteResult()` |
| `CommandException` | The server rejected a command; `getResultDocument()` has the reply |
| `InvalidArgumentException` | Invalid option or argument |
| `EncryptionException` | Client-side encryption error |

```php
use SwooleMongo\Driver\Exception\{BulkWriteException, ConnectionTimeoutException, ExecutionTimeoutException, RuntimeException};

try {
    $users->insertOne(['email' => 'ann@example.com']);
} catch (BulkWriteException $e) {
    foreach ($e->getWriteResult()->getWriteErrors() as $error) {
        if ($error->getCode() === 11000) {
            throw new DomainException('This e-mail is already registered');
        }
    }
    throw $e;
} catch (ExecutionTimeoutException $e) {
    // the query took too long
} catch (ConnectionTimeoutException $e) {
    // the database is unreachable: return 503
} catch (RuntimeException $e) {
    if ($e->hasErrorLabel('TransientTransactionError')) {
        // safe to retry the transaction
    }
}
```

Retryable reads and writes already retry once after a network error or a primary failover. You do not need your own retry loop for those cases.

## 17. Running many operations at once

Inside one worker, start coroutines and wait for them:

```php
use Swoole\Coroutine\WaitGroup;

Co\run(function () use ($client) {
    $db = $client->selectDatabase('shop');
    $results = [];
    $wg = new WaitGroup();

    foreach (['orders', 'items', 'customers'] as $name) {
        $wg->add();
        go(function () use ($db, $name, &$results, $wg) {
            $results[$name] = $db->selectCollection($name)->estimatedDocumentCount();
            $wg->done();
        });
    }
    $wg->wait();                                    // the three counts ran in parallel
    print_r($results);
});
```

Tips:
- Each concurrent operation uses its own connection, up to `maxPoolSize` per server. Extra operations wait for a free connection (`waitQueueTimeoutMS`).
- Bound the concurrency yourself for huge fan-outs, for example with a `Swoole\Coroutine\Channel` used as a semaphore.
- A cursor or a session belongs to the coroutine using it. Do not iterate the same cursor from two coroutines.

## 18. Swoole servers and connection pools

**One client per worker** is the recommended setup. The client already manages its own connections:

```php
$server = new Swoole\Http\Server('0.0.0.0', 9501);
$server->set(['worker_num' => swoole_cpu_num(), 'hook_flags' => SWOOLE_HOOK_ALL]);

$server->on('workerStart', function () {
    $GLOBALS['mongo'] = new SwooleMongo\Client(getenv('MONGO_URI'), ['maxPoolSize' => 64]);
});

$server->on('request', function ($req, $res) {
    $users = $GLOBALS['mongo']->selectCollection('app', 'users');
    $res->end(json_encode($users->findOne(['_id' => (int) $req->get['id']])));
});

$server->start();
```

- Create the client in `workerStart`, **not** before `$server->start()`. Connections must not be shared across forked processes.
- Total connections to MongoDB ≈ `worker_num × maxPoolSize` per server. Keep that within the server's connection limit.

**External pool libraries** (for example a framework connection pool): give each pooled client a single connection and let the pool limit concurrency:

```php
$pool = new Swoole\ConnectionPool(fn () => new SwooleMongo\Client($uri, ['maxPoolSize' => 1]), 32);
$client = $pool->get();
try {
    $client->selectCollection('app', 'users')->insertOne($doc);
} finally {
    $pool->put($client);
}
```

## 19. Authentication examples

```php
// SCRAM (default); special characters must be URL-encoded
new Client('mongodb://' . rawurlencode($user) . ':' . rawurlencode($pass) . '@db.example.com/?authSource=admin');

// X.509 client certificate
new Client('mongodb://db.example.com/?tls=true&authMechanism=MONGODB-X509&tlsCertificateKeyFile=/etc/ssl/client.pem&tlsCAFile=/etc/ssl/ca.pem');

// AWS IAM (credentials from the environment, ECS or EC2 when not in the URI)
new Client('mongodb+srv://cluster0.example.net/?authMechanism=MONGODB-AWS&authSource=$external');

// OIDC with your own token provider
new Client('mongodb+srv://cluster0.example.net/?authMechanism=MONGODB-OIDC', [], [
    'oidcCallback' => fn (array $context) => ['accessToken' => fetchTokenFromIdp(), 'expiresInSeconds' => 3600],
]);

// OIDC on Azure / GCP / Kubernetes
new Client('mongodb+srv://cluster0.example.net/?authMechanism=MONGODB-OIDC&authMechanismProperties=ENVIRONMENT:azure,TOKEN_RESOURCE:api://my-app');

// Kerberos: run `kinit app@EXAMPLE.COM` first, or put the password in the URI
new Client('mongodb://app%40EXAMPLE.COM@db.example.com/?authMechanism=GSSAPI&authMechanismProperties=SERVICE_NAME:mongodb');
```

Client-side field level encryption is covered in the [README](../README.md#client-side-field-level-encryption).

## 20. Monitoring and logging

Subscribe to command events to log slow queries or collect metrics:

```php
use SwooleMongo\Driver\Monitoring\{CommandSubscriber, CommandStartedEvent, CommandSucceededEvent, CommandFailedEvent};

final class SlowQueryLogger implements CommandSubscriber
{
    public function commandStarted(CommandStartedEvent $event): void {}

    public function commandSucceeded(CommandSucceededEvent $event): void
    {
        if ($event->getDurationMicros() > 100_000) {
            error_log(sprintf('slow %s on %s: %d ms', $event->getCommandName(), $event->getDatabaseName(), $event->getDurationMicros() / 1000));
        }
    }

    public function commandFailed(CommandFailedEvent $event): void
    {
        error_log('mongo error: ' . $event->getError()->getMessage());
    }
}

$client->addSubscriber(new SlowQueryLogger());
```

`SDAMSubscriber` reports topology changes, such as a new primary, a server going down or heartbeats.

## 21. The low level driver API

`SwooleMongo\Driver\*` mirrors ext-mongodb, for when you need full control:

```php
use SwooleMongo\Driver\{Manager, Query, BulkWrite, Command};

$manager = new Manager('mongodb://localhost:27017');      // or $client->getManager()

$bulk = new BulkWrite(['ordered' => true]);
$bulk->insert(['x' => 1]);
$bulk->update(['x' => 1], ['$set' => ['y' => 2]]);
$bulk->delete(['x' => 0], ['limit' => 1]);
$result = $manager->executeBulkWrite('shop.items', $bulk);

$cursor = $manager->executeQuery('shop.items', new Query(['x' => 1], ['limit' => 10]));
$cursor->setTypeMap(['root' => 'array']);
foreach ($cursor as $doc) { /* ... */ }

$reply = $manager->executeCommand('admin', new Command(['serverStatus' => 1]))->toArray()[0];
```

## 22. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Warning: *running inside a Swoole coroutine but the runtime hook … is disabled* | Enable `SWOOLE_HOOK_ALL` (`Co\run` does it; servers need `hook_flags`). |
| `ConnectionTimeoutException: No suitable server` | The URI host or port is wrong, a firewall blocks it, or the replica set advertises host names your app cannot resolve. Try `directConnection=true` to check. |
| `AuthenticationException` | Wrong user/password or `authSource`. Users are often defined in `admin`. Passwords with special characters must be URL-encoded. |
| Too many connections on the server | Lower `maxPoolSize` or `worker_num`; create clients only in `workerStart`. |
| A request hangs on `ChangeStream` | Iterate change streams in their own coroutine; set `maxAwaitTimeMS`. |
| `Class "SwooleMongo\Client" not found` | `swoole_mongo.enable_library` is `0`, or the extension is not loaded (`php --ri swoole_mongo`). |
| `ClientEncryption` class missing | The extension was built without libmongocrypt; rebuild with `--with-swoole-mongo-mongocrypt`. |
