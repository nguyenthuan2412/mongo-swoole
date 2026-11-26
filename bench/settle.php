<?php
/** Lets the server settle between drivers: flush dirty data to disk, then wait. */
$m = new SwooleMongo\Driver\Manager(getenv('BENCH_URI') ?: 'mongodb://127.0.0.1:27119/?replicaSet=rs0');
$m->executeCommand('admin', new SwooleMongo\Driver\Command(['fsync' => 1]));
sleep((int) (getenv('BENCH_SETTLE') ?: 5));
