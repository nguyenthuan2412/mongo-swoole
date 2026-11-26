<?php
/**
 * php bench/compare.php php.json node.json [java.json]  ->  markdown table (ops/s, higher is better)
 * Rows are matched by workload name; Java's extra "(platform threads)" rows are listed at the end.
 */
$sets = ['swoole-mongo' => $argv[1], 'Node.js' => $argv[2]] + (isset($argv[3]) ? ['Java' => $argv[3]] : []);
$data = [];
foreach ($sets as $label => $file) {
    foreach (json_decode(file_get_contents($file), true) as $r) $data[$r['name']][$label] = $r;
}
$labels = array_keys($sets);
echo '| Workload | ' . implode(' | ', array_map(fn ($l) => "$l ops/s", $labels)) . ' | ' . implode(' | ', array_map(fn ($l) => "$l p50 / p99 ms", $labels)) . " |\n";
echo '|---|' . str_repeat('---:|', count($labels) * 2) . "\n";
foreach ($data as $name => $row) {
    $best = max(array_map(fn ($r) => $r['opsSec'], $row));
    $ops = $lat = [];
    foreach ($labels as $l) {
        if (!isset($row[$l])) { $ops[] = '–'; $lat[] = '–'; continue; }
        $v = number_format($row[$l]['opsSec']);
        $ops[] = $row[$l]['opsSec'] == $best && count($row) > 1 ? "**$v**" : $v;
        $lat[] = $row[$l]['p50'] . ' / ' . $row[$l]['p99'];
    }
    echo "| $name | " . implode(' | ', $ops) . ' | ' . implode(' | ', $lat) . " |\n";
}
