/*
 * kern_echo.c — sockperf-protocol UDP echo server over ordinary BSD
 * sockets. This is the kernel-path control for the CAPIO benchmark: it
 * runs the SAME application logic as the CAPIO (sockperf_echo.c) and
 * DPDK (testpmd sockperfecho) echoers, so the only difference between
 * the three measurements is the I/O path.
 *
 * Reply rule matches the others: echo only messages with CLIENT set,
 * WARMUP clear, PONG set — with the CLIENT bit cleared so the sockperf
 * client accepts the datagram as a server response.
 *
 * usage: ./kern_echo [bind_ip] [port]
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#pragma pack(push, 2)
struct spf_msg_hdr {
    uint64_t seq;      /* big-endian */
    uint16_t flags;    /* big-endian */
    uint32_t length;   /* big-endian */
};
#pragma pack(pop)

#define SPF_MASK_CLIENT     0x0001u
#define SPF_MASK_PONG       0x0002u
#define SPF_MASK_WARMUP_MSG 0x0004u

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

int
main(int argc, char **argv)
{
    const char *ip   = (argc > 1) ? argv[1] : "0.0.0.0";
    int         port = (argc > 2) ? atoi(argv[2]) : 11111;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family      = AF_INET;
    me.sin_port        = htons((uint16_t)port);
    me.sin_addr.s_addr = inet_addr(ip);
    if (bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0) {
        perror("bind"); return 1;
    }
    fprintf(stderr, "kern_echo: listening on %s:%d (kernel sockets)\n", ip, port);

    uint8_t buf[2048];
    unsigned long long rx = 0, echoed = 0, ignored = 0;

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 0) {
            if (g_stop) break;
            continue;
        }
        rx++;
        if ((size_t)n < sizeof(struct spf_msg_hdr)) { ignored++; continue; }

        struct spf_msg_hdr *m = (struct spf_msg_hdr *)buf;
        uint16_t flags = ntohs(m->flags);
        if (!(flags & SPF_MASK_CLIENT))  { ignored++; continue; }
        if (flags & SPF_MASK_WARMUP_MSG) { ignored++; continue; }
        if (!(flags & SPF_MASK_PONG))    { ignored++; continue; }

        flags &= ~SPF_MASK_CLIENT;      /* becomes a server response */
        m->flags = htons(flags);

        if (sendto(fd, buf, (size_t)n, 0,
                   (struct sockaddr *)&peer, plen) == n)
            echoed++;
        else
            ignored++;
    }

    fprintf(stderr, "\nkern_echo: rx=%llu echoed=%llu ignored=%llu\n",
            rx, echoed, ignored);
    close(fd);
    return 0;
}
