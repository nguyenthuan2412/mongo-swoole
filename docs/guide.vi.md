# Hướng dẫn sử dụng swoole-mongo

[English](guide.md) · **Tiếng Việt** · [Về README](../README.vi.md)

Tài liệu này hướng dẫn dùng swoole-mongo trong công việc hằng ngày: từ kết nối đầu tiên tới transaction, change stream, GridFS,
xử lý lỗi và các mẫu triển khai với Swoole server. Mọi ví dụ đều dùng API bậc cao `SwooleMongo\Client`, giống hệt
[mongodb/mongodb](https://www.mongodb.com/docs/php-library/current/). Code viết cho thư viện đó thường chỉ cần đổi
namespace `MongoDB\` thành `SwooleMongo\` là chạy.

1. [Chạy code trong coroutine](#1-chạy-code-trong-coroutine)
2. [Kết nối](#2-kết-nối)
3. [Database và collection](#3-database-và-collection)
4. [Thêm document](#4-thêm-document)
5. [Truy vấn](#5-truy-vấn)
6. [Cập nhật và thay thế](#6-cập-nhật-và-thay-thế)
7. [Xoá](#7-xoá)
8. [Ghi hàng loạt (bulk write)](#8-ghi-hàng-loạt-bulk-write)
9. [Aggregation, đếm và distinct](#9-aggregation-đếm-và-distinct)
10. [Index](#10-index)
11. [Kiểu BSON và typeMap](#11-kiểu-bson-và-typemap)
12. [Session và transaction](#12-session-và-transaction)
13. [Change stream](#13-change-stream)
14. [GridFS (lưu file)](#14-gridfs-lưu-file)
15. [Read/write concern, read preference, timeout](#15-readwrite-concern-read-preference-timeout)
16. [Xử lý lỗi](#16-xử-lý-lỗi)
17. [Chạy nhiều thao tác cùng lúc](#17-chạy-nhiều-thao-tác-cùng-lúc)
18. [Swoole server và connection pool](#18-swoole-server-và-connection-pool)
19. [Ví dụ xác thực](#19-ví-dụ-xác-thực)
20. [Giám sát và ghi log](#20-giám-sát-và-ghi-log)
21. [API driver cấp thấp](#21-api-driver-cấp-thấp)
22. [Khắc phục sự cố](#22-khắc-phục-sự-cố)

---

## 1. Chạy code trong coroutine

swoole-mongo chỉ non-blocking khi chạy trong coroutine của Swoole và đã bật runtime hook:

```php
// Script CLI: Co\run() tạo coroutine và tự bật SWOOLE_HOOK_ALL
Co\run(function () {
    $client = new SwooleMongo\Client('mongodb://localhost:27017');
    // ...
});
```

```php
// Swoole server: bật hook trong phần cấu hình server
$server->set(['hook_flags' => SWOOLE_HOOK_ALL]);
```

Khi không có Swoole (php-fpm, CLI thường), cùng đoạn code đó vẫn chạy được, chỉ là chạy đồng bộ như mọi driver khác.
Nếu gọi driver trong coroutine mà chưa bật hook, swoole-mongo sẽ in cảnh báo, vì khi đó mỗi lệnh sẽ chặn cả worker.
Muốn tắt cảnh báo này thì đặt `swoole_mongo.warn_blocking=0`.

## 2. Kết nối

```php
use SwooleMongo\Client;

$client = new Client('mongodb://localhost:27017');                        // server đơn
$client = new Client('mongodb://h1:27017,h2:27017/?replicaSet=rs0');      // replica set
$client = new Client('mongodb+srv://user:pass@cluster0.example.net/');    // Atlas / DNS seed list
$client = new Client('mongodb://user:pass@localhost/?authSource=admin');  // có tài khoản
```

Hàm khởi tạo nhận ba tham số: `new Client(?string $uri, array $uriOptions = [], array $driverOptions = [])`.

```php
$client = new Client('mongodb://localhost', [
    // URI option: giống các tham số viết trong connection string
    'appname'                  => 'billing-api',
    'maxPoolSize'              => 50,
    'serverSelectionTimeoutMS' => 5000,
    'retryWrites'              => true,
    'compressors'              => 'zstd,zlib',
    'tls'                      => true,
    'tlsCAFile'                => '/etc/ssl/mongo-ca.pem',
], [
    // driver option
    'typeMap'   => ['root' => 'array', 'document' => 'array', 'array' => 'array'],
    'serverApi' => new SwooleMongo\Driver\ServerApi(SwooleMongo\Driver\ServerApi::V1),
]);
```

Kết nối diễn ra "lười": chưa có gì được gửi đi cho tới thao tác đầu tiên. Một `Client` giữ lâu không tốn gì và dùng chung giữa các coroutine an toàn,
nên hãy tạo một lần rồi tái sử dụng. Đừng tạo mới cho mỗi request.

| URI option hay dùng | Mặc định | Ý nghĩa |
|---|---|---|
| `maxPoolSize` | 100 | Số connection tối đa tới mỗi server (= số thao tác đồng thời tối đa trên mỗi server) |
| `serverSelectionTimeoutMS` | 30000 | Thời gian chờ tìm được server phù hợp |
| `connectTimeoutMS` | 10000 | Timeout khi mở kết nối TCP / TLS |
| `socketTimeoutMS` | 0 (không giới hạn) | Timeout cho mỗi lần đọc/ghi |
| `timeoutMS` | không | Giới hạn tổng thời gian của mỗi thao tác (CSOT) |
| `heartbeatFrequencyMS` | 10000 | Chu kỳ làm mới thông tin topology |
| `readPreference` | primary | `primaryPreferred`, `secondary`, `secondaryPreferred`, `nearest` |
| `w`, `journal`, `wtimeoutMS` | mặc định của server | Write concern |
| `readConcernLevel` | mặc định của server | `local`, `majority`, `snapshot`… |
| `retryReads`, `retryWrites` | true | Tự thử lại một lần khi lỗi mạng hoặc khi đổi primary |
| `compressors` | không | `zstd`, `zlib`, `snappy` |
| `directConnection` | false | Chỉ nói chuyện với đúng một host, không dò cả replica set |
| `loadBalanced` | false | Kết nối tới mongos qua load balancer |
| `proxyHost`, `proxyPort`, `proxyUsername`, `proxyPassword` | không | Đi qua SOCKS5 proxy |

## 3. Database và collection

```php
$db    = $client->selectDatabase('shop');          // hoặc $client->shop
$items = $db->selectCollection('items');           // hoặc $db->items, hoặc $client->selectCollection('shop', 'items')

$db->createCollection('logs', ['capped' => true, 'size' => 10 * 1024 * 1024]);
foreach ($db->listCollectionNames() as $name) echo $name, PHP_EOL;
foreach ($client->listDatabaseNames() as $name) echo $name, PHP_EOL;

$db->renameCollection('logs', 'logs_old');
$db->dropCollection('logs_old');
$db->command(['ping' => 1])->toArray();           // chạy bất kỳ lệnh database nào
```

`withOptions()` trả về một bản sao với cấu hình mặc định khác. Object gốc không bị thay đổi:

```php
use SwooleMongo\Driver\ReadPreference;
use SwooleMongo\Driver\WriteConcern;

$fastWrites  = $items->withOptions(['writeConcern' => new WriteConcern(1)]);
$fromReplica = $items->withOptions(['readPreference' => new ReadPreference(ReadPreference::SECONDARY_PREFERRED)]);
```

## 4. Thêm document

```php
$result = $items->insertOne(['sku' => 'A-1', 'name' => 'Bút', 'price' => 1.5, 'tags' => ['office']]);
$id = $result->getInsertedId();                    // SwooleMongo\BSON\ObjectId do driver tự sinh

$result = $items->insertMany([
    ['sku' => 'A-2', 'name' => 'Bút chì'],
    ['sku' => 'A-3', 'name' => 'Tẩy'],
]);
$result->getInsertedCount();                        // 2
$result->getInsertedIds();                          // [0 => ObjectId, 1 => ObjectId]
```

- Muốn dùng mã riêng thì tự đặt `_id`: `['_id' => 'A-1', ...]`.
- `insertMany` tự chia dữ liệu lớn thành nhiều batch, nên một lần gọi với 100 000 document vẫn chạy được.
- `['ordered' => false]` sẽ tiếp tục ghi khi gặp lỗi (ví dụ trùng khoá), rồi báo tất cả lỗi ở cuối.

## 5. Truy vấn

```php
$pen = $items->findOne(['sku' => 'A-1']);           // trả về document hoặc null
echo $pen->name;                                    // mặc định là BSONDocument: dùng như object hoặc như mảng
echo $pen['price'];

$cursor = $items->find(
    ['price' => ['$gte' => 1, '$lt' => 10], 'tags' => 'office'],
    [
        'projection' => ['name' => 1, 'price' => 1, '_id' => 0],
        'sort'       => ['price' => -1],
        'skip'       => 20,
        'limit'      => 10,
        'batchSize'  => 100,
        'collation'  => ['locale' => 'vi', 'strength' => 2],
        'hint'       => ['price' => 1],
        'maxTimeMS'  => 2000,
        'comment'    => 'trang danh sách sản phẩm',
    ],
);

foreach ($cursor as $doc) {
    echo $doc->name, ' ', $doc->price, PHP_EOL;
}
```

- Cursor chỉ lấy batch tiếp theo từ server khi bạn duyệt hết batch hiện tại. Trong lúc chờ, nó nhường cho coroutine khác chạy.
- `$cursor->toArray()` đọc toàn bộ kết quả vào một mảng. Chỉ nên dùng khi kết quả nhỏ.
- Mỗi cursor chỉ duyệt được một lần. Muốn đọc lại thì chạy lại query.

**Toán tử truy vấn**: dùng được mọi toán tử quen thuộc của MongoDB, ví dụ `$eq`, `$in`, `$regex`, `$exists`, `$elemMatch`, `$text`, `$geoWithin`.

```php
use SwooleMongo\BSON\Regex;
use SwooleMongo\BSON\UTCDateTime;

$items->find(['name' => new Regex('^bút', 'i')]);
$items->find(['createdAt' => ['$gte' => new UTCDateTime(strtotime('-1 day') * 1000)]]);
$items->find(['$or' => [['stock' => 0], ['discontinued' => true]]]);
```

**Phân trang**: với collection lớn, hãy lọc theo một field có index thay vì dùng `skip`:

```php
$page = $items->find(['_id' => ['$gt' => $lastSeenId]], ['sort' => ['_id' => 1], 'limit' => 50]);
```

## 6. Cập nhật và thay thế

```php
$r = $items->updateOne(['sku' => 'A-1'], ['$set' => ['price' => 1.75], '$inc' => ['views' => 1]]);
$r->getMatchedCount();   // 1
$r->getModifiedCount();  // 1 (0 nếu dữ liệu không đổi)

$items->updateMany(['tags' => 'office'], ['$addToSet' => ['tags' => 'sale']]);

// thêm mới nếu không tìm thấy document nào
$r = $items->updateOne(['sku' => 'B-9'], ['$set' => ['name' => 'Dập ghim']], ['upsert' => true]);
$r->getUpsertedId();

// thay cả document (giữ nguyên _id)
$items->replaceOne(['sku' => 'A-1'], ['sku' => 'A-1', 'name' => 'Bút xanh', 'price' => 2]);

// cập nhật bằng aggregation pipeline
$items->updateMany([], [['$set' => ['total' => ['$multiply' => ['$price', '$qty']]]]]);

// cập nhật một phần tử trong mảng
$items->updateOne(
    ['sku' => 'A-1'],
    ['$set' => ['variants.$[v].stock' => 0]],
    ['arrayFilters' => [['v.color' => 'red']]],
);
```

**Đọc và sửa trong một bước nguyên tử**:

```php
use SwooleMongo\Operation\FindOneAndUpdate;

$counter = $db->counters->findOneAndUpdate(
    ['_id' => 'invoice'],
    ['$inc' => ['seq' => 1]],
    ['upsert' => true, 'returnDocument' => FindOneAndUpdate::RETURN_DOCUMENT_AFTER],
);
$soHoaDonTiepTheo = $counter->seq;
```

`findOneAndReplace()` và `findOneAndDelete()` dùng tương tự.

## 7. Xoá

```php
$items->deleteOne(['sku' => 'A-3'])->getDeletedCount();     // 0 hoặc 1
$items->deleteMany(['discontinued' => true])->getDeletedCount();
```

## 8. Ghi hàng loạt (bulk write)

Gửi nhiều thao tác ghi khác nhau trong ít round trip nhất có thể:

```php
$result = $items->bulkWrite([
    ['insertOne'  => [['sku' => 'C-1', 'name' => 'Thước kẻ']]],
    ['updateOne'  => [['sku' => 'A-1'], ['$set' => ['price' => 2.5]]]],
    ['updateMany' => [['tags' => 'sale'], ['$unset' => ['discount' => '']]]],
    ['replaceOne' => [['sku' => 'B-9'], ['sku' => 'B-9', 'name' => 'Dập ghim lớn'], ['upsert' => true]]],
    ['deleteOne'  => [['sku' => 'C-0']]],
    ['deleteMany' => [['stock' => 0]]],
], ['ordered' => false]);

$result->getInsertedCount();
$result->getModifiedCount();
$result->getDeletedCount();
$result->getUpsertedIds();
```

**MongoDB 8.0 trở lên**: một bulk write có thể ghi vào nhiều collection, nhiều database:

```php
use SwooleMongo\ClientBulkWrite;

$bulk = ClientBulkWrite::createWithCollection($db->orders, ['ordered' => true]);
$bulk->insertOne(['_id' => 1001, 'total' => 25], $orderId);
$bulk->withCollection($db->stock)->updateOne(['sku' => 'A-1'], ['$inc' => ['qty' => -1]]);
$result = $client->bulkWrite($bulk);
```

## 9. Aggregation, đếm và distinct

```php
$cursor = $db->orders->aggregate([
    ['$match' => ['status' => 'paid', 'createdAt' => ['$gte' => new UTCDateTime(strtotime('first day of this month') * 1000)]]],
    ['$group' => ['_id' => '$customerId', 'total' => ['$sum' => '$amount'], 'orders' => ['$sum' => 1]]],
    ['$sort' => ['total' => -1]],
    ['$limit' => 10],
    ['$lookup' => ['from' => 'customers', 'localField' => '_id', 'foreignField' => '_id', 'as' => 'customer']],
], ['allowDiskUse' => true]);

foreach ($cursor as $row) {
    printf("%s: %.2f (%d đơn)\n", $row->customer[0]->name ?? $row->_id, $row->total, $row->orders);
}
```

```php
$items->countDocuments(['tags' => 'office']);   // đếm chính xác theo bộ lọc
$items->estimatedDocumentCount();               // đếm nhanh dựa trên metadata của collection
$items->distinct('tags', ['price' => ['$gt' => 1]]);
```

## 10. Index

```php
$items->createIndex(['sku' => 1], ['unique' => true]);
$items->createIndex(['name' => 'text']);
$items->createIndex(['createdAt' => 1], ['expireAfterSeconds' => 86400]);   // TTL: tự xoá sau 1 ngày
$items->createIndexes([
    ['key' => ['category' => 1, 'price' => -1]],
    ['key' => ['location' => '2dsphere'], 'name' => 'geo'],
]);

foreach ($items->listIndexes() as $index) {
    echo $index->getName(), ' ', json_encode($index->getKey()), PHP_EOL;
}
$items->dropIndex('sku_1');
```

Index của Atlas Search dùng `createSearchIndex()`, `listSearchIndexes()`, `updateSearchIndex()` và `dropSearchIndex()`.

## 11. Kiểu BSON và typeMap

| Giá trị PHP | Lưu thành |
|---|---|
| `int`, `float`, `bool`, `string`, `null` | int32 / int64, double, bool, string, null |
| mảng tuần tự `[1, 2]` | array |
| mảng kết hợp, `stdClass` | document |
| `SwooleMongo\BSON\ObjectId` | ObjectId |
| `UTCDateTime` | date (mili giây) |
| `Decimal128` | decimal128 (lưu tiền chính xác) |
| `Int64`, `Binary`, `Regex`, `Timestamp`, `Javascript`, `MinKey`, `MaxKey` | kiểu BSON tương ứng |

```php
use SwooleMongo\BSON\{ObjectId, UTCDateTime, Decimal128, Binary};

$id = new ObjectId();
$id->getTimestamp();                            // thời điểm tạo
$date = new UTCDateTime();                      // thời điểm hiện tại
$date->toDateTime();                            // \DateTime
$money = new Decimal128('19.99');
$uuid = Binary::fromUuid('1b4e28ba-2fa1-11d2-883f-0016d3cca427');   // (subtype 4)
```

**typeMap** quyết định document đọc ra sẽ thành kiểu gì:

```php
// mặc định của Client: SwooleMongo\Model\BSONDocument / BSONArray (ArrayObject)
$client = new Client($uri);

// dùng mảng PHP thuần ở mọi nơi (nhanh nhất)
$client = new Client($uri, [], ['typeMap' => ['root' => 'array', 'document' => 'array', 'array' => 'array']]);

// chỉ áp dụng cho một thao tác
$items->findOne([], ['typeMap' => ['root' => 'array']]);
```

**Ánh xạ sang class của bạn**: implement `SwooleMongo\BSON\Persistable`. Tên class được lưu trong `__pclass`, và khi đọc với typeMap rỗng thì bạn nhận lại đúng class đó:

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
// typeMap rỗng để __pclass quyết định class (typeMap mặc định của Client sẽ trả về BSONDocument)
$product = $items->findOne(['sku' => 'P-1'], ['typeMap' => []]);     // là instance của Product
```

**JSON**: dùng `SwooleMongo\BSON\Document::fromPHP($doc)->toRelaxedExtendedJSON()` hoặc `Document::fromJSON($json)`.

## 12. Session và transaction

Transaction cần replica set hoặc sharded cluster.

**Nên dùng**: `withTransaction()` tự commit giúp bạn, và tự chạy lại toàn bộ callback khi gặp lỗi tạm thời:

```php
$session = $client->startSession();

$session->withTransaction(function (SwooleMongo\Driver\Session $session) use ($db) {
    $db->accounts->updateOne(['_id' => 'alice'], ['$inc' => ['balance' => -100]], ['session' => $session]);
    $db->accounts->updateOne(['_id' => 'bob'],   ['$inc' => ['balance' => 100]],  ['session' => $session]);
    $db->transfers->insertOne(['from' => 'alice', 'to' => 'bob', 'amount' => 100], ['session' => $session]);
});
```

- Truyền `['session' => $session]` vào **mọi** thao tác bên trong transaction.
- Ném exception trong callback thì transaction bị huỷ (abort).

**Tự điều khiển**:

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

**Các kiểu session khác**:
- **Causal consistency** (mặc định với session tạo thủ công): lệnh đọc trong session luôn thấy được các lệnh ghi trước đó của chính session, kể cả khi đọc từ secondary.
- **Snapshot**: `$client->startSession(['snapshot' => true])` khiến mọi query đọc dữ liệu tại cùng một thời điểm.

Mỗi lúc, một session chỉ thuộc về một coroutine. Đừng dùng cùng một session ở hai coroutine song song.

## 13. Change stream

Change stream cho phép phản ứng ngay khi dữ liệu thay đổi. Tính năng này cần replica set hoặc sharded cluster.

```php
$stream = $db->orders->watch(
    [['$match' => ['operationType' => ['$in' => ['insert', 'update']]]]],
    ['fullDocument' => 'updateLookup'],
);

for ($stream->rewind(); true; $stream->next()) {
    if (!$stream->valid()) {
        continue;                                   // chưa có sự kiện; next() sẽ chờ tiếp (non-blocking)
    }
    $event = $stream->current();
    echo $event->operationType, ' ', json_encode($event->fullDocument), PHP_EOL;

    $resumeToken = $stream->getResumeToken();       // lưu lại để chạy tiếp sau khi khởi động lại
}
```

- Muốn chạy tiếp sau khi khởi động lại, truyền `['resumeAfter' => $savedToken]` (hoặc `startAfter`).
- Stream cũng tự nối lại khi gặp lỗi mạng hoặc khi đổi primary.
- Hãy chạy mỗi change stream trong một coroutine riêng (`go(fn () => ...)`). Phần còn lại của worker vẫn phục vụ request bình thường trong lúc nó chờ.
- `$client->watch()` và `$db->watch()` theo dõi cả hệ thống hoặc cả một database.

## 14. GridFS (lưu file)

GridFS lưu được file lớn hơn giới hạn 16 MB của một document, bằng cách chia file thành nhiều chunk.

```php
$bucket = $db->selectGridFSBucket();               // ['bucketName' => 'fs', 'chunkSizeBytes' => 261120]

// upload
$fileId = $bucket->uploadFromStream('bao-cao.pdf', fopen('/tmp/bao-cao.pdf', 'rb'), ['metadata' => ['owner' => 'alice']]);

$upload = $bucket->openUploadStream('log.txt');
fwrite($upload, "dòng 1\n");
fclose($upload);

// download
$bucket->downloadToStream($fileId, fopen('/tmp/ban-sao.pdf', 'wb'));
$stream = $bucket->openDownloadStreamByName('bao-cao.pdf');   // lấy bản mới nhất
echo stream_get_contents($stream);

// quản lý
foreach ($bucket->find(['metadata.owner' => 'alice']) as $file) {
    echo $file->filename, ' ', $file->length, PHP_EOL;
}
$bucket->rename($fileId, 'bao-cao-2026.pdf');
$bucket->delete($fileId);
```

## 15. Read/write concern, read preference, timeout

```php
use SwooleMongo\Driver\{ReadConcern, ReadPreference, WriteConcern};

// ghi an toàn: đa số node xác nhận, ghi vào journal, chờ tối đa 5 giây
$items->insertOne($doc, ['writeConcern' => new WriteConcern(WriteConcern::MAJORITY, 5000, true)]);

// gửi rồi quên (không chờ phản hồi; nhanh nhất nhưng không biết có lỗi hay không)
$items->insertOne($doc, ['writeConcern' => new WriteConcern(0)]);

// đọc từ secondary ở một data centre cụ thể, chậm hơn primary tối đa 90 giây
$items->find([], ['readPreference' => new ReadPreference(ReadPreference::SECONDARY, [['dc' => 'sg']], ['maxStalenessSeconds' => 90])]);

// chỉ đọc dữ liệu đã được đa số node xác nhận
$items->find([], ['readConcern' => new ReadConcern(ReadConcern::MAJORITY)]);
```

**Timeout**:
- `timeoutMS` (đặt cho client hoặc cho từng thao tác) là giới hạn tổng thời gian của một thao tác. Nó tính cả chọn server, mở kết nối, các lần thử lại và `maxTimeMS` phía server.
- Khi hết giờ, driver ném `ExecutionTimeoutException` hoặc `ConnectionTimeoutException`.

```php
$client = new Client('mongodb://localhost/?timeoutMS=2000');
$items->find($filter, ['timeoutMS' => 500]);
```

## 16. Xử lý lỗi

Mọi exception đều implement `SwooleMongo\Driver\Exception\Exception`.

| Exception | Khi nào |
|---|---|
| `ConnectionTimeoutException` | Không tìm được server phù hợp trong `serverSelectionTimeoutMS` / `timeoutMS` |
| `ConnectionException` | Lỗi mạng khi đang nói chuyện với server |
| `AuthenticationException` | Sai tài khoản hoặc sai cơ chế xác thực |
| `ExecutionTimeoutException` | Thao tác vượt quá `maxTimeMS` / `timeoutMS` |
| `BulkWriteException` | Ghi thất bại (trùng khoá, sai validation…); chi tiết nằm trong `getWriteResult()` |
| `CommandException` | Server từ chối lệnh; `getResultDocument()` chứa phản hồi của server |
| `InvalidArgumentException` | Option hoặc tham số không hợp lệ |
| `EncryptionException` | Lỗi mã hoá phía client |

```php
use SwooleMongo\Driver\Exception\{BulkWriteException, ConnectionTimeoutException, ExecutionTimeoutException, RuntimeException};

try {
    $users->insertOne(['email' => 'ann@example.com']);
} catch (BulkWriteException $e) {
    foreach ($e->getWriteResult()->getWriteErrors() as $error) {
        if ($error->getCode() === 11000) {
            throw new DomainException('Email này đã được đăng ký');
        }
    }
    throw $e;
} catch (ExecutionTimeoutException $e) {
    // query chạy quá lâu
} catch (ConnectionTimeoutException $e) {
    // không kết nối được database: trả về 503
} catch (RuntimeException $e) {
    if ($e->hasErrorLabel('TransientTransactionError')) {
        // có thể chạy lại transaction an toàn
    }
}
```

Retryable read/write đã tự thử lại một lần khi lỗi mạng hoặc đổi primary. Với những trường hợp đó, bạn không cần tự viết vòng lặp thử lại.

## 17. Chạy nhiều thao tác cùng lúc

Trong một worker, tạo các coroutine rồi chờ chúng xong:

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
    $wg->wait();                                    // ba lệnh đếm chạy song song
    print_r($results);
});
```

Lưu ý:
- Mỗi thao tác đồng thời dùng một connection riêng, tối đa `maxPoolSize` cho mỗi server. Thao tác vượt quá sẽ chờ tới khi có connection rảnh (`waitQueueTimeoutMS`).
- Khi cần chạy song song rất nhiều thao tác, hãy tự giới hạn số lượng, ví dụ dùng `Swoole\Coroutine\Channel` làm semaphore.
- Cursor và session thuộc về coroutine đang dùng nó. Đừng duyệt cùng một cursor từ hai coroutine.

## 18. Swoole server và connection pool

Cách nên dùng là **mỗi worker một client**. Bản thân client đã tự quản lý connection:

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

- Tạo client trong `workerStart`, **không** tạo trước `$server->start()`. Không được dùng chung connection giữa các process đã fork.
- Tổng số connection tới MongoDB ≈ `worker_num × maxPoolSize` trên mỗi server. Hãy giữ con số này trong giới hạn connection của server.

**Dùng thư viện pool bên ngoài** (ví dụ connection pool của framework): cho mỗi client trong pool đúng một connection, và để pool giới hạn số thao tác đồng thời:

```php
$pool = new Swoole\ConnectionPool(fn () => new SwooleMongo\Client($uri, ['maxPoolSize' => 1]), 32);
$client = $pool->get();
try {
    $client->selectCollection('app', 'users')->insertOne($doc);
} finally {
    $pool->put($client);
}
```

## 19. Ví dụ xác thực

```php
// SCRAM (mặc định); ký tự đặc biệt phải được URL-encode
new Client('mongodb://' . rawurlencode($user) . ':' . rawurlencode($pass) . '@db.example.com/?authSource=admin');

// chứng chỉ X.509 phía client
new Client('mongodb://db.example.com/?tls=true&authMechanism=MONGODB-X509&tlsCertificateKeyFile=/etc/ssl/client.pem&tlsCAFile=/etc/ssl/ca.pem');

// AWS IAM (nếu URI không có credentials thì lấy từ biến môi trường, ECS hoặc EC2)
new Client('mongodb+srv://cluster0.example.net/?authMechanism=MONGODB-AWS&authSource=$external');

// OIDC với hàm lấy token của bạn
new Client('mongodb+srv://cluster0.example.net/?authMechanism=MONGODB-OIDC', [], [
    'oidcCallback' => fn (array $context) => ['accessToken' => fetchTokenFromIdp(), 'expiresInSeconds' => 3600],
]);

// OIDC trên Azure / GCP / Kubernetes
new Client('mongodb+srv://cluster0.example.net/?authMechanism=MONGODB-OIDC&authMechanismProperties=ENVIRONMENT:azure,TOKEN_RESOURCE:api://my-app');

// Kerberos: chạy `kinit app@EXAMPLE.COM` trước, hoặc truyền mật khẩu trong URI
new Client('mongodb://app%40EXAMPLE.COM@db.example.com/?authMechanism=GSSAPI&authMechanismProperties=SERVICE_NAME:mongodb');
```

Mã hoá field phía client (CSFLE / Queryable Encryption) được hướng dẫn trong [README](../README.vi.md#mã-hoá-field-phía-client).

## 20. Giám sát và ghi log

Đăng ký nhận event của các lệnh để log query chậm hoặc thu thập số liệu:

```php
use SwooleMongo\Driver\Monitoring\{CommandSubscriber, CommandStartedEvent, CommandSucceededEvent, CommandFailedEvent};

final class SlowQueryLogger implements CommandSubscriber
{
    public function commandStarted(CommandStartedEvent $event): void {}

    public function commandSucceeded(CommandSucceededEvent $event): void
    {
        if ($event->getDurationMicros() > 100_000) {
            error_log(sprintf('chậm: %s trên %s mất %d ms', $event->getCommandName(), $event->getDatabaseName(), $event->getDurationMicros() / 1000));
        }
    }

    public function commandFailed(CommandFailedEvent $event): void
    {
        error_log('lỗi mongo: ' . $event->getError()->getMessage());
    }
}

$client->addSubscriber(new SlowQueryLogger());
```

`SDAMSubscriber` báo các thay đổi của topology, như có primary mới, một server bị down hay các lần heartbeat.

## 21. API driver cấp thấp

`SwooleMongo\Driver\*` giống ext-mongodb, dành cho khi bạn cần toàn quyền kiểm soát:

```php
use SwooleMongo\Driver\{Manager, Query, BulkWrite, Command};

$manager = new Manager('mongodb://localhost:27017');      // hoặc $client->getManager()

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

## 22. Khắc phục sự cố

| Hiện tượng | Nguyên nhân và cách xử lý |
|---|---|
| Cảnh báo *running inside a Swoole coroutine but the runtime hook … is disabled* | Bật `SWOOLE_HOOK_ALL` (`Co\run` tự bật; với server thì cần `hook_flags`). |
| `ConnectionTimeoutException: No suitable server` | Sai host/port trong URI, firewall chặn, hoặc replica set khai báo hostname mà ứng dụng không phân giải được. Thử `directConnection=true` để kiểm tra. |
| `AuthenticationException` | Sai user/mật khẩu hoặc `authSource`. Tài khoản thường được tạo trong `admin`. Mật khẩu có ký tự đặc biệt phải URL-encode. |
| Server báo quá nhiều connection | Giảm `maxPoolSize` hoặc `worker_num`; chỉ tạo client trong `workerStart`. |
| Request bị treo ở `ChangeStream` | Duyệt change stream trong coroutine riêng; đặt `maxAwaitTimeMS`. |
| `Class "SwooleMongo\Client" not found` | `swoole_mongo.enable_library` đang là `0`, hoặc extension chưa được nạp (kiểm tra bằng `php --ri swoole_mongo`). |
| Không có class `ClientEncryption` | Extension được build không kèm libmongocrypt; build lại với `--with-swoole-mongo-mongocrypt`. |
