// Same workload as bench/bench.php and bench/node/bench.mjs, with the official sync Java driver.
// The concurrent part runs twice: on virtual threads and on a pool of platform threads.
import com.mongodb.client.*;
import com.mongodb.client.model.*;
import org.bson.Document;
import org.bson.conversions.Bson;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

public class Bench {
    static final List<Map<String, Object>> results = new ArrayList<>();

    interface Body { void run(List<Double> lat) throws Exception; }
    interface Op { void run() throws Exception; }

    static Document doc(int i) {
        return new Document("i", i).append("name", "user" + i).append("email", "user" + i + "@example.com").append("age", i % 90)
            .append("active", i % 2 == 0).append("tags", List.of("a", "b", "c"))
            .append("address", new Document("city", "Hanoi").append("zip", "100000").append("geo", List.of(105.8, 21.0)))
            .append("score", i * 1.5).append("created", new Date());
    }

    static void timed(List<Double> lat, Op f) throws Exception {
        long t = System.nanoTime();
        f.run();
        lat.add((System.nanoTime() - t) / 1e6);
    }

    static void bench(String name, int ops, Body b) throws Exception {
        List<Double> lat = Collections.synchronizedList(new ArrayList<>(ops));
        long t0 = System.nanoTime();
        b.run(lat);
        double ms = (System.nanoTime() - t0) / 1e6;
        List<Double> s = new ArrayList<>(lat);
        Collections.sort(s);
        Map<String, Object> r = new LinkedHashMap<>();
        r.put("name", name); r.put("ops", ops); r.put("ms", ms); r.put("opsSec", ops / ms * 1000);
        r.put("p50", s.isEmpty() ? "-" : String.format(Locale.ROOT, "%.3f", s.get(Math.min(s.size() - 1, (int) (s.size() * 0.5)))));
        r.put("p99", s.isEmpty() ? "-" : String.format(Locale.ROOT, "%.3f", s.get(Math.min(s.size() - 1, (int) (s.size() * 0.99)))));
        results.add(r);
    }

    static void concurrent(String name, int total, int conc, ExecutorService ex, IntOp op) throws Exception {
        bench(name, total, lat -> {
            AtomicInteger next = new AtomicInteger();
            List<Future<?>> fs = new ArrayList<>();
            for (int w = 0; w < conc; w++) {
                fs.add(ex.submit(() -> {
                    int i;
                    while ((i = next.getAndIncrement()) < total) { final int k = i; timed(lat, () -> op.run(k)); }
                    return null;
                }));
            }
            for (Future<?> f : fs) f.get();
        });
    }
    interface IntOp { void run(int i) throws Exception; }

    public static void main(String[] args) throws Exception {
        String uri = Optional.ofNullable(System.getenv("BENCH_URI")).orElse("mongodb://127.0.0.1:27119/?replicaSet=rs0");
        int N = Integer.parseInt(Optional.ofNullable(System.getenv("BENCH_N")).orElse("5000"));
        int CONC = Integer.parseInt(Optional.ofNullable(System.getenv("BENCH_CONC")).orElse("100"));
        int WARM = Integer.parseInt(Optional.ofNullable(System.getenv("BENCH_WARMUP")).orElse("2000"));
        String sep = uri.contains("?") ? "&" : "?";
        try (MongoClient client = MongoClients.create(uri + sep + "maxPoolSize=" + CONC)) {
            MongoCollection<Document> col = client.getDatabase("bench").getCollection("java");
            col.drop();
            MongoCollection<Document> warm = client.getDatabase("bench").getCollection("warmup_java");
            warm.drop();
            for (int i = 0; i < WARM; i++) { warm.insertOne(doc(i)); warm.find(Filters.eq("i", i)).first(); }
            warm.drop();

            bench("insertOne (sequential)", N, lat -> { for (int i = 0; i < N; i++) { final int k = i; timed(lat, () -> col.insertOne(doc(k))); } });
            bench("findOne by field (sequential)", N, lat -> { for (int i = 0; i < N; i++) { final int k = i; timed(lat, () -> col.find(Filters.eq("i", k)).first()); } });
            col.createIndex(Indexes.ascending("i"));
            bench("findOne indexed (sequential)", N, lat -> { for (int i = 0; i < N; i++) { final int k = i; timed(lat, () -> col.find(Filters.eq("i", k)).first()); } });
            bench("updateOne indexed (sequential)", N, lat -> { for (int i = 0; i < N; i++) { final int k = i; timed(lat, () -> col.updateOne(Filters.eq("i", k), Updates.inc("age", 1))); } });
            bench("find 100 docs toArray", 500, lat -> { for (int k = 0; k < 500; k++) { final int g = k; timed(lat, () -> col.find(Filters.gte("i", g)).limit(100).into(new ArrayList<>())); } });
            bench("find all 5000 docs (iterate)", 20, lat -> { for (int k = 0; k < 20; k++) timed(lat, () -> { int n = 0; for (Document d : col.find()) n++; }); });
            bench("insertMany 1000 docs", 20, lat -> {
                for (int k = 0; k < 20; k++) {
                    List<Document> docs = new ArrayList<>(1000);
                    for (int j = 0; j < 1000; j++) docs.add(doc(100000 + k * 1000 + j));
                    timed(lat, () -> col.insertMany(docs));
                }
            });
            List<Bson> pipeline = List.of(Aggregates.match(Filters.lt("age", 50)),
                Aggregates.group("$active", Accumulators.sum("n", 1), Accumulators.avg("avg", "$score")));
            bench("aggregate $group", 200, lat -> { for (int k = 0; k < 200; k++) timed(lat, () -> col.aggregate(pipeline).into(new ArrayList<>())); });

            int CN = N * 4;
            try (ExecutorService vt = Executors.newVirtualThreadPerTaskExecutor()) {
                concurrent("findOne concurrent x" + CONC, CN, CONC, vt, i -> col.find(Filters.eq("i", i % N)).first());
                concurrent("insertOne concurrent x" + CONC, CN, CONC, vt, i -> col.insertOne(doc(200000 + i)));
            }
            try (ExecutorService pt = Executors.newFixedThreadPool(CONC)) {
                concurrent("findOne concurrent x" + CONC + " (platform threads)", CN, CONC, pt, i -> col.find(Filters.eq("i", i % N)).first());
                concurrent("insertOne concurrent x" + CONC + " (platform threads)", CN, CONC, pt, i -> col.insertOne(doc(400000 + i)));
            }
        }
        StringBuilder sb = new StringBuilder("[");
        for (Map<String, Object> r : results) {
            if (sb.length() > 1) sb.append(',');
            sb.append(String.format(Locale.ROOT, "{\"name\":\"%s\",\"ops\":%d,\"ms\":%.3f,\"opsSec\":%.3f,\"p50\":\"%s\",\"p99\":\"%s\"}",
                r.get("name"), r.get("ops"), r.get("ms"), r.get("opsSec"), r.get("p50"), r.get("p99")));
        }
        System.out.println(sb.append(']'));
    }
}
