#!/bin/sh
# Runs the three benchmarks ROUNDS times (interleaved) against BENCH_URI and prints the comparison table.
# Each driver uses its fastest settings:
#   swoole-mongo  OPcache + tracing JIT, array typeMap
#   Node.js       NODE_ENV=production, enableUtf8Validation=false
#   Java          JDK 21+, fixed 2 GB heap, ParallelGC, virtual threads (+ platform threads for reference)
set -e
cd "$(dirname "$0")"
ROUNDS=${ROUNDS:-3}
OUT=${OUT:-results}
EXT=${SWOOLE_MONGO_SO:-../modules/swoole_mongo.so}
JAVA=${JAVA:-java}
mkdir -p "$OUT"
[ -d node/node_modules ] || (cd node && npm install --silent)
[ -d java/target/lib ] || (cd java && mvn -q package -DskipTests)
run_one() {
  # flush the previous driver's writes and let the server go idle, so nobody inherits its background work
  php -d extension="$EXT" settle.php
  case $1 in
    php)  php -d extension="$EXT" -d opcache.enable_cli=1 -d opcache.jit=tracing -d opcache.jit_buffer_size=128M bench.php > "$OUT/php.$2.json" ;;
    node) NODE_ENV=production node node/bench.mjs > "$OUT/node.$2.json" ;;
    java) "$JAVA" -Xms2g -Xmx2g -XX:+UseParallelGC -XX:+AlwaysPreTouch -cp "java/target/bench-java-1.jar:java/target/lib/*" Bench > "$OUT/java.$2.json" ;;
  esac
}
for r in $(seq 1 "$ROUNDS"); do
  # rotate the order every round: php node java / node java php / java php node
  case $(( (r - 1) % 3 )) in
    0) order="php node java" ;;
    1) order="node java php" ;;
    2) order="java php node" ;;
  esac
  echo "round $r/$ROUNDS: $order" >&2
  for d in $order; do run_one "$d" "$r"; done
done
for d in php node java; do php median.php "$OUT/$d.json" "$OUT"/$d.[0-9]*.json; done
php compare.php "$OUT/php.json" "$OUT/node.json" "$OUT/java.json"
