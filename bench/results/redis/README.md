# redis benchmark: fsA vs fsB vs fsBrev

F-Stack redis 6.2.6 on Morello (single instance, core 3), three client tools
from the Fedora box over the 10G link. fsBrev was warmed to revocation steady
state (300k ops) before measurement; its throughput oscillates with sweep
cadence (~110k/~33k rps alternating on the warmup probe), so cell results
average over sweep cycles.

## YCSB 0.17.0 (official Java client, redis binding; 50k records, 100k ops, 8 threads)

| workload | fsA | fsB | fsBrev |
|----------|----:|----:|-------:|
| A (50/50 rw) | 77280 | 77101 | 44783 |
| B (95/5)     | 88339 | 73801 | 66225 |
| C (read-only)| 92851 | 72674 | 42265 |
| F (rmw)      | 55804 | 53879 | 48008 |

fsB reaches 99.8% of fsA on workload A and 96.6% on F.

## memtier_benchmark (2 threads x 4 conns, 20s cells, ops/sec totals)

| ratio | value | pipe | fsA | fsB | fsBrev |
|-------|------:|-----:|----:|----:|-------:|
| 1:1 | 100B | 1  | 160383 | 149177 | 68452 |
| 1:1 | 100B | 16 | 612960 | 452621 | 123086 |
| 1:1 | 1KB  | 1  | 177963 | 134548 | 45990 |
| 1:1 | 1KB  | 16 | 353332 | 239512 | 66394 |
| 1:9 | 100B | 1  | 162853 | 146020 | 89725 |
| 1:9 | 100B | 16 | 520153 | 371656 | 155273 |
| 1:9 | 1KB  | 1  | 167989 | 153067 | 79113 |
| 1:9 | 1KB  | 16 | 406208 | 291799 | 126583 |

## redis-benchmark (SET/GET grid in redis_benchmark_results.csv)

fsB = 90% of fsA unpipelined, 60-65% at peak pipelined load; p99 sub-ms in
every cell on fsA/fsB. fsBrev = 30-65% of fsB depending on write share.

These numbers postdate the CAPIO PMD TX-completion fix (see commit message);
without it, pipelined SET workloads collapsed to ~70 rps with corrupted
response bytes on the wire.
