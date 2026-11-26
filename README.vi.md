<div align="center">

# swoole-mongo

**Driver MongoDB non-blocking cho PHP và Swoole, viết bằng C.**

[![CI](https://github.com/nguyenthuan2412/mongo-swoole/actions/workflows/ci.yml/badge.svg)](https://github.com/nguyenthuan2412/mongo-swoole/actions/workflows/ci.yml)
![PHP](https://img.shields.io/badge/PHP-8.1%2B-777bb4)
![MongoDB](https://img.shields.io/badge/MongoDB-7.x%20%7C%208.x-47a248)
![License](https://img.shields.io/badge/license-MIT-blue)

[English](README.md) · **Tiếng Việt** · [Hướng dẫn sử dụng](docs/guide.vi.md)

</div>

---

swoole-mongo mang đầy đủ tính năng của MongoDB vào ứng dụng PHP mà không làm nghẽn event loop. Mỗi lần đọc/ghi socket
đều nhường cho coroutine khác của Swoole, nên một worker có thể chạy hàng nghìn query cùng lúc.

API giống bộ chính thức của PHP tới từng class. Nếu bạn đã quen `ext-mongodb` và `mongodb/mongodb` thì dùng swoole-mongo được ngay.

| Bộ chính thức của PHP | swoole-mongo |
|---|---|
| `MongoDB\Driver\*`, `MongoDB\BSON\*` (ext-mongodb) | `SwooleMongo\Driver\*`, `SwooleMongo\BSON\*` (C) |
| `MongoDB\Client`, `Database`, `Collection`, `GridFS\Bucket`, `Operation\*`, `Model\*` (mongodb/mongodb) | `SwooleMongo\Client`, … (PHP, **nhúng sẵn trong extension**) |

Chỉ cần đúng một file `swoole_mongo.so`. Không cần ext-mongodb, libmongoc/libbson hay package Composer nào.

## Mục lục

- [Vì sao cần swoole-mongo](#vì-sao-cần-swoole-mongo)
- [Yêu cầu](#yêu-cầu)
- [Cài đặt](#cài-đặt): [PIE](#bằng-pie-khuyên-dùng) · [PECL](#bằng-pecl) · [từ source](#build-từ-source) · [Docker](#docker)
- [Bắt đầu nhanh](#bắt-đầu-nhanh)
- [Dùng trong Swoole server](#dùng-trong-swoole-server)
- [Tính năng](#tính-năng)
- [Xác thực](#xác-thực)
- [Mã hoá field phía client](#mã-hoá-field-phía-client)
- [Cấu hình](#cấu-hình)
- [Hiệu năng](#hiệu-năng)
- [Test](#test)
- [Hạn chế](#hạn-chế)
- [Hướng dẫn sử dụng chi tiết](docs/guide.vi.md)
- [Giấy phép](#giấy-phép)

## Vì sao cần swoole-mongo

Driver chính thức được xây trên libmongoc. libmongoc tự chờ socket bằng `poll()` trên file descriptor gốc, nên khi chạy trong coroutine
nó chặn cả worker. Nó cũng không chịu được việc coroutine bị chuyển giữa chừng một thao tác.

swoole-mongo tự viết toàn bộ driver bằng C: BSON, giao thức OP_MSG, xác thực, dò và theo dõi server, quản lý connection và cursor.
Socket được mở bằng PHP stream, nên runtime hook của Swoole biến chúng thành non-blocking.

- **Trong coroutine** (`Co\run()`, Swoole server có `hook_flags`): I/O không chặn.
- **Ngoài coroutine** (php-fpm, script CLI): chạy đồng bộ như một driver bình thường.

## Yêu cầu

| | |
|---|---|
| PHP | 8.1 trở lên, bản NTS (không hỗ trợ ZTS). CI chạy PHP 8.3 |
| Hệ điều hành | Linux, macOS (x86_64 và arm64) |
| Swoole | 5.x để có I/O non-blocking (không bắt buộc) |
| zlib | bắt buộc |
| Tuỳ chọn | `ext-intl` (SASLprep cho mật khẩu có ký tự ngoài ASCII), `ext-zstd` (nén zstd), MIT Kerberos (GSSAPI), libmongocrypt ≥ 1.8 (CSFLE / Queryable Encryption) |

## Cài đặt

### Bằng PIE (khuyên dùng)

[PIE](https://github.com/php/pie) là công cụ cài extension chính thức của PHP. Nó tự build extension và bật trong `php.ini` giúp bạn.

```sh
pie install nguyenthuan2412/swoole-mongo
```

Muốn bật các tính năng tuỳ chọn thì thêm cờ tương ứng. Các cờ này cần những thư viện liệt kê ở mục [Yêu cầu](#yêu-cầu).

```sh
pie install nguyenthuan2412/swoole-mongo --with-swoole-mongo-gssapi --with-swoole-mongo-mongocrypt
```

<details>
<summary>Cài thẳng từ GitHub (khi package chưa lên Packagist, hoặc muốn dùng bản đang phát triển)</summary>

```sh
pie repository:add vcs https://github.com/nguyenthuan2412/mongo-swoole
pie install nguyenthuan2412/swoole-mongo:dev-main
```
</details>

### Bằng PECL

```sh
# từ file release trên GitHub
pecl install https://github.com/nguyenthuan2412/mongo-swoole/releases/download/v0.2.0/swoole_mongo-0.2.0.tgz

# từ file đã tải về
pecl install swoole_mongo-0.2.0.tgz
```

PECL sẽ hỏi có bật Kerberos và libmongocrypt không. Cứ Enter để giữ `auto`: thư viện nào đã cài trên máy thì tự được bật.
Sau đó thêm extension vào `php.ini`:

```ini
extension=swoole_mongo
```

### Build từ source

```sh
git clone https://github.com/nguyenthuan2412/mongo-swoole.git && cd mongo-swoole
./build.sh                                   # phpize + configure + make
sudo make install
echo "extension=swoole_mongo" | sudo tee "$(php -r 'echo PHP_CONFIG_FILE_SCAN_DIR;')/50-swoole_mongo.ini"
```

Tuỳ chọn khi build (đều không bắt buộc; `auto` tự bật tính năng nếu tìm thấy thư viện):

| Tuỳ chọn | Cần | Bật tính năng |
|---|---|---|
| `--with-swoole-mongo-gssapi[=DIR]` | MIT Kerberos (`libkrb5-dev`, `brew install krb5`) | `authMechanism=GSSAPI` |
| `--with-swoole-mongo-mongocrypt[=DIR]` | libmongocrypt ≥ 1.8 (tìm qua pkg-config hoặc truyền prefix) | `ClientEncryption`, `autoEncryption` |

```sh
./build.sh --with-swoole-mongo-gssapi --with-swoole-mongo-mongocrypt=/opt/libmongocrypt
```

Kiểm tra đã cài thành công chưa:

```sh
php --ri swoole_mongo
```

### Docker

`Dockerfile` đi kèm build sẵn PHP 8.3 với Swoole, swoole-mongo, zstd, intl, Kerberos, libmongocrypt và crypt_shared:

```sh
docker build -t swoole-mongo .
```

## Bắt đầu nhanh

```php
<?php
use SwooleMongo\Client;
use SwooleMongo\Operation\FindOneAndUpdate;

Co\run(function () {
    $client = new Client('mongodb://localhost:27017/?replicaSet=rs0');
    $users  = $client->selectCollection('app', 'users');

    // Thêm
    $id = $users->insertOne(['name' => 'An', 'age' => 30])->getInsertedId();

    // Đọc
    $an = $users->findOne(['_id' => $id]);           // BSONDocument: $an->name hoặc $an['name']
    foreach ($users->find(['age' => ['$gte' => 18]], ['sort' => ['age' => -1], 'limit' => 10]) as $user) {
        echo $user->name, PHP_EOL;
    }

    // Sửa
    $users->updateOne(['_id' => $id], ['$inc' => ['age' => 1]]);
    $after = $users->findOneAndUpdate(
        ['_id' => $id],
        ['$set' => ['vip' => true]],
        ['returnDocument' => FindOneAndUpdate::RETURN_DOCUMENT_AFTER],
    );

    // Transaction
    $session = $client->startSession();
    $session->withTransaction(function ($session) use ($users) {
        $users->updateOne(['name' => 'An'], ['$set' => ['level' => 2]], ['session' => $session]);
    });

    // Các query chạy song song: 10 coroutine dưới đây dùng chung một worker
    for ($i = 0; $i < 10; $i++) {
        go(fn () => $users->countDocuments(['age' => ['$gt' => $i]]));
    }
});
```

Code viết cho `mongodb/mongodb` thường chỉ cần đổi namespace `MongoDB\` thành `SwooleMongo\` là chạy.

📖 **[Hướng dẫn sử dụng](docs/guide.vi.md) giải thích chi tiết từng chủ đề.** Tài liệu đi từ kết nối, truy vấn tới transaction, change stream, GridFS, xử lý lỗi, chạy song song và khắc phục sự cố.

## Dùng trong Swoole server

Tạo một `Client` cho mỗi worker trong `workerStart` và bật runtime hook:

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

Một `Client` có thể dùng chung giữa các coroutine một cách an toàn. Các thao tác song song dùng connection riêng (tối đa `maxPoolSize`, mặc định 100).
Nếu bạn quản lý connection bằng thư viện pool bên ngoài, hãy đặt `maxPoolSize=1` cho mỗi `Client` trong pool.
Xem thêm [examples/http_server.php](examples/http_server.php).

## Tính năng

**Tầng driver (C)**
- `Manager` và `Server`: `executeCommand`, `executeRead/Write/ReadWriteCommand`, `executeQuery`, `executeBulkWrite`, `executeBulkWriteCommand` (MongoDB 8).
- `Cursor` có `setTypeMap`; `Session` có transaction và `withTransaction`.
- `ReadPreference`, `ReadConcern`, `WriteConcern`, `ServerApi`, `ServerDescription`, `TopologyDescription`.
- Giám sát bằng `CommandSubscriber`, `SDAMSubscriber` với đầy đủ event, cùng hệ exception `SwooleMongo\Driver\Exception\*`.

**BSON (C)**
- Đủ mọi kiểu BSON (`ObjectId`, `UTCDateTime`, `Binary`, `Decimal128`, `Int64`, `Timestamp`, `Regex`, `Javascript`, `MinKey`, `MaxKey`) và `Document` / `PackedArray`.
- Extended JSON (canonical và relaxed), `Serializable` / `Unserializable` / `Persistable`, cùng typeMap (root, document, array, fieldPaths).

**Library (PHP nhúng sẵn, API của mongodb/mongodb)**
- `Client`, `Database`, `Collection`: CRUD, bulkWrite, aggregate, countDocuments, distinct, findOneAndX, index và search index, explain, watch.
- `ChangeStream` tự nối lại khi lỗi, `GridFS\Bucket` kèm stream wrapper `gridfs://`, và `ClientBulkWrite`.
- `Operation\*` và `Model\*`.

**Kết nối**
- URI `mongodb://` và `mongodb+srv://` (`srvServiceName`, `srvMaxHosts`).
- Server đơn, replica set, sharded cluster và chế độ load balanced.
- Read preference có tags, `maxStalenessSeconds` và cửa sổ độ trễ.
- Retryable read/write, causal consistency, snapshot session, `timeoutMS` (CSOT), Stable API, ghi không chờ phản hồi bằng `moreToCome`.
- Nén zlib, zstd, snappy; TLS; SOCKS5 proxy.
- Theo dõi topology bằng coroutine chạy nền, nên request không bao giờ phải chờ heartbeat.

## Xác thực

| Cơ chế | Ghi chú |
|---|---|
| SCRAM-SHA-256, SCRAM-SHA-1 | Mặc định. SASLprep cần `ext-intl` |
| MONGODB-X509 | Chứng chỉ TLS phía client |
| PLAIN | LDAP (bản Enterprise) |
| MONGODB-AWS | Credentials lấy từ URI, biến môi trường, web identity, ECS hoặc EC2 |
| MONGODB-OIDC | Driver option `oidcCallback` hoặc `ENVIRONMENT` (test, azure, gcp, k8s); tự xác thực lại khi gặp lỗi 391 |
| GSSAPI (Kerberos) | Build với `--with-swoole-mongo-gssapi`. Dùng ticket có sẵn (`kinit`) hoặc mật khẩu trong URI. Hỗ trợ `SERVICE_NAME`, `SERVICE_HOST`, `SERVICE_REALM`, `CANONICALIZE_HOST_NAME` |

```php
new Client('mongodb://app%40EXAMPLE.COM@db.example.com/?authMechanism=GSSAPI');
```

## Mã hoá field phía client

Build với `--with-swoole-mongo-mongocrypt` để có CSFLE và Queryable Encryption.

```php
use SwooleMongo\BSON\Binary;
use SwooleMongo\Client;

$kms = ['local' => ['key' => new Binary($masterKey96Bytes)]];

// 1. Tạo data key
$keyVault   = new Client($uri);
$encryption = $keyVault->createClientEncryption(['keyVaultNamespace' => 'encryption.__keyVault', 'kmsProviders' => $kms]);
$keyId      = $encryption->createDataKey('local');

// 2. Field được tự động mã hoá khi ghi và giải mã khi đọc
$client = new Client($uri, [], ['autoEncryption' => [
    'keyVaultNamespace'  => 'encryption.__keyVault',
    'kmsProviders'       => $kms,
    'encryptedFieldsMap' => ['app.patients' => ['fields' => [
        ['path' => 'cccd', 'bsonType' => 'string', 'keyId' => $keyId, 'queries' => ['queryType' => 'equality']],
    ]]],
    'extraOptions' => ['cryptSharedLibPath' => '/opt/mongo_crypt_v1.so'],
]]);

$patients = $client->selectCollection('app', 'patients');
$patients->insertOne(['name' => 'An', 'cccd' => '001099012345']);   // cccd được lưu ở dạng mã hoá
$patients->findOne(['cccd' => '001099012345']);                      // nhưng vẫn truy vấn được
```

**Mã hoá thủ công** bằng `ClientEncryption`:
- Data key: `createDataKey`, `rewrapManyDataKey`, `deleteKey`.
- Mã hoá: `encrypt`, `encryptExpression`, `decrypt`.
- Tìm key: `getKey`, `getKeys`, `getKeyByAltName`, `addKeyAltName`, `removeKeyAltName`.
- Thuật toán: Deterministic, Random, Indexed, Unindexed và Range.

**Mã hoá tự động** bằng driver option `autoEncryption`:
- `schemaMap` cho CSFLE, `encryptedFieldsMap` cho Queryable Encryption.
- `keyVaultClient`, `bypassAutoEncryption`, `bypassQueryAnalysis` và `tlsOptions`.
- KMS provider: local, aws, azure, gcp và kmip.
- Phân tích query bằng crypt_shared (`cryptSharedLibPath`) hoặc mongocryptd (`mongocryptdURI`, `mongocryptdBypassSpawn`, `mongocryptdSpawnPath/Args`).

`Database::createEncryptedCollection()` tạo data key và các collection trạng thái của Queryable Encryption.
`dropCollection()` xoá luôn các collection trạng thái đó.

## Cấu hình

Thiết lập trong `php.ini`:

| Thiết lập | Mặc định | Ý nghĩa |
|---|---|---|
| `swoole_mongo.enable_library` | `1` | Nạp tầng library nhúng sẵn (`SwooleMongo\Client`…) |
| `swoole_mongo.warn_blocking` | `1` | Cảnh báo khi chạy trong coroutine mà chưa bật runtime hook (lúc đó lệnh sẽ chặn worker) |

Tham số trong connection string theo [đặc tả URI của MongoDB](https://www.mongodb.com/docs/manual/reference/connection-string/).

## Hiệu năng

Phần nặng đều chạy bằng C: BSON, socket, OP_MSG, nén, xác thực, chia batch cho bulk, cursor và topology. Tầng PHP nhúng chỉ tốn khoảng 1–1,5 µs mỗi thao tác.

Benchmark so với driver chính thức của Node.js và Java (bản mới nhất, mỗi bên dùng cấu hình nhanh nhất, cùng một server MongoDB, trung vị của 3 vòng có đổi thứ tự):

| Workload (ops/s, càng cao càng tốt) | swoole-mongo | Node.js 22 | Java 26 (virtual thread) |
|---|---:|---:|---:|
| findOne, 100 đồng thời | **27,475** | 15,636 | 12,379 |
| insertOne, 100 đồng thời | **12,868** | 10,241 | 9,530 |
| find + duyệt 5000 doc | **52** | 32 | 43 |
| insertMany 1000 doc | **84** | 53 | 41 |
| find 100 doc → mảng | **1,078** | 676 | 904 |
| findOne có index, tuần tự | **1,771** | 1,767 | 1,623 |
| insertOne, tuần tự | **710** | 680 | 586 |
| updateOne, tuần tự | 633 | **669** | 549 |

swoole-mongo nhanh nhất khi chạy đồng thời (gấp 1,6–2,2 lần với findOne) và khi xử lý nhiều document (gấp 1,2–2 lần). Thao tác tuần tự đơn lẻ gần như ngang nhau vì thời gian chủ yếu nằm ở round trip mạng; phần việc phía client của swoole-mongo chỉ tốn 5–18 µs mỗi thao tác.

Chi tiết cấu hình và script để tự chạy lại nằm trong [bench/README.vi.md](bench/README.vi.md).

## Test

```sh
docker compose up --build --exit-code-from tests --abort-on-container-exit tests
```

Lệnh trên dựng đủ môi trường test:
- replica set MongoDB 7 và 8, cùng một server có xác thực;
- sharded cluster sau HAProxy (chế độ load balanced) và một SOCKS5 proxy;
- KDC Kerberos kèm MongoDB Enterprise, và mongocryptd.

| Bộ test | Nội dung |
|---|---|
| `testsuite/run.php` | CRUD, BSON, transaction, change stream, GridFS, coroutine, xác thực |
| `testsuite/features.php` | Stable API, `w:0`, `timeoutMS`, snapshot, SDAM, bulkWrite của MongoDB 8, load balancer, SOCKS5 |
| `testsuite/security.php` | Kerberos, CSFLE và Queryable Encryption (crypt_shared và mongocryptd) |
| `testsuite/leak.php`, `compression.php` | Rò rỉ bộ nhớ và cursor, nén dữ liệu |

GitHub Actions chạy cùng lệnh này trên Linux x86_64 mỗi lần push.

## Hạn chế

- Chưa hỗ trợ `tlsCRLFile`.
- Không hỗ trợ PHP bản ZTS (thread-safe).
- libkrb5 và KMS provider tự mở socket riêng, Swoole không hook được. Vì vậy bước xin ticket Kerberos (chỉ khi mở connection mới) và các request tới KMS sẽ chặn worker một chút.
- Trên arm64, context switch của Swoole không giữ lại các thanh ghi số thực `d8`–`d15`. Vì thế phần code I/O được build với `-mgeneral-regs-only` và mọi phép tính thời gian dùng số nguyên micro giây. Phần này đã được xử lý sẵn, bạn không cần làm gì.

## Giấy phép

[MIT](LICENSE)
