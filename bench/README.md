# Benchmark: swmongo vs the Node.js driver

Same workload on both sides (`bench.php` and `node/bench.mjs`), against the same MongoDB 7 replica set (1 node, Docker).
- Documents are about 250 bytes. The sequential part runs 5000 operations.
- The concurrent part uses 100 coroutines (swmongo) or 100 promises (Node), with `maxPoolSize=100`.

```sh
docker run -d --name swmbench -p 127.0.0.1:27300:27300 mongo:7 mongod --replSet rb --bind_ip_all --port 27300
docker exec swmbench mongosh --port 27300 --eval "rs.initiate({_id:'rb',members:[{_id:0,host:'127.0.0.1:27300'}]})"
export BENCH_URI='mongodb://127.0.0.1:27300/?replicaSet=rb'
(cd node && npm install) && node node/bench.mjs > node.json
php -d extension=swmongo.so bench.php > php.json
php compare.php php.json node.json
```

Results on a MacBook (Apple Silicon), with PHP 8.3, Swoole 5.1, Node 22 and mongodb 6.22, average of 2 runs:

| Workload | swmongo ops/s | Node.js ops/s | swmongo / Node |
|---|---:|---:|---:|
| insertOne (sequential) | 810 | 659 | 1.23x |
| findOne by field, no index (sequential) | 1,039 | 768 | 1.35x |
| findOne indexed (sequential) | 2,162 | 1,947 | 1.11x |
| updateOne indexed (sequential) | 652 | 650 | 1.00x |
| find 100 docs toArray | 1,058 | 880 | 1.20x |
| find all 5000 docs (iterate) | 46 | 30 | 1.53x |
| insertMany 1000 docs | 91 | 64 | 1.42x |
| aggregate $group | 77 | 70 | 1.10x |
| findOne concurrent x100 | 28,491 | 13,501 | **2.11x** |
| insertOne concurrent x100 | 11,915 | 10,200 | 1.17x |

Notes:
- Sequential operations are dominated by the network round trip through Docker Desktop (~0.3–1 ms), so individual runs vary by ±20%.
- Decoding and encoding many documents (`find all`, `insertMany`) and high concurrency are where the C BSON code and the coroutine scheduler pull ahead.
