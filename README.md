<div align="center">

# swoole-mongo

**A non-blocking MongoDB driver for PHP and Swoole, written in C.**

[![CI](https://github.com/nguyenthuan2412/mongo-swoole/actions/workflows/ci.yml/badge.svg)](https://github.com/nguyenthuan2412/mongo-swoole/actions/workflows/ci.yml)
![PHP](https://img.shields.io/badge/PHP-8.1%2B-777bb4)
![MongoDB](https://img.shields.io/badge/MongoDB-7.x%20%7C%208.x-47a248)
![License](https://img.shields.io/badge/license-MIT-blue)

**English** · [Tiếng Việt](README.vi.md) · [User guide](docs/guide.md)

</div>

---

swoole-mongo gives PHP applications the full MongoDB feature set without blocking the event loop. Every socket read
and write yields to other Swoole coroutines, so a single worker can run thousands of queries at the same time.

The API mirrors the official PHP stack class for class. If you know `ext-mongodb` and `mongodb/mongodb`, you already know swoole-mongo.

| Official PHP stack | swoole-mongo |
|---|---|
| `MongoDB\Driver\*`, `MongoDB\BSON\*` (ext-mongodb) | `SwooleMongo\Driver\*`, `SwooleMongo\BSON\*` (C) |
| `MongoDB\Client`, `Database`, `Collection`, `GridFS\Bucket`, `Operation\*`, `Model\*` (mongodb/mongodb) | `SwooleMongo\Client`, … (PHP, **embedded in the extension**) |

One `swoole_mongo.so` is all you need. It needs no ext-mongodb, no libmongoc/libbson and no Composer packages.

## Contents

- [Why swoole-mongo](#why-swoole-mongo)
- [Requirements](#requirements)
- [Installation](#installation) — [PIE](#with-pie-recommended) · [PECL](#with-pecl) · [from source](#from-source) · [Docker](#docker)
- [Quick start](#quick-start)
- [Using it in a Swoole server](#using-it-in-a-swoole-server)
- [Features](#features)
- [Authentication](#authentication)
- [Client-side field level encryption](#client-side-field-level-encryption)
- [Configuration](#configuration)
- [Performance](#performance)
- [Testing](#testing)
- [Limitations](#limitations)
- [User guide](docs/guide.md)
- [License](#license)

## Why swoole-mongo

The official driver is built on libmongoc. libmongoc waits on raw file descriptors with `poll()`, so inside a coroutine it blocks
the whole worker, and it cannot survive a coroutine switch in the middle of an operation.

swoole-mongo implements the whole driver itself in C: BSON, the OP_MSG wire protocol, authentication, server discovery and monitoring,
connection handling and cursors. Sockets are opened with PHP streams, so Swoole's runtime hooks make them coroutine aware.

- **Inside a coroutine** (`Co\run()`, a Swoole server with `hook_flags`), I/O is non-blocking.
- **Outside a coroutine** (php-fpm, CLI scripts), it behaves like a normal synchronous driver.

## Requirements

| | |
|---|---|
| PHP | 8.1 or newer, NTS (ZTS is not supported). CI runs PHP 8.3 |
| OS | Linux, macOS (x86_64 and arm64) |
| Swoole | 5.x for non-blocking I/O (optional) |
| zlib | required |
| Optional | `ext-intl` (SASLprep for non-ASCII passwords), `ext-zstd` (zstd compression), MIT Kerberos (GSSAPI), libmongocrypt ≥ 1.8 (CSFLE / Queryable Encryption) |

## Installation

### With PIE (recommended)

[PIE](https://github.com/php/pie) is the official PHP extension installer. It builds the extension and enables it in `php.ini` for you.

```sh
pie install nguyenthuan2412/swoole-mongo
```

To enable the optional features, pass the matching flags. They need the libraries listed in [Requirements](#requirements).

```sh
pie install nguyenthuan2412/swoole-mongo --with-swoole-mongo-gssapi --with-swoole-mongo-mongocrypt
```

<details>
<summary>Installing straight from GitHub (before the package is on Packagist, or for a development version)</summary>

```sh
pie repository:add vcs https://github.com/nguyenthuan2412/mongo-swoole
pie install nguyenthuan2412/swoole-mongo:dev-main
```
</details>

### With PECL

```sh
# from a release archive
pecl install https://github.com/nguyenthuan2412/mongo-swoole/releases/download/v0.2.0/swoole_mongo-0.2.0.tgz

# from a downloaded archive
pecl install swoole_mongo-0.2.0.tgz
```

PECL asks whether to enable Kerberos and libmongocrypt. Press Enter to keep `auto`, which enables each one if it is installed.
Then add the extension to `php.ini`:

```ini
extension=swoole_mongo
```

### From source

```sh
git clone https://github.com/nguyenthuan2412/mongo-swoole.git && cd mongo-swoole
./build.sh                                   # phpize + configure + make
sudo make install
echo "extension=swoole_mongo" | sudo tee "$(php -r 'echo PHP_CONFIG_FILE_SCAN_DIR;')/50-swoole_mongo.ini"
```

Build options (all optional; `auto` enables a feature when its library is found):

| Option | Needs | Enables |
|---|---|---|
| `--with-swoole-mongo-gssapi[=DIR]` | MIT Kerberos (`libkrb5-dev`, `brew install krb5`) | `authMechanism=GSSAPI` |
| `--with-swoole-mongo-mongocrypt[=DIR]` | libmongocrypt ≥ 1.8 (pkg-config or prefix) | `ClientEncryption`, `autoEncryption` |

```sh
./build.sh --with-swoole-mongo-gssapi --with-swoole-mongo-mongocrypt=/opt/libmongocrypt
```

Check the installation:

```sh
php --ri swoole_mongo
```

### Docker

The bundled `Dockerfile` builds PHP 8.3 with Swoole, swoole-mongo, zstd, intl, Kerberos, libmongocrypt and crypt_shared:

```sh
docker build -t swoole-mongo .
```

## Quick start

```php
<?php
use SwooleMongo\Client;
use SwooleMongo\Operation\FindOneAndUpdate;

Co\run(function () {
    $client = new Client('mongodb://localhost:27017/?replicaSet=rs0');
    $users  = $client->selectCollection('app', 'users');

    // Create
    $id = $users->insertOne(['name' => 'Ann', 'age' => 30])->getInsertedId();

    // Read
    $ann = $users->findOne(['_id' => $id]);          // BSONDocument: $ann->name or $ann['name']
    foreach ($users->find(['age' => ['$gte' => 18]], ['sort' => ['age' => -1], 'limit' => 10]) as $user) {
        echo $user->name, PHP_EOL;
    }

    // Update
    $users->updateOne(['_id' => $id], ['$inc' => ['age' => 1]]);
    $after = $users->findOneAndUpdate(
        ['_id' => $id],
        ['$set' => ['vip' => true]],
        ['returnDocument' => FindOneAndUpdate::RETURN_DOCUMENT_AFTER],
    );

    // Transactions
    $session = $client->startSession();
    $session->withTransaction(function ($session) use ($users) {
        $users->updateOne(['name' => 'Ann'], ['$set' => ['level' => 2]], ['session' => $session]);
    });

    // Queries run concurrently: the 10 coroutines below share one worker
    for ($i = 0; $i < 10; $i++) {
        go(fn () => $users->countDocuments(['age' => ['$gt' => $i]]));
    }
});
```

Code written for `mongodb/mongodb` usually only needs its namespace changed from `MongoDB\` to `SwooleMongo\`.

📖 **The [user guide](docs/guide.md) covers every topic in detail.** It goes from connecting and querying to transactions, change streams, GridFS, error handling, concurrency patterns and troubleshooting.

## Using it in a Swoole server

Create one `Client` per worker in `workerStart` and enable the runtime hooks:

```php
$server = new Swoole\Http\Server('0.0.0.0', 9501);
$server->set(['worker_num' => 4, 'hook_flags' => SWOOLE_HOOK_ALL]);

$server->on('workerStart', function () use (&$client) {
    $client = new SwooleMongo\Client('mongodb://localhost:27017');
});

$server->on('request', function ($request, $response) use (&$client) {
    $doc = $client->selectCollection('app', 'users')->findOne(['_id' => (int) ($request->get['id'] ?? 0)]);
    $response->header('Content-Type', 'application/json');
    $response->end(json_encode($doc));
});

$server->start();
```

A `Client` is safe to share between coroutines. Concurrent operations use separate connections (up to `maxPoolSize`, default 100).
If you manage connections with an external pool library, give every pooled `Client` `maxPoolSize=1`.
See [examples/http_server.php](examples/http_server.php).

## Features

**Driver layer (C)**
- `Manager` and `Server`: `executeCommand`, `executeRead/Write/ReadWriteCommand`, `executeQuery`, `executeBulkWrite`, `executeBulkWriteCommand` (MongoDB 8).
- `Cursor` with `setTypeMap`, and `Session` with transactions and `withTransaction`.
- `ReadPreference`, `ReadConcern`, `WriteConcern`, `ServerApi`, `ServerDescription`, `TopologyDescription`.
- Monitoring with `CommandSubscriber`, `SDAMSubscriber` and every event, plus the exception hierarchy under `SwooleMongo\Driver\Exception\*`.

**BSON (C)**
- All BSON types (`ObjectId`, `UTCDateTime`, `Binary`, `Decimal128`, `Int64`, `Timestamp`, `Regex`, `Javascript`, `MinKey`, `MaxKey`) and `Document` / `PackedArray`.
- Extended JSON (canonical and relaxed), `Serializable` / `Unserializable` / `Persistable`, and typeMaps (root, document, array, fieldPaths).

**Library (embedded PHP, mongodb/mongodb API)**
- `Client`, `Database` and `Collection`: CRUD, bulkWrite, aggregate, countDocuments, distinct, findOneAndX, indexes and search indexes, explain, watch.
- `ChangeStream` with automatic resume, `GridFS\Bucket` with the `gridfs://` stream wrapper, and `ClientBulkWrite`.
- `Operation\*` and `Model\*`.

**Connectivity**
- `mongodb://` and `mongodb+srv://` URIs (`srvServiceName`, `srvMaxHosts`).
- Standalone, replica set, sharded and load-balanced deployments.
- Read preferences with tags, `maxStalenessSeconds` and the latency window.
- Retryable reads and writes, causal consistency, snapshot sessions, `timeoutMS` (CSOT), Stable API and unacknowledged writes with `moreToCome`.
- zlib, zstd and snappy compression, TLS, and SOCKS5 proxies.
- Topology monitoring in background coroutines, so requests never wait for a heartbeat.

## Authentication

| Mechanism | Notes |
|---|---|
| SCRAM-SHA-256, SCRAM-SHA-1 | Default. SASLprep with `ext-intl` |
| MONGODB-X509 | TLS client certificate |
| PLAIN | LDAP (Enterprise) |
| MONGODB-AWS | Credentials from the URI, environment, web identity, ECS or EC2 |
| MONGODB-OIDC | `oidcCallback` driver option or `ENVIRONMENT` (test, azure, gcp, k8s); re-authenticates on error 391 |
| GSSAPI (Kerberos) | Build with `--with-swoole-mongo-gssapi`. Uses the ticket cache (`kinit`) or a password in the URI. Supports `SERVICE_NAME`, `SERVICE_HOST`, `SERVICE_REALM` and `CANONICALIZE_HOST_NAME` |

```php
new Client('mongodb://app%40EXAMPLE.COM@db.example.com/?authMechanism=GSSAPI');
```

## Client-side field level encryption

Build with `--with-swoole-mongo-mongocrypt` to get CSFLE and Queryable Encryption.

```php
use SwooleMongo\BSON\Binary;
use SwooleMongo\Client;

$kms = ['local' => ['key' => new Binary($masterKey96Bytes)]];

// 1. Create a data key
$keyVault   = new Client($uri);
$encryption = $keyVault->createClientEncryption(['keyVaultNamespace' => 'encryption.__keyVault', 'kmsProviders' => $kms]);
$keyId      = $encryption->createDataKey('local');

// 2. Fields are encrypted and decrypted automatically
$client = new Client($uri, [], ['autoEncryption' => [
    'keyVaultNamespace'  => 'encryption.__keyVault',
    'kmsProviders'       => $kms,
    'encryptedFieldsMap' => ['app.patients' => ['fields' => [
        ['path' => 'ssn', 'bsonType' => 'string', 'keyId' => $keyId, 'queries' => ['queryType' => 'equality']],
    ]]],
    'extraOptions' => ['cryptSharedLibPath' => '/opt/mongo_crypt_v1.so'],
]]);

$patients = $client->selectCollection('app', 'patients');
$patients->insertOne(['name' => 'Ann', 'ssn' => '123-45-6789']);   // ssn is stored encrypted
$patients->findOne(['ssn' => '123-45-6789']);                       // and can still be queried
```

**Explicit encryption** with `ClientEncryption`:
- Data keys: `createDataKey`, `rewrapManyDataKey`, `deleteKey`.
- Encryption: `encrypt`, `encryptExpression`, `decrypt`.
- Key lookup: `getKey`, `getKeys`, `getKeyByAltName`, `addKeyAltName`, `removeKeyAltName`.
- Algorithms: Deterministic, Random, Indexed, Unindexed and Range.

**Automatic encryption** with the `autoEncryption` driver option:
- `schemaMap` for CSFLE, `encryptedFieldsMap` for Queryable Encryption.
- `keyVaultClient`, `bypassAutoEncryption`, `bypassQueryAnalysis` and `tlsOptions`.
- KMS providers: local, aws, azure, gcp and kmip.
- Query analysis with crypt_shared (`cryptSharedLibPath`) or mongocryptd (`mongocryptdURI`, `mongocryptdBypassSpawn`, `mongocryptdSpawnPath/Args`).

`Database::createEncryptedCollection()` creates the data keys and the Queryable Encryption state collections.
`dropCollection()` removes those state collections again.

## Configuration

`php.ini` settings:

| Setting | Default | Meaning |
|---|---|---|
| `swoole_mongo.enable_library` | `1` | Load the embedded `SwooleMongo\Client` library layer |
| `swoole_mongo.warn_blocking` | `1` | Warn when an operation runs in a coroutine while the runtime hooks are off, which would block |

Connection string options follow the [MongoDB URI specification](https://www.mongodb.com/docs/manual/reference/connection-string/).

## Performance

The heavy work runs in C: BSON, sockets, OP_MSG, compression, authentication, bulk splitting, cursors and topology. The embedded PHP layer adds about 1–1.5 µs per operation.

Benchmark against the official drivers for Node.js and Java (latest versions, each tuned for speed, same MongoDB server, median of 3 rounds with rotating order):

| Workload (ops/s, higher is better) | swoole-mongo | Node.js 22 | Java 26 (virtual threads) |
|---|---:|---:|---:|
| findOne, 100 concurrent | **27,475** | 15,636 | 12,379 |
| insertOne, 100 concurrent | **12,868** | 10,241 | 9,530 |
| find + iterate 5000 docs | **52** | 32 | 43 |
| insertMany 1000 docs | **84** | 53 | 41 |
| find 100 docs → array | **1,078** | 676 | 904 |
| findOne with index, sequential | **1,771** | 1,767 | 1,623 |
| insertOne, sequential | **710** | 680 | 586 |
| updateOne, sequential | 633 | **669** | 549 |

swoole-mongo is fastest for concurrent work (1.6–2.2× on findOne) and for bulk document processing (1.2–2×). Single sequential operations are practically a tie, because the network round trip dominates; the client-side work of swoole-mongo is only 5–18 µs per operation.

Details, settings and the scripts to reproduce it are in [bench/README.md](bench/README.md).

## Testing

```sh
docker compose up --build --exit-code-from tests --abort-on-container-exit tests
```

This command starts a full test environment:
- MongoDB 7 and 8 replica sets and a server with authentication;
- a sharded cluster behind HAProxy (load-balanced mode) and a SOCKS5 proxy;
- a Kerberos KDC with MongoDB Enterprise, and mongocryptd.

| Suite | Covers |
|---|---|
| `testsuite/run.php` | CRUD, BSON, transactions, change streams, GridFS, coroutines, authentication |
| `testsuite/features.php` | Stable API, `w:0`, `timeoutMS`, snapshot, SDAM, MongoDB 8 bulkWrite, load balancer, SOCKS5 |
| `testsuite/security.php` | Kerberos, CSFLE and Queryable Encryption (crypt_shared and mongocryptd) |
| `testsuite/leak.php`, `compression.php` | Memory and cursor leaks, wire compression |

GitHub Actions runs the same command on Linux x86_64 for every push.

## Limitations

- `tlsCRLFile` is not supported.
- ZTS (thread-safe) PHP builds are not supported.
- libkrb5 and KMS providers open their own sockets, which Swoole cannot hook. So the Kerberos ticket exchange (only when a new connection is opened) and KMS requests briefly block the worker.
- On arm64, Swoole's context switch does not preserve the floating-point registers `d8`–`d15`. The I/O code is therefore compiled with `-mgeneral-regs-only` and keeps all time in integer microseconds. This is handled for you.

## License

[MIT](LICENSE)
