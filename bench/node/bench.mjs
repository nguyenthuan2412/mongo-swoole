// Same workload as bench/bench.php, using the official Node.js driver.
import { MongoClient } from 'mongodb';

const uri = process.env.BENCH_URI || 'mongodb://127.0.0.1:27119/?replicaSet=rs0';
const N = +(process.env.BENCH_N || 5000);
const CONC = +(process.env.BENCH_CONC || 100);
const doc = (i) => ({ i, name: 'user' + i, email: `user${i}@example.com`, age: i % 90, active: i % 2 === 0,
  tags: ['a', 'b', 'c'], address: { city: 'Hanoi', zip: '100000', geo: [105.8, 21.0] }, score: i * 1.5, created: new Date() });

// swoole-mongo does not validate UTF-8 while decoding either
const client = new MongoClient(uri, { maxPoolSize: CONC, enableUtf8Validation: false });
await client.connect();
const col = client.db('bench').collection('node');
await col.drop().catch(() => {});
const warm = client.db('bench').collection('warmup_node');
await warm.drop().catch(() => {});
for (let i = 0, W = +(process.env.BENCH_WARMUP || 2000); i < W; i++) { await warm.insertOne(doc(i)); await warm.findOne({ i }); }
await warm.drop();
const results = [];

async function bench(name, ops, fn) {
  const lat = [];
  const t0 = process.hrtime.bigint();
  await fn(lat);
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  lat.sort((a, b) => a - b);
  const p = (q) => lat.length ? lat[Math.min(lat.length - 1, Math.floor(lat.length * q))].toFixed(3) : '-';
  results.push({ name, ops, ms, opsSec: ops / ms * 1000, p50: p(0.5), p99: p(0.99) });
}
const timed = async (lat, f) => { const t = process.hrtime.bigint(); await f(); lat.push(Number(process.hrtime.bigint() - t) / 1e6); };

await bench('insertOne (sequential)', N, async (lat) => { for (let i = 0; i < N; i++) await timed(lat, () => col.insertOne(doc(i))); });
await bench('findOne by field (sequential)', N, async (lat) => { for (let i = 0; i < N; i++) await timed(lat, () => col.findOne({ i })); });
await col.createIndex({ i: 1 });
await bench('findOne indexed (sequential)', N, async (lat) => { for (let i = 0; i < N; i++) await timed(lat, () => col.findOne({ i })); });
await bench('updateOne indexed (sequential)', N, async (lat) => { for (let i = 0; i < N; i++) await timed(lat, () => col.updateOne({ i }, { $inc: { age: 1 } })); });
await bench('find 100 docs toArray', 500, async (lat) => { for (let k = 0; k < 500; k++) await timed(lat, () => col.find({ i: { $gte: k } }).limit(100).toArray()); });
await bench('find all 5000 docs (iterate)', 20, async (lat) => { for (let k = 0; k < 20; k++) await timed(lat, async () => { let n = 0; for await (const d of col.find({})) n++; }); });
await bench('insertMany 1000 docs', 20, async (lat) => { for (let k = 0; k < 20; k++) await timed(lat, () => col.insertMany(Array.from({ length: 1000 }, (_, j) => doc(100000 + k * 1000 + j)))); });
await bench('aggregate $group', 200, async (lat) => { for (let k = 0; k < 200; k++) await timed(lat, () => col.aggregate([{ $match: { age: { $lt: 50 } } }, { $group: { _id: '$active', n: { $sum: 1 }, avg: { $avg: '$score' } } }]).toArray()); });
const CN = N * 4;
await bench(`findOne concurrent x${CONC}`, CN, async (lat) => {
  let next = 0;
  await Promise.all(Array.from({ length: CONC }, async () => { while (next < CN) { const i = next++; await timed(lat, () => col.findOne({ i: i % N })); } }));
});
await bench(`insertOne concurrent x${CONC}`, CN, async (lat) => {
  let next = 0;
  await Promise.all(Array.from({ length: CONC }, async () => { while (next < CN) { const i = next++; await timed(lat, () => col.insertOne(doc(200000 + i))); } }));
});
await client.close();
console.log(JSON.stringify(results));
