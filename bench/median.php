<?php
/** php bench/median.php out.json run1.json run2.json ...  -> per workload median of ops/s (and of p50 / p99) */
[$target, $runs] = [$_SERVER['argv'][1], array_slice($_SERVER['argv'], 2)];
$by = [];
foreach ($runs as $f) foreach (json_decode(file_get_contents($f), true) as $r) $by[$r['name']][] = $r;
$med = function (array $v) { sort($v); return $v[intdiv(count($v), 2)]; };
$res = [];
foreach ($by as $name => $rs) {
    $res[] = ['name' => $name, 'ops' => $rs[0]['ops'], 'runs' => count($rs),
        'opsSec' => $med(array_column($rs, 'opsSec')),
        'p50' => number_format($med(array_map('floatval', array_column($rs, 'p50'))), 3, '.', ''),
        'p99' => number_format($med(array_map('floatval', array_column($rs, 'p99'))), 3, '.', '')];
}
file_put_contents($target, json_encode($res));
