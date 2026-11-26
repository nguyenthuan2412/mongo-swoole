<?php
/** php bench/compare.php php.json node.json -> markdown table */
[$a, $b] = [json_decode(file_get_contents($argv[1]), true), json_decode(file_get_contents($argv[2]), true)];
echo "| Workload | swmongo ops/s | Node.js ops/s | swmongo / Node | swmongo p50 / p99 ms | Node p50 / p99 ms |\n|---|---:|---:|---:|---:|---:|\n";
foreach ($a as $k => $r) {
    $n = $b[$k];
    printf("| %s | %s | %s | %.2fx | %s / %s | %s / %s |\n", $r['name'], number_format($r['opsSec']), number_format($n['opsSec']), $r['opsSec'] / $n['opsSec'], $r['p50'], $r['p99'], $n['p50'], $n['p99']);
}
