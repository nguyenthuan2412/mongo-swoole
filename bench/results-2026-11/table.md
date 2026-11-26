| Workload | swoole-mongo ops/s | Node.js ops/s | Java ops/s | swoole-mongo p50 / p99 ms | Node.js p50 / p99 ms | Java p50 / p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| insertOne (sequential) | **710** | 680 | 586 | 1.104 / 5.464 | 1.169 / 5.832 | 1.158 / 8.005 |
| findOne by field (sequential) | **954** | 762 | 776 | 0.841 / 4.188 | 1.190 / 3.954 | 1.130 / 4.637 |
| findOne indexed (sequential) | **1,771** | 1,767 | 1,623 | 0.426 / 2.022 | 0.407 / 2.193 | 0.376 / 3.821 |
| updateOne indexed (sequential) | 633 | **669** | 549 | 1.175 / 5.890 | 1.117 / 5.745 | 1.358 / 6.648 |
| find 100 docs toArray | **1,078** | 676 | 904 | 0.745 / 2.586 | 1.213 / 4.763 | 0.963 / 4.297 |
| find all 5000 docs (iterate) | **52** | 32 | 43 | 18.653 / 29.062 | 32.023 / 46.135 | 22.989 / 38.508 |
| insertMany 1000 docs | **84** | 53 | 41 | 11.130 / 29.016 | 16.660 / 42.074 | 24.066 / 43.345 |
| aggregate $group | **79** | 65 | 62 | 12.362 / 27.482 | 14.269 / 34.505 | 14.270 / 47.813 |
| findOne concurrent x100 | **27,475** | 15,636 | 12,379 | 2.649 / 14.189 | 5.585 / 19.966 | 5.636 / 42.603 |
| insertOne concurrent x100 | **12,868** | 10,241 | 9,530 | 7.179 / 19.026 | 9.007 / 23.431 | 9.560 / 30.409 |
| findOne concurrent x100 (platform threads) | – | – | 17,305 | – | – | 4.162 / 30.329 |
| insertOne concurrent x100 (platform threads) | – | – | 10,027 | – | – | 8.564 / 32.256 |
