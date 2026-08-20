# nginx benchmark: fsA vs fsB vs fsBrev

ab (ApacheBench) from the Fedora client against F-Stack nginx 1.28.0 on Morello,
single worker pinned to core 3, files of zeros served from html/.
Cells: {1KB, 48KB, 64KB} x {c1, c8} x {keepalive off, on}.
`nginx_results.csv` holds every cell as one row (fsBrev appears twice,
pass 1 and pass 2; filter `steady_state == 1` for the table below).
fsBrev files are the steady-state second pass (`*_p1.txt` = first pass);
revocation throughput depends on cumulative allocation churn, so fresh-process
numbers are much higher and unrepresentative.

Steady-state requests/sec:

| cell        | fsA (hybrid+sfc) | fsB (purecap+CAPIO) | fsBrev (fsB+revocation) |
|-------------|-----------------:|--------------------:|------------------------:|
| 1k  c1      | 10868 | 9775  | 2224 |
| 1k  c1 ka   | 29750 | 27946 | 3213 |
| 1k  c8      | 25074 | 22178 | 2881 |
| 1k  c8 ka   | 83285 | 69157 | 3268 |
| 48k c1      | 5656  | 5084  | 851  |
| 48k c1 ka   | 8993  | 8164  | 944  |
| 48k c8      | 11231 | 8655  | 940  |
| 48k c8 ka   | 17116 | 12494 | 918  |
| 64k c1      | 410   | 1722  | 970  |
| 64k c1 ka   | 7985  | 7098  | 998  |
| 64k c8      | 3620  | 6995  | 991  |
| 64k c8 ka   | 14278 | 10159 | 1001 |

These fsB/fsBrev columns postdate the CAPIO PMD TX-completion fix; the
originally committed numbers (see git history) were depressed 2-8x on bulk
cells by the PMD recycling in-flight TX buffers. fsA (stock sfc PMD) was
unaffected. With the fix, fsB shows p99 <= 1ms and zero failures in every
cell, and the 64k-with-fresh-connections collapse turns out to be an
fsA-side artifact (410 rps) rather than a shared one (fsB: 1722 rps).

Reading:
- Small responses: fsB reaches 84-96% of the hybrid arm. The CAPIO+purecap
  mechanism is nearly free at HTTP level.
- Bulk responses: fsB tops out around 1.9 Gbit/s (CAPIO TX copy path,
  single core) while fsA reaches wire rate (~7.5 Gbit/s at 64k c8 ka).
- Revocation (fsBrev) costs 3-20x depending on allocation churn, and
  fluctuates bimodally batch-to-batch (~2.7k vs ~3.9k rps at 1k ka).
- The 64k fresh-connection collapse is an fsA-side artifact, since
  diagnosed: on a fresh DUT the cell runs clean (2963 rps, p100 2ms),
  but after ~10^5 requests of connection churn ~1-2% of fresh-connection
  64KB responses silently lose their segment at byte offset 44889
  (exactly 31x1448; deterministic burst depth) and pay one 239 ms
  initial RTO. All retransmissions originate from the server; F-Stack's
  tx_dropped stays 0 (the stack believes the segment was sent), the
  state persists through idle (not TIME_WAIT decay), and only a reboot
  clears it. Stock-sfc-PMD/F-Stack interaction under churn - not CAPIO;
  fsB post-fix measures 1722 rps in the same cell. Keepalive avoids it
  entirely on both arms.
- Zero failed requests in every cell on every arm.

All three arms were only measurable after fixing dead TCP timers in the
F-Stack port (empty callout_when stub: no RTO/delack/keepalive ever armed,
so any packet loss hung the connection forever). See commit message.
