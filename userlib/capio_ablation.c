/*
 * capio_ablation.c -- does CAPIO slicing cost anything on the I/O path?
 *
 * Same method as the e1000e / ConnectX-4 ablation in the paper
 * (E1000Lwip/e1000_test_app/src/ablation.c), made device-independent so
 * the Solarflare and NVMe stubs can be measured with one binary.
 *
 * The kernel stub exports a region twice: once sliced (one bounded
 * capability per register / per entry, the normal CAPIO view) and once as
 * a single wide capability over the same bytes (ablation-only twin,
 * enabled with a kenv at module load). This program maps both, builds the
 * table of slice offsets, and times random 4-byte loads:
 *
 *   unsliced:  *(wide_cap + offset[idx])      one cap, address arithmetic
 *   sliced:    *(slice[idx].addr)             CAPIO per-slice cap
 *
 * Both loops do the same table lookup, so the only difference is which
 * capability the load goes through. Seed-42 random index sequence, 65536
 * entries, so the index stream is identical for every path and device.
 *
 * Two loop shapes are timed for each path:
 *   indep: consecutive loads are independent, exactly the loop the paper's
 *          e1000e / mlx5 rows used. The core may keep several device reads
 *          in flight, so this is closer to a throughput number.
 *   chain: the next load's index depends on the value just loaded (masked
 *          with a runtime zero the compiler cannot fold), so each read must
 *          complete before the next issues: true per-access latency.
 *
 * usage: capio_ablation <dev> <sliced_type>:<unsliced_type> [more pairs]
 *        [-n iters] [-r reps]
 *
 *   Solarflare: capio_ablation /dev/sfc7120pol0 2:6
 *   NVMe:       capio_ablation /dev/nvmepol 0:4 1:5     (MMIO, SQ ring)
 *
 * Build on the box:
 *   cc -Wall -O2 -I$HOME/E1000Lwip/include/netif capio_ablation.c -o capio_ablation
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <cheriintrin.h>
#include "modmap.h"
#include "capio.h"

extern int __sys_ioctl(int fd, unsigned long request, void *data);

#define RAND_SEQ_LEN (1u << 16)

static volatile uint32_t zero_v = 0;

typedef struct {
    int               type;
    size_t            len;
    size_t            nslices;
    user_slice_def_t *slices;   /* NULL for the unsliced twin */
    void             *base;     /* region cap; LOAD/STORE stripped when sliced */
} region_t;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int
map_region(int modmap_fd, int dev_fd, cap_req_t *tok, int type, region_t *out)
{
    get_slice_length_t sz = { .fd = dev_fd, .map_type = type };
    if (__sys_ioctl(modmap_fd, MODMAPIOC_GET_SLICES, &sz) < 0) {
        fprintf(stderr, "GET_SLICES type %d: %s\n", type, strerror(errno));
        return -1;
    }
    size_t n = sz.region_sizes.slice_def_length;
    size_t len = sz.region_sizes.region_length;
    if (len == 0) {
        fprintf(stderr, "region type %d has length 0: is the module loaded "
                "with the ablation kenv set?\n", type);
        return -1;
    }

    user_map_req_t mreq = { 0 };
    mreq.user_cap = tok->user_cap;
    mreq.sealed_cap = tok->sealed_cap;
    mreq.map_type = type;
    mreq.slice_definitions = n ? calloc(n, sizeof(user_slice_def_t)) : NULL;
    mreq.slice_def_length = n;

    mmap_req_user_t req = { 0 };
    req.len = len;
    req.prot = PROT_READ | PROT_WRITE;
    req.flags = MAP_SHARED;
    req.fd = dev_fd;
    req.extra = (void * __capability)&mreq;
    if (__sys_ioctl(modmap_fd, MODMAPIOC_MAP, &req) < 0) {
        fprintf(stderr, "MAP type %d: %s\n", type, strerror(errno));
        free(mreq.slice_definitions);
        return -1;
    }
    out->type = type;
    out->len = len;
    out->nslices = n;
    out->slices = mreq.slice_definitions;
    out->base = (void *)req.addr;
    printf("  type %d: %zu bytes, %zu slices, region cap %#p\n",
           type, len, n, out->base);
    return 0;
}

static void
run_pair(int modmap_fd, int dev_fd, cap_req_t *tok, int st, int ut,
         unsigned long iters, int reps)
{
    region_t s = { 0 }, u = { 0 };
    printf("\n== sliced type %d vs unsliced type %d ==\n", st, ut);
    if (map_region(modmap_fd, dev_fd, tok, st, &s) < 0) return;
    if (map_region(modmap_fd, dev_fd, tok, ut, &u) < 0) return;
    if (s.nslices == 0) { fprintf(stderr, "type %d is not sliced\n", st); return; }
    if (u.nslices != 0) { fprintf(stderr, "type %d is sliced\n", ut); return; }
    if (s.len != u.len)
        fprintf(stderr, "warning: region lengths differ (%zu vs %zu)\n", s.len, u.len);

    /* Offsets of every slice from the sliced region base; the unsliced
     * path adds these to its wide capability. */
    size_t n = s.nslices;
    size_t *off = malloc(n * sizeof(size_t));
    ptraddr_t sbase = cheri_address_get(s.base);
    ptraddr_t ubase = cheri_address_get(u.base);
    for (size_t i = 0; i < n; i++) {
        off[i] = cheri_address_get(s.slices[i].addr) - sbase;
        if (i < 8 || i == n - 1)
            printf("  slice[%zu] off 0x%zx len %zu perms %#lx\n", i, off[i],
                   cheri_length_get(s.slices[i].addr),
                   (unsigned long)cheri_perms_get(s.slices[i].addr));
        else if (i == 8)
            printf("  ...\n");
    }
    printf("  sliced base %#lx, unsliced base %#lx (len %zu)\n",
           (unsigned long)sbase, (unsigned long)ubase, u.len);
    printf("  wide cap: len %zu perms %#lx\n", cheri_length_get(u.base),
           (unsigned long)cheri_perms_get(u.base));

    uint32_t *seq = malloc(RAND_SEQ_LEN * sizeof(uint32_t));
    srand(42);
    for (size_t i = 0; i < RAND_SEQ_LEN; i++)
        seq[i] = (uint32_t)(rand() % n);

    volatile uint32_t sink = 0;
    uint8_t *ub = u.base;
    user_slice_def_t *sl = s.slices;
    const uint32_t mask = RAND_SEQ_LEN - 1;
    uint32_t zero = zero_v;            /* runtime 0; keeps the chain honest */
    double sum[2][2] = { { 0, 0 }, { 0, 0 } };   /* [mode][0=unsliced,1=sliced] */

    /* Warm-up: page faults and TLB fills excluded from the timed loops. */
    for (unsigned long i = 0; i < 10000; i++) {
        sink = *(volatile uint32_t *)(ub + off[seq[i & mask]]);
        sink = *(volatile uint32_t *)sl[seq[i & mask]].addr;
    }

    printf("  %-4s %13s %13s | %13s %13s\n", "rep",
           "indep unsl", "indep sliced", "chain unsl", "chain sliced");
    for (int r = 0; r < reps; r++) {
        uint64_t t0 = now_ns();
        for (unsigned long i = 0; i < iters; i++)
            sink = *(volatile uint32_t *)(ub + off[seq[i & mask]]);
        uint64_t t1 = now_ns();
        for (unsigned long i = 0; i < iters; i++)
            sink = *(volatile uint32_t *)sl[seq[i & mask]].addr;
        uint64_t t2 = now_ns();

        uint32_t dep = 0;
        uint64_t t3 = now_ns();
        for (unsigned long i = 0; i < iters; i++) {
            uint32_t v = *(volatile uint32_t *)(ub + off[seq[(i + dep) & mask]]);
            dep = v & zero;
        }
        uint64_t t4 = now_ns();
        for (unsigned long i = 0; i < iters; i++) {
            uint32_t v = *(volatile uint32_t *)sl[seq[(i + dep) & mask]].addr;
            dep = v & zero;
        }
        uint64_t t5 = now_ns();
        sink = dep;

        double v[2][2] = { { (double)(t1 - t0) / iters, (double)(t2 - t1) / iters },
                           { (double)(t4 - t3) / iters, (double)(t5 - t4) / iters } };
        for (int m = 0; m < 2; m++) for (int k = 0; k < 2; k++) sum[m][k] += v[m][k];
        printf("  %-4d %13.3f %13.3f | %13.3f %13.3f\n", r,
               v[0][0], v[0][1], v[1][0], v[1][1]);
    }
    (void)sink;
    static const char *mode_name[2] = { "indep", "chain" };
    for (int m = 0; m < 2; m++) {
        double mu = sum[m][0] / reps, ms = sum[m][1] / reps;
        printf("  %s mean: unsliced %.3f  sliced %.3f  delta %+.3f ns (%+.2f%%)\n",
               mode_name[m], mu, ms, ms - mu, 100.0 * (ms - mu) / mu);
        printf("RESULT mode=%s types=%d:%d n=%zu iters=%lu reps=%d unsliced_ns=%.3f "
               "sliced_ns=%.3f delta_ns=%+.3f delta_pct=%+.2f\n",
               mode_name[m], st, ut, n, iters, reps, mu, ms, ms - mu,
               100.0 * (ms - mu) / mu);
    }

    free(seq); free(off);
    munmap(u.base, u.len);
    munmap(s.base, s.len);
    free(s.slices);
}

int
main(int argc, char **argv)
{
    unsigned long iters = 1000000UL;
    int reps = 5;
    const char *dev = NULL;
    int pairs[8][2], npairs = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) iters = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!dev) dev = argv[i];
        else if (npairs < 8 && sscanf(argv[i], "%d:%d", &pairs[npairs][0], &pairs[npairs][1]) == 2) npairs++;
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    if (!dev || npairs == 0) {
        fprintf(stderr, "usage: %s <dev> <sliced>:<unsliced> [...] [-n iters] [-r reps]\n", argv[0]);
        return 2;
    }

    int modmap_fd = open("/dev/modmap", O_RDWR);
    int fd = open(dev, O_RDWR);
    if (modmap_fd < 0 || fd < 0) { perror("open"); return 2; }

    cap_req_t tok = { 0 };
    tok.user_cap = malloc(4096);
    if (__sys_ioctl(fd, CAPIO_ATTACH, &tok) < 0) { perror("CAPIO_ATTACH"); return 1; }
    printf("attached to %s (%lu iters x %d reps per path)\n", dev, iters, reps);

    for (int i = 0; i < npairs; i++)
        run_pair(modmap_fd, fd, &tok, pairs[i][0], pairs[i][1], iters, reps);

    __sys_ioctl(fd, CAPIO_GOODBYE, &tok);
    close(fd); close(modmap_fd);
    return 0;
}
