# Benchmark: swoole-mongo so với Node.js và Java

[English](README.md) · **Tiếng Việt** · [Về README](../README.vi.md)

Ba driver chạy cùng một workload, trên cùng một server MongoDB, và bên nào cũng dùng cấu hình nhanh nhất của mình.
Mọi con số dưới đây là **trung vị của 3 vòng chạy**. Thứ tự driver đổi qua từng vòng, và trước mỗi driver server đều được flush rồi cho nghỉ.

## Kết quả

Số thao tác mỗi giây (càng cao càng tốt; số **in đậm** là nhanh nhất của từng dòng):

| Workload | swoole-mongo | Node.js | Java (virtual thread) | Java (platform thread) |
|---|---:|---:|---:|---:|
| insertOne, tuần tự | **710** | 680 | 586 | |
| findOne không index, tuần tự | **954** | 762 | 776 | |
| findOne có index, tuần tự | **1,771** | 1,767 | 1,623 | |
| updateOne có index, tuần tự | 633 | **669** | 549 | |
| find 100 doc → mảng | **1,078** | 676 | 904 | |
| find + duyệt 5000 doc | **52** | 32 | 43 | |
| insertMany 1000 doc | **84** | 53 | 41 | |
| aggregate `$match` + `$group` | **79** | 65 | 62 | |
| findOne, 100 đồng thời | **27,475** | 15,636 | 12,379 | 17,305 |
| insertOne, 100 đồng thời | **12,868** | 10,241 | 9,530 | 10,027 |

Độ trễ tính bằng mili giây (p50 / p99, càng thấp càng tốt):

| Workload | swoole-mongo | Node.js | Java (virtual thread) |
|---|---:|---:|---:|
| insertOne, tuần tự | 1.10 / 5.46 | 1.17 / 5.83 | 1.16 / 8.01 |
| findOne có index, tuần tự | 0.43 / 2.02 | 0.41 / 2.19 | 0.38 / 3.82 |
| find 100 doc → mảng | 0.75 / 2.59 | 1.21 / 4.76 | 0.96 / 4.30 |
| find + duyệt 5000 doc | 18.7 / 29.1 | 32.0 / 46.1 | 23.0 / 38.5 |
| insertMany 1000 doc | 11.1 / 29.0 | 16.7 / 42.1 | 24.1 / 43.3 |
| findOne, 100 đồng thời | 2.65 / 14.2 | 5.59 / 20.0 | 5.64 / 42.6 |
| insertOne, 100 đồng thời | 7.18 / 19.0 | 9.01 / 23.4 | 9.56 / 30.4 |

Dữ liệu gốc (đủ mọi dòng, p50 và p99) nằm trong [results-2026-11/](results-2026-11/).

## Kết quả nói lên điều gì

- **Chạy đồng thời: swoole-mongo nhanh nhất rõ rệt.**
  - findOne với 100 thao tác đồng thời: gấp 1,76 lần Node.js, 2,2 lần Java dùng virtual thread và 1,6 lần Java dùng platform thread; p99 cũng thấp nhất.
  - insertOne với 100 thao tác đồng thời: gấp 1,26 lần Node.js và 1,35 lần Java.
  - Lý do là việc chuyển sang coroutine kế tiếp trong C gần như không tốn gì: không phải tạo promise, không phải park/unpark thread.
- **Xử lý nhiều document (encode/decode BSON): swoole-mongo nhanh nhất.**
  - find + duyệt 5000 doc: gấp 1,6 lần Node.js và 1,2 lần Java.
  - insertMany: gấp 1,6 lần Node.js và 2 lần Java.
  - find 100 doc: gấp 1,6 lần Node.js và 1,2 lần Java.
  - Bộ codec BSON viết bằng C và tạo thẳng mảng PHP.
- **Thao tác tuần tự đơn lẻ: gần như ngang nhau.** Thời gian chủ yếu nằm ở round trip mạng (khoảng 0,4–1,2 ms qua Docker Desktop). swoole-mongo dẫn 4/5 dòng tuần tự; Node.js nhanh hơn 5% ở updateOne.
- **Java: virtual thread so với platform thread.**
  - Với findOne, virtual thread *chậm hơn* 40% so với pool 100 thread (12,4k so với 17,3k ops/s); với insertOne thì xấp xỉ nhau.
  - Khi connection pool là 100, virtual thread chỉ thêm phần lập lịch quanh I/O socket blocking chứ không bỏ được round trip. Ở mức đồng thời này nó không thắng được driver dùng coroutine.

### Vì sao các thao tác tuần tự trông giống nhau

Khi đo riêng, phần việc phía client của swoole-mongo chỉ tốn **5–18 µs mỗi thao tác**.
- Driver C chiếm 5–7 µs: dựng và encode lệnh, ghi ra socket, đọc và decode phản hồi.
- Tầng library PHP nhúng sẵn tốn thêm khoảng 11 µs với `insertOne`.

Toàn bộ thời gian còn lại là chờ MongoDB trả lời. Vì thế driver nhanh hơn cũng không làm một thao tác tuần tự nhanh lên được bao nhiêu. Cái thật sự giúp nhanh hơn là chạy song song nhiều thao tác.

Bản benchmark trước đây luôn chạy PHP ngay sau Java. Lúc đó server vẫn đang flush dữ liệu Java vừa ghi, nên bài đầu tiên của PHP (insertOne) trông chậm hơn khoảng 20%.
Giờ script đổi thứ tự driver qua từng vòng, và trước mỗi driver đều `fsync` rồi cho server nghỉ.

Đây là kết quả trên một máy: laptop nói chuyện với container Docker.
- Trên server Linux có mạng thật, số tuyệt đối sẽ cao hơn.
- Các dòng tuần tự dao động khoảng ±15% giữa các lần chạy.
- Hãy chạy lại trên phần cứng của bạn trước khi kết luận (xem bên dưới).

## Môi trường

| | |
|---|---|
| Máy | Apple M4 (10 nhân), RAM 16 GB, macOS |
| MongoDB | 7.0.48, replica set một node chạy trong Docker Desktop (`mongo:7`), port 27300 |
| swoole-mongo | 0.2.0 trên PHP 8.3.30 + Swoole 5.1.7 |
| Node.js | v22.14.0 + `mongodb` 6.22.0 (driver chính thức, bản major mới nhất) |
| Java | OpenJDK 26.0.1 + `mongodb-driver-sync` 5.15.0 (driver chính thức, mới nhất) |

## Cấu hình: bên nào cũng chạy ở mức nhanh nhất

| Driver | Cấu hình |
|---|---|
| swoole-mongo | Bật OPcache kèm JIT tracing (`opcache.jit=tracing`, buffer 128 MB); `typeMap` trả về mảng PHP thuần (cách biểu diễn rẻ nhất) |
| Node.js | `NODE_ENV=production`; `enableUtf8Validation: false` (swoole-mongo cũng không validate UTF-8 khi decode, nên hai bên làm cùng một lượng việc) |
| Java | Heap cố định 2 GB (`-Xms2g -Xmx2g`), `-XX:+UseParallelGC` (GC tối ưu throughput), `-XX:+AlwaysPreTouch`; phần đồng thời chạy trên `Executors.newVirtualThreadPerTaskExecutor()`, thêm một lượt với pool 100 platform thread |
| Chung | `maxPoolSize=100`, không nén, cùng document 250 byte, 2000 thao tác insert+find để warm-up trước khi đo (làm nóng JIT của Node, Java và PHP), cùng thứ tự workload, `fsync` + nghỉ 5 giây trước mỗi driver, đổi thứ tự driver qua từng vòng |

Các workload (mỗi dòng tuần tự là 5000 thao tác):

| Workload | Nội dung |
|---|---|
| insertOne, tuần tự | 5000 lệnh insert lần lượt từng cái |
| findOne không / có index | 5000 lần tìm theo field `i`, lượt đầu chưa có index, lượt sau đã có |
| updateOne có index | 5000 lệnh `$inc` theo `i` |
| find 100 doc → mảng | 500 query, mỗi query trả 100 document, gom vào mảng |
| find + duyệt 5000 doc | 20 lần quét toàn bộ 5000 document bằng cursor |
| insertMany 1000 doc | 20 batch, mỗi batch 1000 document |
| aggregate | 200 lần `$match` + `$group` với `$sum` / `$avg` |
| đồng thời | 20 000 lệnh findOne hoặc insertOne, chia cho 100 coroutine / promise / thread |

## Tự chạy lại

```sh
# 1. MongoDB
docker run -d --name swmbench -p 127.0.0.1:27300:27300 mongo:7 mongod --replSet rb --bind_ip_all --port 27300
docker exec swmbench mongosh --port 27300 --eval "rs.initiate({_id:'rb',members:[{_id:0,host:'127.0.0.1:27300'}]})"

# 2. Chạy cả ba benchmark, 3 vòng, lấy trung vị, in bảng so sánh
export BENCH_URI='mongodb://127.0.0.1:27300/?replicaSet=rb'
ROUNDS=3 ./run.sh
```

Lần đầu chạy, `run.sh` tự cài dependency cho Node và Java (`npm install`, `mvn package`). Cần có PHP kèm Swoole, Node.js 18+, JDK 21+ và Maven.

| Biến | Mặc định | Ý nghĩa |
|---|---|---|
| `BENCH_URI` | `mongodb://127.0.0.1:27119/?replicaSet=rs0` | Server dùng để test |
| `BENCH_N` | 5000 | Số thao tác cho mỗi workload tuần tự (×4 với phần đồng thời) |
| `BENCH_CONC` | 100 | Mức đồng thời, cũng là `maxPoolSize` |
| `BENCH_WARMUP` | 2000 | Số thao tác warm-up |
| `ROUNDS` | 3 | Số vòng (báo cáo trung vị) |
| `BENCH_SETTLE` | 5 | Số giây cho server nghỉ trước mỗi driver |
| `JAVA` | `java` | Đường dẫn tới lệnh `java` |
| `SWOOLE_MONGO_SO` | `../modules/swoole_mongo.so` | Extension sẽ nạp |

Các file:
- `bench.php` (swoole-mongo), `node/bench.mjs` và `java/src/main/java/Bench.java` là ba bản benchmark.
- `settle.php` flush server và chờ giữa các driver.
- `median.php` gộp các vòng chạy, còn `compare.php` in bảng so sánh.
