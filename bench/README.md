# Benchmark: swoole-mongo vs Node.js vs Java

**English** · [Tiếng Việt](README.vi.md) · [Back to README](../README.md)

The same workload was run with three drivers against the same MongoDB server, and each driver was tuned for speed.
Every number below is the **median of 3 rounds**. The order of the drivers rotates every round, and the server is flushed and left idle before each driver.

## Results

Operations per second (higher is better; the best of each row is **bold**):

| Workload | swoole-mongo | Node.js | Java (virtual threads) | Java (platform threads) |
|---|---:|---:|---:|---:|
| insertOne, sequential | **710** | 680 | 586 | |
| findOne without index, sequential | **954** | 762 | 776 | |
| findOne with index, sequential | **1,771** | 1,767 | 1,623 | |
| updateOne with index, sequential | 633 | **669** | 549 | |
| find 100 docs → array | **1,078** | 676 | 904 | |
| find + iterate 5000 docs | **52** | 32 | 43 | |
| insertMany 1000 docs | **84** | 53 | 41 | |
| aggregate `$match` + `$group` | **79** | 65 | 62 | |
| findOne, 100 concurrent | **27,475** | 15,636 | 12,379 | 17,305 |
| insertOne, 100 concurrent | **12,868** | 10,241 | 9,530 | 10,027 |

Latency in milliseconds (p50 / p99, lower is better):

| Workload | swoole-mongo | Node.js | Java (virtual threads) |
|---|---:|---:|---:|
| insertOne, sequential | 1.10 / 5.46 | 1.17 / 5.83 | 1.16 / 8.01 |
| findOne with index, sequential | 0.43 / 2.02 | 0.41 / 2.19 | 0.38 / 3.82 |
| find 100 docs → array | 0.75 / 2.59 | 1.21 / 4.76 | 0.96 / 4.30 |
| find + iterate 5000 docs | 18.7 / 29.1 | 32.0 / 46.1 | 23.0 / 38.5 |
| insertMany 1000 docs | 11.1 / 29.0 | 16.7 / 42.1 | 24.1 / 43.3 |
| findOne, 100 concurrent | 2.65 / 14.2 | 5.59 / 20.0 | 5.64 / 42.6 |
| insertOne, 100 concurrent | 7.18 / 19.0 | 9.01 / 23.4 | 9.56 / 30.4 |

The raw data (every row, p50 and p99) is in [results-2026-11/](results-2026-11/).

## What the numbers say

- **Concurrency: swoole-mongo is clearly fastest.**
  - findOne with 100 concurrent operations: 1.76× Node.js, 2.2× Java on virtual threads and 1.6× Java on platform threads, with the lowest p99.
  - insertOne with 100 concurrent operations: 1.26× Node.js and 1.35× Java.
  - Switching to the next coroutine costs almost nothing in C: there is no promise to allocate and no thread to park or unpark.
- **Bulk document work (BSON encode/decode): swoole-mongo is fastest.**
  - find + iterate 5000 docs: 1.6× Node.js and 1.2× Java.
  - insertMany: 1.6× Node.js and 2× Java.
  - find 100 docs: 1.6× Node.js and 1.2× Java.
  - The BSON codec is written in C and builds PHP arrays directly.
- **Single sequential operations: practically a tie.** The network round trip (~0.4–1.2 ms through Docker Desktop) dominates. swoole-mongo leads 4 of the 5 sequential rows; Node.js is 5% ahead on updateOne.
- **Java: virtual threads vs platform threads.**
  - Virtual threads were 40% *slower* than a 100-thread pool for findOne (12.4k vs 17.3k ops/s), and about equal for insertOne.
  - With a connection pool of 100, virtual threads add scheduling around blocking socket I/O and do not remove the round trip. At this level of concurrency they do not beat a coroutine driver.

### Why sequential operations look the same

Measured on its own, the client-side work of swoole-mongo is **5–18 µs per operation**.
- The C driver accounts for 5–7 µs: building and encoding the command, writing to the socket, reading and decoding the reply.
- The embedded PHP library adds about 11 µs on top for `insertOne`.

Everything else is time spent waiting for MongoDB. A faster driver cannot make a single sequential operation much faster; running operations concurrently can.

An earlier version of this benchmark always ran PHP right after Java. The server was still flushing Java's writes, so PHP's first workload (insertOne) looked about 20% slower.
The script now rotates the order every round, and flushes (`fsync`) and idles the server before each driver.

These are results from one machine, a laptop talking to a Docker container.
- Absolute numbers are higher on Linux servers with real networking.
- Sequential rows vary by about ±15% between runs.
- Run the benchmark on your own hardware before drawing conclusions (see below).

## Environment

| | |
|---|---|
| Machine | Apple M4 (10 cores), 16 GB RAM, macOS |
| MongoDB | 7.0.48, single-node replica set in Docker Desktop (`mongo:7`), port 27300 |
| swoole-mongo | 0.2.0 on PHP 8.3.30 + Swoole 5.1.7 |
| Node.js | v22.14.0 + `mongodb` 6.22.0 (official driver, latest major) |
| Java | OpenJDK 26.0.1 + `mongodb-driver-sync` 5.15.0 (official, latest) |

## Settings: each driver at its fastest

| Driver | Settings |
|---|---|
| swoole-mongo | OPcache enabled with the tracing JIT (`opcache.jit=tracing`, 128 MB buffer); `typeMap` returns plain PHP arrays (the cheapest representation) |
| Node.js | `NODE_ENV=production`; `enableUtf8Validation: false` (swoole-mongo does not validate UTF-8 on decode either, so both do the same work) |
| Java | Fixed 2 GB heap (`-Xms2g -Xmx2g`), `-XX:+UseParallelGC` (throughput collector), `-XX:+AlwaysPreTouch`; concurrency on `Executors.newVirtualThreadPerTaskExecutor()`, plus a run on a 100 platform thread pool |
| All | `maxPoolSize=100`, no compression, same 250-byte documents, 2000 insert+find warm-up operations before measuring (warms the JIT of Node, Java and PHP), same order of workloads, `fsync` + 5 s idle before each driver, driver order rotated every round |

Workloads (5000 operations for each sequential row):

| Workload | What it does |
|---|---|
| insertOne, sequential | 5000 single inserts, one after another |
| findOne without / with index | 5000 lookups by an `i` field, first without, then with an index |
| updateOne with index | 5000 `$inc` updates by `i` |
| find 100 docs → array | 500 queries returning 100 documents each, collected into an array |
| find + iterate 5000 docs | 20 full scans of 5000 documents, iterating the cursor |
| insertMany 1000 docs | 20 batches of 1000 documents |
| aggregate | 200 runs of `$match` + `$group` with `$sum` / `$avg` |
| concurrent | 20 000 findOne or insertOne calls by 100 coroutines, promises or threads |

## Reproduce

```sh
# 1. MongoDB
docker run -d --name swmbench -p 127.0.0.1:27300:27300 mongo:7 mongod --replSet rb --bind_ip_all --port 27300
docker exec swmbench mongosh --port 27300 --eval "rs.initiate({_id:'rb',members:[{_id:0,host:'127.0.0.1:27300'}]})"

# 2. All three benchmarks, 3 rounds, median, comparison table
export BENCH_URI='mongodb://127.0.0.1:27300/?replicaSet=rb'
ROUNDS=3 ./run.sh
```

`run.sh` installs the Node and Java dependencies the first time (`npm install`, `mvn package`). It needs PHP with Swoole, Node.js 18+, JDK 21+ and Maven.

| Variable | Default | Meaning |
|---|---|---|
| `BENCH_URI` | `mongodb://127.0.0.1:27119/?replicaSet=rs0` | Server to test |
| `BENCH_N` | 5000 | Operations per sequential workload (×4 for the concurrent ones) |
| `BENCH_CONC` | 100 | Concurrency and `maxPoolSize` |
| `BENCH_WARMUP` | 2000 | Warm-up operations |
| `ROUNDS` | 3 | Rounds (the median is reported) |
| `BENCH_SETTLE` | 5 | Seconds of idle time before each driver |
| `JAVA` | `java` | Path of the `java` binary |
| `SWOOLE_MONGO_SO` | `../modules/swoole_mongo.so` | Extension to load |

Files:
- `bench.php` (swoole-mongo), `node/bench.mjs` and `java/src/main/java/Bench.java` run the benchmarks.
- `settle.php` flushes the server and waits between drivers.
- `median.php` merges the rounds, and `compare.php` prints the table.
