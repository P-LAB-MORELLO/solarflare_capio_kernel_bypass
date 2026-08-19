/*
 * fstack_echo.c — UDP echo server on F-Stack, for the CAPIO-vs-DPDK sweep.
 *
 * Mirrors the semantics of the CAPIO userlib's sockperf_echo: receive a
 * datagram and send it straight back to the sender, unmodified. sockperf
 * ping-pong owns the protocol; this only reflects packets.
 *
 * Usage: fstack_echo --conf <f-stack.conf> --proc-type=primary --proc-id=0 [-p port]
 * The port is taken from FF_ECHO_PORT (default 11111) so the F-Stack argument
 * parser does not have to be extended.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ff_config.h"
#include "ff_api.h"

#define RECV_BUF 2048

/* sockperf MsgHeader flag bits */
#define SPF_MASK_CLIENT  0x0001u
#define SPF_MASK_PONG    0x0002u
#define SPF_MASK_WARMUP  0x0004u

static int sockfd;
static char buf[RECV_BUF];

static int
loop(void *arg)
{
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);

    for (;;) {
        ssize_t n = ff_recvfrom(sockfd, buf, sizeof(buf), 0,
                                (struct linux_sockaddr *)&peer, &peerlen);
        if (n <= 0)
            break;                      /* nothing left this poll cycle */

#ifdef SPF_STRICT
        /* sockperf message header: { uint64 seq BE, uint16 flags BE,
         * uint32 len BE }. The client rejects any reply that still has the
         * CLIENT bit set, and warmup messages must not be answered at all.
         * Same handling as the CAPIO userlib echo, so both arms are equal. */
        if (n < 10)
            continue;
        uint16_t flags = ((uint8_t)buf[8] << 8) | (uint8_t)buf[9];
        if (!(flags & SPF_MASK_CLIENT) || (flags & SPF_MASK_WARMUP) ||
            !(flags & SPF_MASK_PONG))
            continue;
        flags &= (uint16_t)~SPF_MASK_CLIENT;
        buf[8] = (char)(flags >> 8);
        buf[9] = (char)(flags & 0xff);
#else
        /* plain UDP echo: reflect every datagram unmodified. The RFC 2544
         * generator sends unframed UDP, so any sockperf gate here drops
         * 100%% of offered traffic. Matches sockperf_echo_plain. */
#endif

        ssize_t sent = ff_sendto(sockfd, buf, n, 0,
                                 (struct linux_sockaddr *)&peer, peerlen);
        if (sent != n && sent < 0 && errno != EAGAIN)
            fprintf(stderr, "ff_sendto: %s\n", strerror(errno));
    }
    return 0;
}

int
main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    const char *env;
    int port = 11111;

    ff_init(argc, argv);

    env = getenv("FF_ECHO_PORT");
    if (env != NULL)
        port = atoi(env);

    sockfd = ff_socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "ff_socket: %s\n", strerror(errno));
        return 1;
    }

    /* Non-blocking so loop() drains the queue and returns to the poll cycle
     * rather than parking inside recvfrom. */
    int on = 1;
    ff_ioctl(sockfd, FIONBIO, &on);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (ff_bind(sockfd, (struct linux_sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ff_bind(%d): %s\n", port, strerror(errno));
        return 1;
    }

    printf("fstack_echo: UDP echo on port %d\n", port);
    fflush(stdout);

    ff_run(loop, NULL);
    return 0;
}
