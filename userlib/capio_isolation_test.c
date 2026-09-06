/*
 * capio_isolation_test — proof of concept that two CAPIO devices held by two
 * different processes cannot reach each other.
 *
 *   capio_isolation_test <dev A> <dev B> [map_type on A] [map_type on B]
 *
 * The program attaches to device A (which must be free) and then, holding
 * A's token, tries every way a process could get at device B:
 *
 *   1. CAPIO_ATTACH to B           -> refused while another process holds B
 *   2. MODMAPIOC_MAP on B, A token  -> EPERM: the token does not match B's
 *   3. MODMAPIOC_MAP on B, forged   -> EPERM: an unsealed capability is no token
 *   4. MODMAPIOC_MAP on A, forged   -> EPERM: forgery fails even on own device
 *   5. MODMAPIOC_MAP on A, A token  -> succeeds (control): A's own regions map
 *
 * Every ioctl goes through __sys_ioctl with a fixed three-argument prototype;
 * a variadic declaration garbles the capability under the purecap ABI.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cheriintrin.h>
#include "modmap.h"
#include "capio.h"

extern int __sys_ioctl(int fd, unsigned long request, void *data);

static int pass = 0, fail = 0;

static void
verdict(const char *what, int rc, int err, int expect_ok)
{
    int ok = expect_ok ? (rc == 0) : (rc != 0);
    printf("  %-58s %s  (rc=%d%s%s)\n", what, ok ? "PASS" : "FAIL", rc,
           rc ? " errno=" : "", rc ? strerror(err) : "");
    if (ok) pass++; else fail++;
}

/* One MODMAPIOC_MAP attempt against dev_fd with the given token. */
static int
try_map(int modmap_fd, int dev_fd, int map_type, cap_req_t *tok, int *err)
{
    get_slice_length_t sz = { .fd = dev_fd, .map_type = map_type };
    if (__sys_ioctl(modmap_fd, MODMAPIOC_GET_SLICES, &sz) < 0) { *err = errno; return -1; }

    user_map_req_t mreq = { 0 };
    mreq.user_cap = tok->user_cap;
    mreq.sealed_cap = tok->sealed_cap;
    mreq.map_type = map_type;
    size_t n = sz.region_sizes.slice_def_length;
    mreq.slice_definitions = calloc(n ? n : 1, sizeof(user_slice_def_t));
    mreq.slice_def_length = n;

    mmap_req_user_t req = { 0 };
    req.addr = NULL;
    req.len = sz.region_sizes.region_length;
    req.prot = PROT_READ | PROT_WRITE;
    req.flags = MAP_SHARED;
    req.fd = dev_fd;
    req.pos = 0;
    req.extra = (void * __capability)&mreq;

    int rc = __sys_ioctl(modmap_fd, MODMAPIOC_MAP, &req);
    *err = errno;
    if (rc == 0) {
        printf("      mapped %zu bytes, %zu slices, region cap %#p\n",
               req.len, n, (void *)req.addr);
        if (n) printf("      slice[0] %#p\n", (void *)mreq.slice_definitions[0].addr);
        munmap((void *)req.addr, req.len);
    }
    free(mreq.slice_definitions);
    return rc;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dev A> <dev B> [map_type A] [map_type B]\n", argv[0]);
        return 2;
    }
    const char *devA = argv[1], *devB = argv[2];
    int mtA = argc > 3 ? atoi(argv[3]) : 0;
    int mtB = argc > 4 ? atoi(argv[4]) : 0;

    int modmap_fd = open("/dev/modmap", O_RDWR);
    int fdA = open(devA, O_RDWR);
    int fdB = open(devB, O_RDWR);
    if (modmap_fd < 0 || fdA < 0 || fdB < 0) { perror("open"); return 2; }

    printf("process %d: attaching to A=%s, then probing B=%s\n", getpid(), devA, devB);

    /* Attach to A: the page we pass in becomes the identity the kernel seals. */
    cap_req_t tokA = { 0 };
    tokA.user_cap = malloc(4096);
    int rc = __sys_ioctl(fdA, CAPIO_ATTACH, &tokA);
    verdict("attach to A (must be free)", rc, errno, 1);
    if (rc != 0) return 1;
    printf("      A token: %#p\n", (void *)tokA.sealed_cap);

    /* 1. Attach to B while another process holds it. */
    cap_req_t tokB = { 0 };
    tokB.user_cap = malloc(4096);
    rc = __sys_ioctl(fdB, CAPIO_ATTACH, &tokB);
    verdict("attach to B while another process holds it -> refused", rc, errno, 0);
    int got_B = (rc == 0);

    /* 2. Map a region of B using A's token. */
    int err;
    rc = try_map(modmap_fd, fdB, mtB, &tokA, &err);
    verdict("map B region with A's token -> refused", rc, err, 0);

    /* 3. Map a region of B with a forged token (unsealed capability). */
    cap_req_t forged = { 0 };
    forged.user_cap = malloc(4096);
    forged.sealed_cap = forged.user_cap;          /* not sealed: cannot be a token */
    rc = try_map(modmap_fd, fdB, mtB, &forged, &err);
    verdict("map B region with a forged token -> refused", rc, err, 0);

    /* 4. Forged token against our own device A. */
    rc = try_map(modmap_fd, fdA, mtA, &forged, &err);
    verdict("map A region with a forged token -> refused", rc, err, 0);

    /* 5. Control: A's own token maps A's region. */
    rc = try_map(modmap_fd, fdA, mtA, &tokA, &err);
    verdict("map A region with A's token -> allowed (control)", rc, err, 1);

    /* Cleanup. */
    if (got_B) __sys_ioctl(fdB, CAPIO_GOODBYE, &tokB);
    __sys_ioctl(fdA, CAPIO_GOODBYE, &tokA);
    close(fdB); close(fdA); close(modmap_fd);

    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
