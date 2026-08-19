/*
 * sockperf_echo.c — CAPIO/Solarflare userspace sockperf-compatible UDP echoer.
 *
 * Sits on top of the zero-syscall sfc7120_user.h data path (poll / rx_recv /
 * tx_post). Receives UDP packets destined for the local PF, parses the
 * sockperf MsgHeader in the payload, and echoes back only packets that have
 * CLIENT set, WARMUP clear, and PONG set — with the CLIENT bit cleared so the
 * sockperf ping-pong client accepts the reply as a real server response.
 *
 * Usage: ./sockperf_echo [/dev/sfc7120pol1]
 */
#include "sfc7120_user.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define ETH_HDR_LEN  14
#define IP_HDR_LEN   20
#define UDP_HDR_LEN  8
#define SPF_HDR_LEN  14
#define MIN_FRAME_LEN (ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + SPF_HDR_LEN)

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;      /* big-endian */
} __attribute__((packed));

struct ip_hdr {
    uint8_t  vhl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

#pragma pack(push, 2)
struct spf_msg_hdr {
    uint64_t seq;
    uint16_t flags;
    uint32_t length;
};
#pragma pack(pop)

#define SPF_MASK_CLIENT      0x0001u
#define SPF_MASK_PONG        0x0002u
#define SPF_MASK_WARMUP_MSG  0x0004u

static uint16_t
ip_cksum(const void *data, size_t len)
{
    const uint16_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int
main(int argc, char **argv)
{
    const char *devpath = (argc > 1) ? argv[1] : "/dev/sfc7120pol1";
    sfc7120_if_t sfc = { .dev_path = devpath };

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "sfc7120_init(%s) failed\n", devpath);
        return 1;
    }
    fprintf(stderr,
        "capio_sockperf_echo: %s MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
        devpath, sfc.mac_addr[0], sfc.mac_addr[1], sfc.mac_addr[2],
        sfc.mac_addr[3], sfc.mac_addr[4], sfc.mac_addr[5]);

    fprintf(stderr, "slice[DATA_EVQ_RPTR_DBL] = %#p\n",
        (void *)sfc.mmio_slices[SFC7120_SLICE_DATA_EVQ_RPTR_DBL].addr);
    fprintf(stderr, "slice[TX_DESC_DBL]       = %#p\n",
        (void *)sfc.mmio_slices[SFC7120_SLICE_TX_DESC_DBL].addr);
    fprintf(stderr, "mmio region base          = %#p\n",
        (void *)sfc.region_maps[SFC7120_MMIO_REGION].base);
    fflush(stderr);

    /* --- startup TX probe: one broadcast frame so we can verify the
     * TX doorbell + wire path independently of RX. --- */
    {
        /* UDP probe: a valid sockperf CLIENT|PONG message over IPv4/UDP,
         * unicast to the OTHER PF's MAC (last octet toggled). With daemons
         * on both PFs this creates a one-shot cross-PF UDP ping-pong whose
         * timestamps all live on one clock. */
        uint8_t probe[64];
        memset(probe, 0, sizeof(probe));
        memcpy(&probe[0], sfc.mac_addr, 6);
        probe[5] ^= 1;                              /* peer PF MAC */
        memcpy(&probe[6], sfc.mac_addr, 6);         /* src: our MAC  */
        probe[12] = 0x08; probe[13] = 0x00;         /* IPv4 */
        uint8_t *ip = &probe[14];
        ip[0] = 0x45; ip[1] = 0;
        ip[2] = 0; ip[3] = 50;                      /* total len 50 */
        ip[8] = 64; ip[9] = 17;                     /* ttl, UDP */
        ip[12]=10; ip[13]=0; ip[14]=1; ip[15]=3;    /* src 10.0.1.3 */
        ip[16]=10; ip[17]=0; ip[18]=1; ip[19]=4;    /* dst 10.0.1.4 */
        {   /* IP checksum */
            uint32_t sum = 0;
            for (int k = 0; k < 20; k += 2)
                sum += ((uint16_t)ip[k] << 8) | ip[k+1];
            while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
            uint16_t c = ~sum;
            ip[10] = c >> 8; ip[11] = c & 0xff;
        }
        uint8_t *udp = &probe[34];
        udp[0]=0x2b; udp[1]=0x67;                   /* src port 11111 */
        udp[2]=0x2b; udp[3]=0x67;                   /* dst port 11111 */
        udp[4]=0; udp[5]=30;                        /* len 30 */
        uint8_t *msg = &probe[42];
        msg[7] = 1;                                 /* seq=1 (BE u64) */
        msg[8] = 0x00; msg[9] = 0x03;               /* flags CLIENT|PONG */
        msg[13] = 22;                               /* length */
        for (int p50 = 0; p50 < 50; p50++) {
            if (sfc7120_tx_post(&sfc, probe, sizeof(probe)) != 0) {
                fprintf(stderr, "probe %d post FAILED\n", p50);
                break;
            }
        }
        {
            struct timespec _t;
            clock_gettime(CLOCK_MONOTONIC, &_t);
            fprintf(stderr, "50 probes posted mono=%lld.%06ld\n",
                (long long)_t.tv_sec, _t.tv_nsec / 1000);
        }
        fflush(stderr);

        /* Paced cross-PF UDP RTT: send probes at a fixed interval, measure
         * tx_post -> echo-RX round trip in this process. Requires the peer
         * PF daemon running as echo. Prints a percentile summary. */
        if (getenv("SFC_RTT_BENCH") != NULL) {
            int pace_us = atoi(getenv("SFC_RTT_BENCH"));
            enum { NSAMP = 2000 };
            static double rtts[NSAMP];
            int got = 0, lost = 0;
            sfc7120_ev_t bevs[8];
            uint8_t bbuf[2048];

            for (int it = 0; it < NSAMP; it++) {
                struct timespec t0, t1;

                /* 1. Drain every pending event so nothing stale can be
                 *    mistaken for this iteration reply. */
                for (;;) {
                    int dn = sfc7120_poll(&sfc, bevs, 8);
                    if (dn <= 0) break;
                    for (int k = 0; k < dn; k++) {
                        if (bevs[k].type == SFC7120_EV_RX) {
                            size_t bl = sizeof(bbuf);
                            (void)sfc7120_rx_recv(&sfc, bbuf, &bl,
                                                  bevs[k].rx_bytes);
                        }
                    }
                }

                /* 2. Stamp a unique sequence number into the probe. */
                uint64_t seq = (uint64_t)it + 1;
                uint8_t *m = &probe[42];
                for (int b = 0; b < 8; b++)
                    m[b] = (uint8_t)(seq >> (56 - 8 * b));

                clock_gettime(CLOCK_MONOTONIC, &t0);
                if (sfc7120_tx_post(&sfc, probe, sizeof(probe)) != 0) {
                    lost++;
                    continue;
                }

                /* 3. Accept only a server reply carrying OUR sequence:
                 *    CLIENT bit clear, seq matches. */
                int done = 0;
                for (;;) {
                    int dn = sfc7120_poll(&sfc, bevs, 8);
                    for (int k = 0; k < dn && !done; k++) {
                        if (bevs[k].type != SFC7120_EV_RX) continue;
                        size_t bl = sizeof(bbuf);
                        if (sfc7120_rx_recv(&sfc, bbuf, &bl,
                                            bevs[k].rx_bytes) != 0)
                            continue;
                        if (bl < 56) continue;
                        uint8_t *rm = &bbuf[42];
                        uint64_t rseq = 0;
                        for (int b = 0; b < 8; b++)
                            rseq = (rseq << 8) | rm[b];
                        uint16_t rfl = ((uint16_t)rm[8] << 8) | rm[9];
                        if (rseq == seq && !(rfl & 0x1))
                            done = 1;
                    }
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    double el = (t1.tv_sec - t0.tv_sec) * 1e6 +
                                (t1.tv_nsec - t0.tv_nsec) / 1e3;
                    if (done || el > 500000.0) break;   /* 500 ms cap */
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                if (done)
                    rtts[got++] = (t1.tv_sec - t0.tv_sec) * 1e6 +
                                  (t1.tv_nsec - t0.tv_nsec) / 1e3;
                else
                    lost++;
                if (pace_us > 0) usleep(pace_us);
            }

            for (int a = 1; a < got; a++) {
                double v = rtts[a]; int b = a - 1;
                while (b >= 0 && rtts[b] > v) { rtts[b+1] = rtts[b]; b--; }
                rtts[b+1] = v;
            }
            int fast = 0, slow = 0;
            for (int a = 0; a < got; a++) {
                if (rtts[a] < 100.0) fast++; else slow++;
            }
            if (got > 0)
                fprintf(stderr,
                    "RTTBENCH pace=%d n=%d lost=%d fast(<100us)=%d slow=%d "
                    "min=%.1f p25=%.1f p50=%.1f p60=%.1f p75=%.1f p90=%.1f max=%.1f us\n",
                    pace_us, got, lost, fast, slow, rtts[0],
                    rtts[(int)(got*0.25)], rtts[got/2], rtts[(int)(got*0.60)],
                    rtts[(int)(got*0.75)], rtts[(int)(got*0.90)], rtts[got-1]);
            else
                fprintf(stderr, "RTTBENCH pace=%d n=0 lost=%d\n",
                        pace_us, lost);
            fflush(stderr);
            sfc7120_destroy(&sfc);
            return 0;
        }
    }

    uint8_t buf[2048];
    sfc7120_ev_t evs[8];
    const int ev_dbg = (getenv("SFC_EV_DBG") != NULL);
    uint64_t rx = 0, echoed = 0, ignored = 0;

    /* Companion-event keepalive: this firmware coalesces event writes in
     * pairs; a lone event (e.g. the RX event of a solitary ping-pong
     * request) waits ~40 ms for a partner or flush timeout. Posting a tiny
     * dummy TX every ~50 us keeps a continuous companion stream so every
     * real event flushes promptly. Equivalent in spirit to disabling
     * interrupt/event moderation for latency benchmarking. */
    uint8_t keepalive[60];
    memset(keepalive, 0, sizeof(keepalive));
    memcpy(&keepalive[0], sfc.mac_addr, 6);   /* dst: self — MAC drops it */
    memcpy(&keepalive[6], sfc.mac_addr, 6);
    keepalive[12] = 0x88; keepalive[13] = 0xB5;
    struct timespec ka_last;
    clock_gettime(CLOCK_MONOTONIC, &ka_last);

    time_t _lts = 0;
    while (!g_stop) {
        int n = sfc7120_poll(&sfc, evs, 8);
        if (n < 0) {
            fprintf(stderr, "poll error\n");
            break;
        }
        for (int j = 0; j < n; j++) {
            /* Only the fprintf was gated here; clock_gettime and fflush ran on
             * every event regardless. Gate the whole block. */
            if (ev_dbg && evs[j].type == SFC7120_EV_TX) {
                struct timespec _t;
                clock_gettime(CLOCK_MONOTONIC, &_t);
                fprintf(stderr, "TX_EV seen mono=%lld.%06ld\n",
                    (long long)_t.tv_sec, _t.tv_nsec / 1000);
                fflush(stderr);
            }
            if (ev_dbg && evs[j].type == SFC7120_EV_RX) {
                fprintf(stderr, "RX_EV raw=%016llx\n",
                    (unsigned long long)evs[j].raw);
                fflush(stderr);
            }
            if (evs[j].type != SFC7120_EV_RX)
                continue;
            size_t   len = 0;
            uint8_t *pkt = NULL;
            uint64_t pa  = 0;
            /* Zero-copy: work directly in the RX DMA slot and transmit from
             * it, matching DPDK's rte_pktmbuf_mtod path. The descriptor is
             * handed back via sfc7120_rx_release() on EVERY exit path below —
             * miss one and the RX ring starves. */
            if (sfc7120_rx_peek(&sfc, (void **)&pkt, &len, &pa,
                                evs[j].rx_bytes) != 0)
                continue;
            rx++;

            if (len < MIN_FRAME_LEN) { ignored++; sfc7120_rx_release(&sfc); continue; }
            struct eth_hdr *eth = (struct eth_hdr *)pkt;
            if (ntohs(eth->type) != 0x0800) { ignored++; sfc7120_rx_release(&sfc); continue; }

            struct ip_hdr *ip = (struct ip_hdr *)(pkt + ETH_HDR_LEN);
            uint8_t ip_hlen = (ip->vhl & 0x0f) * 4;
            if (ip_hlen < IP_HDR_LEN) { ignored++; sfc7120_rx_release(&sfc); continue; }
            if (ip->proto != 17)      { ignored++; sfc7120_rx_release(&sfc); continue; }
            if (len < (size_t)ETH_HDR_LEN + ip_hlen + UDP_HDR_LEN + SPF_HDR_LEN) {
                ignored++; sfc7120_rx_release(&sfc); continue;
            }

            struct udp_hdr *udp = (struct udp_hdr *)(pkt + ETH_HDR_LEN + ip_hlen);
            struct spf_msg_hdr *msg =
                (struct spf_msg_hdr *)((uint8_t *)udp + UDP_HDR_LEN);

#ifdef SPF_STRICT
            /* Legacy sockperf-aware mode: only echo client PONG requests and
             * clear the CLIENT bit. Kept so the latency campaign binaries stay
             * reproducible; the RFC2544 throughput work uses a plain echo so
             * the generator needs no protocol knowledge. */
            uint16_t flags = ntohs(msg->flags);
            if (!(flags & SPF_MASK_CLIENT))     { ignored++; sfc7120_rx_release(&sfc); continue; }
            if (flags & SPF_MASK_WARMUP_MSG)    { ignored++; sfc7120_rx_release(&sfc); continue; }
            if (!(flags & SPF_MASK_PONG))       { ignored++; sfc7120_rx_release(&sfc); continue; }
            flags &= ~SPF_MASK_CLIENT;
            msg->flags = htons(flags);
#else
            (void)msg;   /* plain UDP echo: reflect every datagram */
#endif

            /* L2 swap */
            uint8_t tmp_mac[6];
            memcpy(tmp_mac, eth->dst, 6);
            memcpy(eth->dst, eth->src, 6);
            memcpy(eth->src, tmp_mac, 6);

            /* L3 swap + recompute IP checksum */
            uint32_t tmp_ip = ip->src;
            ip->src = ip->dst;
            ip->dst = tmp_ip;
            ip->checksum = 0;
            ip->checksum = ip_cksum(ip, ip_hlen);

            /* L4 swap; zero UDP checksum to skip receiver validation */
            uint16_t tmp_port = udp->src_port;
            udp->src_port = udp->dst_port;
            udp->dst_port = tmp_port;
            udp->checksum = 0;

            if (sfc7120_tx_post_paddr(&sfc, pa, len) == 0) {
                echoed++;
                /* Companion frame: this firmware appears to flush event-queue
                 * writes in pairs, so a lone TX completion waits ~43 ms for a
                 * partner. Emitting a second, throwaway frame immediately
                 * gives it one. Addressed to our own MAC so the peer drops
                 * it. Gated by SFC_PAIR_TX so the effect can be measured. */
                if (getenv("SFC_PAIR_TX") != NULL) {
                    uint8_t pad[60];
                    memset(pad, 0, sizeof(pad));
                    memcpy(&pad[0], sfc.mac_addr, 6);
                    memcpy(&pad[6], sfc.mac_addr, 6);
                    pad[12] = 0x88; pad[13] = 0xB5;
                    (void)sfc7120_tx_post(&sfc, pad, sizeof(pad));
                }
            } else
                ignored++;

            /* TX is posted (or failed); the slot can go back to the NIC. */
            sfc7120_rx_release(&sfc);

            /* Per-echo progress logging is a write() syscall per packet on the
             * hot path, and testpmd's sockperfecho does no such thing -- so
             * leaving it on made the two arms do different work. Unconditional
             * and flushed, it put 114 MB / 2.5M lines through the filesystem in
             * a single campaign, a plausible source of tail-latency spikes that
             * leave the median untouched. Gated. */
            if (ev_dbg) {
                struct timespec _rt;
                clock_gettime(CLOCK_REALTIME, &_rt);
                fprintf(stderr, "rx=%llu echoed=%llu t=%lld.%06ld\n",
                    (unsigned long long)rx,
                    (unsigned long long)echoed,
                    (long long)_rt.tv_sec, _rt.tv_nsec / 1000);
                fflush(stderr);
            }
        }
    }

    fprintf(stderr,
        "\nstopped: rx=%llu echoed=%llu ignored=%llu\n",
        (unsigned long long)rx,
        (unsigned long long)echoed,
        (unsigned long long)ignored);
    sfc7120_destroy(&sfc);
    return 0;
}
