# nginx benchmark: fsA vs fsB vs fsBrev

ab (ApacheBench) from the Fedora client against F-Stack nginx 1.28.0 on Morello,
single worker pinned to core 3, files of zeros served from html/.
Cells: {1KB, 48KB, 64KB} x {c1, c8} x {keepalive off, on}.
fsBrev files are the steady-state second pass (`*_p1.txt` = first pass);
revocation throughput depends on cumulative allocation churn, so fresh-process
numbers are much higher and unrepresentative.

Steady-state requests/sec:

| cell        | fsA (hybrid+sfc) | fsB (purecap+CAPIO) | fsBrev (fsB+revocation) |
|-------------|-----------------:|--------------------:|------------------------:|
| 1k  c1      | 10868 | 9646  | 2247 |
| 1k  c1 ka   | 29750 | 28575 | 3318 |
| 1k  c8      | 25074 | 22254 | 2702 |
| 1k  c8 ka   | 83285 | 69637 | 3353 |
| 48k c1      | 5656  | 5136  | 871  |
| 48k c1 ka   | 8993  | 5853  | 965  |
| 48k c8      | 11231 | 4136  | 962  |
| 48k c8 ka   | 17116 | 4526  | 833  |
| 64k c1      | 410   | 404   | 207  |
| 64k c1 ka   | 7985  | 914   | 1016 |
| 64k c8      | 3620  | 1147  | 566  |
| 64k c8 ka   | 14278 | 3592  | 1015 |

Reading:
- Small responses: fsB reaches 84-96% of the hybrid arm. The CAPIO+purecap
  mechanism is nearly free at HTTP level.
- Bulk responses: fsB tops out around 1.9 Gbit/s (CAPIO TX copy path,
  single core) while fsA reaches wire rate (~7.5 Gbit/s at 64k c8 ka).
- Revocation (fsBrev) costs 3-20x depending on allocation churn, and
  fluctuates bimodally batch-to-batch (~2.7k vs ~3.9k rps at 1k ka).
- 64k with fresh connections collapses on BOTH arms identically
  (410 vs 404 rps: slow-start burst loss + 240 ms initial RTO;
  an F-Stack/harness artifact, not CAPIO).
- Zero failed requests in every cell on every arm.

All three arms were only measurable after fixing dead TCP timers in the
F-Stack port (empty callout_when stub: no RTO/delack/keepalive ever armed,
so any packet loss hung the connection forever). See commit message.
