# swmongo — MongoDB extension cho PHP/Swoole (non-blocking)

Extension C cho PHP ≥ 8.1. Mọi thao tác I/O tự nhường cho coroutine khác khi chạy trong Swoole.
Cách tổ chức class giống hệt bộ chính thức **ext-mongodb + mongodb/mongodb**, nhưng nằm trong namespace `SwooleMongo`:

| Bộ chính thức | swmongo |
|---|---|
| `MongoDB\Driver\*` (ext-mongodb) | `SwooleMongo\Driver\*` (C) |
| `MongoDB\BSON\*` | `SwooleMongo\BSON\*` (C) |
| `MongoDB\Client`, `Database`, `Collection`, `GridFS\Bucket`, `Operation\*`, `Model\*` (mongodb/mongodb) | `SwooleMongo\Client`, … (PHP, **nhúng sẵn trong file .so**) |

Chỉ cần cài một file `swmongo.so`. Không cần ext-mongodb, libmongoc, libbson hay composer.

## Vì sao không dùng libmongoc

libmongoc tự chờ socket bằng `poll()` trên file descriptor gốc, và không chịu được việc một coroutine yield giữa chừng một
lệnh trên `mongoc_client_t`. Vì vậy swmongo tự viết toàn bộ phần lõi bằng C: BSON, OP_MSG, xác thực, topology, pool, cursor.
Socket được mở qua `php_stream_xport_create()`. Khi Swoole bật hook (`SWOOLE_HOOK_ALL`, mặc định trong `Co\run`, hoặc
`'hook_flags' => SWOOLE_HOOK_ALL` cho server), mỗi lần đọc/ghi socket sẽ tự yield. Ngoài coroutine, extension chạy đồng bộ như bình thường.

## Build & cài đặt

```sh
./build.sh          # nhúng library/*.php rồi chạy phpize + configure + make
sudo make install
# php.ini (đặt sau extension=swoole):
extension=swmongo
```

Tuỳ chọn khi build:

| Tuỳ chọn | Cần | Bật tính năng |
|---|---|---|
| `--with-swmongo-gssapi[=DIR]` | MIT Kerberos (`libkrb5-dev`, hoặc `brew install krb5`) | `authMechanism=GSSAPI` |
| `--with-swmongo-mongocrypt[=DIR]` | libmongocrypt ≥ 1.8 (tìm qua pkg-config, có thể truyền prefix) | `ClientEncryption`, `autoEncryption` (CSFLE + Queryable Encryption) |

Mặc định (`auto`), mỗi thư viện có trên máy sẽ tự được bật.

```sh
./build.sh --with-swmongo-gssapi --with-swmongo-mongocrypt=/opt/libmongocrypt
```

Hoặc dùng Docker: `docker build -t swmongo .`. Image gồm PHP 8.3, Swoole 5.1 và swmongo, kèm zstd/intl, Kerberos, libmongocrypt và crypt_shared.

INI: `swmongo.enable_library=1` (nạp tầng library nhúng sẵn) và `swmongo.warn_blocking=1` (cảnh báo khi chạy trong coroutine mà chưa bật hook).

## Ví dụ

```php
use SwooleMongo\Client;
use SwooleMongo\Operation\FindOneAndUpdate;

Co\run(function () {
    $client = new Client('mongodb://host1,host2/?replicaSet=rs0');
    $users = $client->selectCollection('app', 'users');

    $id = $users->insertOne(['name' => 'Ann', 'age' => 30])->getInsertedId();
    $doc = $users->findOne(['_id' => $id]);                 // BSONDocument: $doc->name, $doc['age']
    foreach ($users->find(['age' => ['$gte' => 18]], ['sort' => ['age' => -1], 'limit' => 10]) as $u) { /* … */ }
    $users->updateOne(['_id' => $id], ['$inc' => ['age' => 1]]);
    $after = $users->findOneAndUpdate(['_id' => $id], ['$set' => ['x' => 1]],
        ['returnDocument' => FindOneAndUpdate::RETURN_DOCUMENT_AFTER]);

    $session = $client->startSession();
    $session->withTransaction(function ($s) use ($users) {
        $users->updateOne(['name' => 'Ann'], ['$set' => ['vip' => true]], ['session' => $s]);
    });
});
```

Trong Swoole server: tạo `Client` trong `workerStart` (mỗi worker một client) và bật `'hook_flags' => SWOOLE_HOOK_ALL`.
Xem [examples/http_server.php](examples/http_server.php). Nếu bạn quản lý pool bằng thư viện ngoài, nên đặt `maxPoolSize=1` cho mỗi `Client` trong pool.

## Tính năng

- **Tầng Driver (C)**:
  - `Manager`, `Server`: `executeCommand`, `executeReadCommand`, `executeWriteCommand`, `executeReadWriteCommand`, `executeQuery`, `executeBulkWrite`, `executeBulkWriteCommand`.
  - `Command`, `Query`, `BulkWrite`, `BulkWriteCommand` (MongoDB 8), `WriteResult`, `WriteError`, `WriteConcernError`.
  - `Cursor`, có `setTypeMap`.
  - `Session` với transaction và `withTransaction`; các class `ReadPreference`, `ReadConcern`, `WriteConcern`, `ServerApi`, `ServerDescription`, `TopologyDescription`.
  - Monitoring: `CommandSubscriber`, `SDAMSubscriber` cùng toàn bộ event.
  - Exception: `SwooleMongo\Driver\Exception\*`.
- **BSON (C)**:
  - `ObjectId`, `UTCDateTime`, `Binary`, `Decimal128`, `Int64`, `Timestamp`, `Regex`, `Javascript`, `MinKey`, `MaxKey`.
  - `Document`, `PackedArray` (ArrayAccess, toPHP, Extended JSON canonical/relaxed, fromJSON).
  - Interface `Serializable`/`Unserializable`/`Persistable` (`__pclass`) và typeMap (root/document/array/fieldPaths).
- **Library (PHP nhúng sẵn)**:
  - `Client`, `Database`, `Collection` (CRUD, bulkWrite, aggregate, countDocuments, distinct, findOneAndX, quản lý index và search index, explain, watch).
  - `ChangeStream` (tự resume), `GridFS\Bucket` (stream `gridfs://`), `ClientBulkWrite`.
  - `Operation\*`, `Model\*` (BSONDocument, BSONArray, IndexInfo…), `Functions::withTransaction`.
- **Kết nối**:
  - `mongodb://`, `mongodb+srv://` (kèm `srvServiceName`, `srvMaxHosts`).
  - Standalone / replica set / sharded / load balanced (`loadBalanced=true`, cursor và transaction gắn cố định vào một connection).
  - Read preference đầy đủ: tags, `maxStalenessSeconds`, cửa sổ latency.
  - Retryable reads/writes, causal consistency, snapshot session, `timeoutMS` (CSOT), `serverApi`, `w:0` với `moreToCome`.
  - Nén zlib (zstd/snappy khi có cài ext tương ứng), TLS, SOCKS5 proxy.
  - Theo dõi topology song song bằng coroutine; refresh định kỳ chạy nền nên request không bị chặn.
- **Xác thực**: SCRAM-SHA-1, SCRAM-SHA-256 (SASLprep), PLAIN, MONGODB-X509, MONGODB-AWS (SigV4, lấy credentials từ URI/env/web identity/ECS/EC2),
  MONGODB-OIDC (driver option `oidcCallback` hoặc `ENVIRONMENT` test/azure/gcp/k8s, tự xác thực lại khi gặp lỗi 391), GSSAPI (Kerberos).
  GSSAPI hỗ trợ `SERVICE_NAME`, `SERVICE_HOST`, `SERVICE_REALM`, `CANONICALIZE_HOST_NAME`. Có thể lấy ticket bằng `kinit` (qua `KRB5CCNAME`) hoặc truyền mật khẩu ngay trong URI.
  Lưu ý: libkrb5 tự mở socket tới KDC (Swoole không hook được), nên bước xin ticket sẽ chặn worker một chút. Việc này chỉ xảy ra khi mở connection mới, không phải ở mỗi lệnh.
- **Mã hoá phía client (libmongocrypt)**:
  - `ClientEncryption`: `createDataKey`, `encrypt`, `encryptExpression`, `decrypt`, `rewrapManyDataKey`, `getKey(s)`, `getKeyByAltName`,
    `add/removeKeyAltName`, `deleteKey`; thuật toán Deterministic/Random, `Indexed`/`Unindexed`/`Range`.
  - Tự động mã hoá/giải mã qua driver option `autoEncryption`: `keyVaultNamespace`, `keyVaultClient`, `kmsProviders` (local/aws/azure/gcp/kmip),
    `schemaMap`, `encryptedFieldsMap`, `bypassAutoEncryption`, `bypassQueryAnalysis`, `tlsOptions`. Phân tích query dùng
    `extraOptions.cryptSharedLibPath` (crypt_shared) hoặc mongocryptd (`mongocryptdURI`, `mongocryptdBypassSpawn`, `mongocryptdSpawnPath/Args`).
  - `Database::createEncryptedCollection()`. `createCollection`/`dropCollection` tự tạo và xoá collection trạng thái `enxcol_.*.esc/ecoc` của Queryable Encryption.
  - Các lệnh tới key vault, mongocryptd và `listCollections` đều chạy qua driver nên cũng non-blocking.
    Riêng request tới KMS từ xa (TLS) chỉ non-blocking khi Swoole hook SSL (`SWOOLE_HOOK_ALL` đã gồm).

```php
$client = new SwooleMongo\Client($uri, [], ['autoEncryption' => [
    'keyVaultNamespace' => 'encryption.__keyVault',
    'kmsProviders' => ['local' => ['key' => new SwooleMongo\BSON\Binary($key96bytes)]],
    'extraOptions' => ['cryptSharedLibPath' => '/opt/crypt/mongo_crypt_v1.so'],
]]);
```

**Chưa hỗ trợ**: `tlsCRLFile`.

## Hiệu năng

- Phần nặng viết bằng C: BSON, socket, OP_MSG, nén, xác thực, chia batch cho bulk, cursor, topology.
- Tầng PHP nhúng chỉ tốn khoảng 1–1,5 µs mỗi lệnh. Một vòng request tới MongoDB mất khoảng 600 µs, nên phần chênh nằm trong nhiễu đo.

## Benchmark so với Node.js

Xem [bench/README.md](bench/README.md). Cùng workload trên cùng server MongoDB, swmongo nhanh hơn driver Node.js chính thức từ 1,0 tới 1,5 lần ở thao tác tuần tự, và khoảng 2,1 lần với `findOne` chạy 100 coroutine đồng thời.

## Lưu ý về Swoole trên arm64

Context switch của Swoole trên arm64 không bảo toàn các thanh ghi số thực callee-saved `d8`–`d15`. Vì vậy các file C có I/O được biên dịch với
`-mgeneral-regs-only`, và mọi phép tính thời gian dùng số nguyên micro giây. Trên x86_64 không có vấn đề này.

## Test

```sh
docker compose up --build --exit-code-from tests --abort-on-container-exit tests
```

Lệnh trên dựng đủ môi trường: replica set MongoDB 7 và 8, server có auth, sharded cluster sau HAProxy, SOCKS5 proxy, KDC Kerberos, MongoDB Enterprise (GSSAPI) và mongocryptd. Sau đó nó chạy:
- `testsuite/run.php`: CRUD, BSON, transaction, change stream, GridFS, coroutine, auth.
- `testsuite/features.php`: serverApi, w:0, timeoutMS, snapshot, SDAM, bulkWrite cho MongoDB 8, load balancer, SOCKS5.
- `testsuite/leak.php`: kiểm tra bộ nhớ và cursor bị bỏ dở.
- `testsuite/compression.php`: kiểm tra nén.
- `testsuite/security.php`: Kerberos (KDC + MongoDB Enterprise), CSFLE/QE (crypt_shared và mongocryptd).

GitHub Actions (`.github/workflows/ci.yml`) chạy cùng lệnh này trên Linux x86_64.
