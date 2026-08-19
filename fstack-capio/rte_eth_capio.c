/* SPDX-License-Identifier: BSD-3-Clause
 *
 * net_capio — a DPDK vdev PMD backed by the CAPIO userlib (sfc7120_user).
 *
 * Why a PMD rather than patching F-Stack: F-Stack touches the device through
 * ethdev in a dozen places (dev_info, MAC, link, MTU, promiscuous, queue
 * setup), not just the two burst calls. Exposing CAPIO as an ordinary port
 * means F-Stack runs byte-identical code on both arms of the comparison, so
 * the driver is the only variable. Patching F-Stack would have left the two
 * arms free to drift.
 *
 * The device is a real Solarflare bound to the CAPIO kernel stub, not to
 * DPDK, so DPDK sees no PCI device and a vdev is the right vehicle.
 *
 * Zero copy: the NIC DMAs straight into rte_mbuf data via
 * sfc7120_rx_release_paddr(), and transmits straight out of mbuf data via
 * sfc7120_tx_post_paddr(). Neither direction copies the payload, which is what
 * keeps this comparable to a stock PMD.
 *
 * Usage: --vdev=net_capio0[,dev=/dev/sfc7120pol0]
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <ethdev_driver.h>
#include <ethdev_vdev.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_bus_vdev.h>
#include <rte_kvargs.h>
#include <rte_errno.h>

#include "sfc7120_user.h"

#define CAPIO_ARG_DEV       "dev"
#define CAPIO_MAX_BURST     32
/* Room for the EF10 RX prefix the NIC writes ahead of the frame. */
#define CAPIO_RX_PREFIX     SFC7120_EF10_RX_PREFIX_LEN

struct capio_internals;

struct capio_rx_queue {
    struct rte_mbuf        *stash[64];    /* bulk-alloc cache */
    int                     nstash;
    struct capio_internals *internals;
    struct rte_mempool     *mb_pool;
    uint16_t                port_id;
    uint64_t                rx_pkts;
    uint64_t                rx_bytes;
    uint64_t                rx_nombuf;
};

struct capio_tx_queue {
    struct capio_internals *internals;
    /* Mbufs handed to the NIC but not yet reported complete. Freeing at
     * post time would return the buffer to the mempool while the NIC may
     * still be DMAing out of it; RX could then hand the same buffer out and
     * overwrite a frame mid-transmit. Freed on TX completion events instead,
     * which is what a stock PMD does. */
    struct rte_mbuf        *inflight[SFC7120_NUM_TX_DESC];
    uint16_t                last_freed;   /* last TX slot freed (merged completions) */
    uint32_t                tx_tail;
    uint64_t                tx_pkts;
    uint64_t                tx_bytes;
    uint64_t                tx_dropped;
};

struct capio_internals {
    int                     tx_copy_mode;
    sfc7120_if_t          sfc;
    bool                  started;
    char                  dev_path[64];
    struct capio_rx_queue rxq;
    struct capio_tx_queue txq;
    struct rte_ether_addr  mac;
};

static struct rte_eth_link capio_link = {
    .link_speed   = RTE_ETH_SPEED_NUM_10G,
    .link_duplex  = RTE_ETH_LINK_FULL_DUPLEX,
    .link_status  = RTE_ETH_LINK_DOWN,
    .link_autoneg = RTE_ETH_LINK_FIXED,
};

RTE_LOG_REGISTER_DEFAULT(eth_capio_logtype, NOTICE);
#define RTE_LOGTYPE_ETH_CAPIO eth_capio_logtype
#define PMD_LOG(level, ...) \
    RTE_LOG_LINE_PREFIX(level, ETH_CAPIO, "%s(): ", __func__, __VA_ARGS__)

/*
 * Post one mbuf into the RX ring. The NIC writes the EF10 prefix at the start
 * of the buffer, so the frame itself lands at CAPIO_RX_PREFIX; hand the NIC
 * the buffer base and account for the prefix on the way back out.
 */
static inline int
capio_post_mbuf(struct capio_internals *pi, struct rte_mbuf *m)
{
    /* Post at the default data offset, so the prefix lands there and the frame
     * at +CAPIO_RX_PREFIX. Space available from that point is the tailroom;
     * counting headroom too would overstate it by data_off bytes. */
    uint64_t iova = rte_mbuf_data_iova_default(m);
    uint16_t room = rte_pktmbuf_tailroom(m);

    if (room > SFC7120_RX_BUFFER_SIZE)
        room = SFC7120_RX_BUFFER_SIZE;

    return sfc7120_rx_release_paddr_nodbl(&pi->sfc, iova, room, m);
}

static inline struct rte_mbuf *
capio_mbuf_get(struct capio_rx_queue *rxq)
{
    if (rxq->nstash == 0) {
        if (rte_pktmbuf_alloc_bulk(rxq->mb_pool, rxq->stash, 64) != 0) {
            /* bulk failed; try a single as a last resort */
            return rte_pktmbuf_alloc(rxq->mb_pool);
        }
        rxq->nstash = 64;
    }
    return rxq->stash[--rxq->nstash];
}

static uint16_t
eth_capio_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
    struct capio_rx_queue *rxq = q;
    struct capio_internals *pi;
    sfc7120_ev_t evs[CAPIO_MAX_BURST];
    uint16_t n_out = 0;
    int n_ev, i;

    if (unlikely(q == NULL || bufs == NULL || nb_bufs == 0))
        return 0;

    pi = rxq->internals;
    if (nb_bufs > CAPIO_MAX_BURST)
        nb_bufs = CAPIO_MAX_BURST;

    /* poll() is the sole EVQ reader; RX events tell us how many slots the NIC
     * has completed and how many bytes each holds. */
    n_ev = sfc7120_poll(&pi->sfc, evs, nb_bufs);
    if (n_ev <= 0)
        return 0;

    for (i = 0; i < n_ev; i++) {
        struct rte_mbuf *m, *repl;
        void *cookie = NULL;
        size_t len = 0;

        /* heartbeat moved here: the old placement sat after this continue,
         * so TX events were invisible (ev_tx always read 0). */
        {
            static int stats_on2 = -1;
            static time_t last2;
            static uint64_t hb_rx, hb_tx, hb_oth;
            if (stats_on2 < 0) stats_on2 = (getenv("CAPIO_STATS") != NULL);
            if (evs[i].type == SFC7120_EV_RX) hb_rx++;
            else if (evs[i].type == SFC7120_EV_TX) hb_tx++;
            else hb_oth++;
            if (stats_on2) {
                time_t now2 = time(NULL);
                if (now2 != last2) {
                    last2 = now2;
                    fprintf(stderr, "HB rx=%lu tx=%lu oth=%lu txp=%lu lastfree=%u txh=%u\n",
                        (unsigned long)hb_rx, (unsigned long)hb_tx, (unsigned long)hb_oth,
                        (unsigned long)pi->txq.tx_pkts,
                        (unsigned)pi->txq.last_freed,
                        (unsigned)pi->sfc.tx_head);
                    fflush(stderr);
                }
            }
        }
        if (evs[i].type != SFC7120_EV_RX) {
            /* Reap TX completions: everything up to the reported slot is done
             * with, so its buffer can go back to the pool. */
            if (evs[i].type == SFC7120_EV_TX) {
                struct capio_tx_queue *txq = &pi->txq;
                while (txq->tx_tail != pi->sfc.tx_head) {
                    uint32_t s = txq->tx_tail & (SFC7120_NUM_TX_DESC - 1);
                    /* Completions are merged: the event carries only the
                     * last completed index. Free the whole range since the
                     * previous completion, or the merged slots leak and the
                     * mempool drains until RX allocation fails. */
                    {
                        uint16_t f = txq->last_freed;
                        while (f != s) {
                            f = (f + 1) & (SFC7120_NUM_TX_DESC - 1);
                            if (txq->inflight[f] != NULL) {
                                rte_pktmbuf_free(txq->inflight[f]);
                                txq->inflight[f] = NULL;
                            }
                        }
                        txq->last_freed = s;
                    }
                    txq->tx_tail = (txq->tx_tail + 1) &
                                   (SFC7120_NUM_TX_DESC - 1);
                }
            }
            continue;
        }

        /* CAPIO_STATS=1: 1-second counter heartbeat for wedge autopsies */
        {
            static int stats_on = -1;
            static time_t last;
            static uint64_t ev_rx, ev_tx, ev_other;
            if (stats_on < 0) stats_on = (getenv("CAPIO_STATS") != NULL);
            if (evs[i].type == SFC7120_EV_RX) ev_rx++;
            else if (evs[i].type == SFC7120_EV_TX) ev_tx++;
            else ev_other++;
            if (stats_on) {
                time_t now = time(NULL);
                if (now != last) {
                    last = now;
                    fprintf(stderr, "CSTAT ev_rx=%lu ev_tx=%lu ev_o=%lu rxp=%lu nombuf=%lu txp=%lu txd=%lu rxh=%u txh=%u stash=%d\n",
                        (unsigned long)ev_rx, (unsigned long)ev_tx, (unsigned long)ev_other,
                        (unsigned long)rxq->rx_pkts, (unsigned long)rxq->rx_nombuf,
                        (unsigned long)pi->txq.tx_pkts, (unsigned long)pi->txq.tx_dropped,
                        (unsigned)pi->sfc.rx_head, (unsigned)pi->sfc.tx_head, rxq->nstash);
                    fflush(stderr);
                }
            }
        }
        if (sfc7120_rx_peek_ext(&pi->sfc, &cookie, &len, evs[i].rx_bytes) != 0)
            break;

        /* A replacement buffer must be available before the slot is given
         * back, or the ring runs dry. If allocation fails, drop this packet
         * and re-post its own buffer rather than losing a descriptor. */
        repl = capio_mbuf_get(rxq);
        if (unlikely(repl == NULL)) {
            rxq->rx_nombuf++;
            if (cookie != NULL)
                capio_post_mbuf(pi, (struct rte_mbuf *)cookie);
            else
                sfc7120_rx_release(&pi->sfc);
            continue;
        }

        /* RX is copy-mode: identity descriptors stay in the ring forever
         * (their re-post rewrites the same bytes, so the NIC's descriptor
         * cache can never go stale) and each frame is copied into a fresh
         * mbuf. Zero-copy mbuf swapping rewrites descriptors the doorbell
         * already covered -- including the 511 the kernel priming advertised
         * at attach -- and EF10 may use its cached copy, filling the OLD
         * buffer while the cookie names the new one: stale replies (lag) or
         * unparseable ones (wedge), mode chosen by pool-recycling distance.
         * CAPIO_RX_ZC=1 re-enables the swap path for future work.
         * The memcpy is noise against the stack's per-packet cost. */
        {
            static int rx_zc = -1;
            if (rx_zc < 0) rx_zc = (getenv("CAPIO_RX_COPY") != NULL);
            if (rx_zc) cookie = NULL;
        }
        if (cookie == NULL) {
            /* First lap after init: this slot is still a library buffer, so
             * copy out. Steady state never takes this branch. */
            void *pkt = NULL;
            size_t plen = 0;
            if (sfc7120_rx_peek(&pi->sfc, &pkt, &plen,
                                NULL, evs[i].rx_bytes) != 0 || plen == 0) {
                rte_pktmbuf_free(repl);
                sfc7120_rx_release(&pi->sfc);
                continue;
            }
            m = repl;
            repl = capio_mbuf_get(rxq);
            if (unlikely(repl == NULL)) {
                rxq->rx_nombuf++;
                rte_pktmbuf_free(m);
                sfc7120_rx_release(&pi->sfc);
                continue;
            }
            rte_memcpy(rte_pktmbuf_mtod(m, void *), pkt, plen);
            len = plen;
            /* copy-mode: the slot keeps its identity descriptor; re-post it
             * (same bytes, cache-safe) and return the spare mbuf. No mbuf
             * descriptor ever enters the ring. */
            {
                static int rx_zc2 = -1;
                if (rx_zc2 < 0) rx_zc2 = (getenv("CAPIO_RX_COPY") != NULL);
                if (rx_zc2) {
                    rte_pktmbuf_free(repl);
                    sfc7120_rx_release(&pi->sfc);
                    m->pkt_len = m->data_len = (uint16_t)len;
                    m->nb_segs = 1;
                    m->next = NULL;
                    m->port = rxq->port_id;
                    m->ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD |
                                   RTE_MBUF_F_RX_L4_CKSUM_GOOD;
                    bufs[n_out++] = m;
                    rxq->rx_pkts++;
                    rxq->rx_bytes += len;
                    continue;
                }
            }
        } else {
            /* Steady state: the NIC wrote directly into this mbuf. Skip the
             * prefix it laid down ahead of the frame. */
            m = (struct rte_mbuf *)cookie;
            m->data_off += CAPIO_RX_PREFIX;
        }

        if (unlikely(len == 0)) {
            rte_pktmbuf_free(m);
            capio_post_mbuf(pi, repl);
            continue;
        }

        m->ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD |
                       RTE_MBUF_F_RX_L4_CKSUM_GOOD;
        m->pkt_len = m->data_len = (uint16_t)len;
        m->nb_segs = 1;
        m->next = NULL;
        m->port = rxq->port_id;

        capio_post_mbuf(pi, repl);

        bufs[n_out++] = m;
        rxq->rx_pkts++;
        rxq->rx_bytes += len;
    }

    /* one doorbell + dsb per burst instead of per packet */
    if (n_ev > 0)
        sfc7120_rx_flush(&pi->sfc);

    return n_out;
}

static uint16_t
eth_capio_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
    struct capio_tx_queue *txq = q;
    struct capio_internals *pi;
    uint16_t i;

    if (unlikely(q == NULL || bufs == NULL))
        return 0;

    pi = txq->internals;

    for (i = 0; i < nb_bufs; i++) {
        struct rte_mbuf *m = bufs[i];
        uint32_t len = rte_pktmbuf_pkt_len(m);

        /* Single-segment only: the CAPIO TX descriptor takes one bus address.
         * F-Stack is configured without TX_MULTI_SEGS, so this holds. */
        if (unlikely(m->nb_segs != 1)) {
            txq->tx_dropped++;
            rte_pktmbuf_free(m);
            continue;
        }

        /* EF10 does not auto-pad runts. Short frames (ARP is 42B) must be
         * padded to the 60B minimum or the peer NIC discards them and ARP
         * never resolves. Rare frames, so the copy is free. */
#define CAPIO_MIN_TX_FRAME 60
        if (len < CAPIO_MIN_TX_FRAME) {
            uint8_t padbuf[CAPIO_MIN_TX_FRAME];
            memset(padbuf, 0, sizeof(padbuf));
            rte_memcpy(padbuf, rte_pktmbuf_mtod(m, void *), len);
            if (sfc7120_tx_post(&pi->sfc, padbuf, CAPIO_MIN_TX_FRAME) != 0) {
                txq->tx_dropped++;
                rte_pktmbuf_free(m);
                break;
            }
            rte_pktmbuf_free(m);
            txq->tx_pkts++;
            txq->tx_bytes += CAPIO_MIN_TX_FRAME;
            continue;
        }

        if (pi->tx_copy_mode) {
            /* Diagnostic: copy into the identity TX slot and free now. No
             * dependence on mbuf iova or lifetime. */
            if (sfc7120_tx_post(&pi->sfc,
                                rte_pktmbuf_mtod(m, void *), len) != 0) {
                txq->tx_dropped++;
                rte_pktmbuf_free(m);
                break;
            }
            rte_pktmbuf_free(m);
            txq->tx_pkts++;
            txq->tx_bytes += len;
            continue;
        }

        /* tx_post_paddr advances tx_head; the descriptor this mbuf rides is
         * the PRE-advance index. Recording at the post-advance slot put every
         * mbuf one slot past its own descriptor. */
        uint32_t tx_slot = pi->sfc.tx_head & (SFC7120_NUM_TX_DESC - 1);
        if (sfc7120_tx_post_paddr_nodbl(&pi->sfc, rte_mbuf_data_iova(m), len) != 0) {
            txq->tx_dropped++;
            rte_pktmbuf_free(m);
            break;
        }

        txq->inflight[tx_slot] = m;
        txq->tx_pkts++;
        txq->tx_bytes += len;
    }

    if (i > 0)
        sfc7120_tx_flush(&pi->sfc);

    return i;
}

static int
eth_dev_configure(struct rte_eth_dev *dev __rte_unused)
{
    return 0;
}

static int
eth_dev_start(struct rte_eth_dev *dev)
{
    struct capio_internals *pi = dev->data->dev_private;
    uint16_t i;

    /* Device was initialised at probe; just refresh the exported MAC. */
    rte_ether_addr_copy(&pi->mac, dev->data->mac_addrs);

    dev->data->dev_link.link_status = RTE_ETH_LINK_UP;
    for (i = 0; i < dev->data->nb_rx_queues; i++)
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    for (i = 0; i < dev->data->nb_tx_queues; i++)
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    return 0;
}

static int
eth_dev_stop(struct rte_eth_dev *dev)
{
    uint16_t i;

    dev->data->dev_link.link_status = RTE_ETH_LINK_DOWN;
    for (i = 0; i < dev->data->nb_rx_queues; i++)
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
    for (i = 0; i < dev->data->nb_tx_queues; i++)
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
    return 0;
}

static int
eth_dev_close(struct rte_eth_dev *dev)
{
    struct capio_internals *pi = dev->data->dev_private;

    if (rte_eal_process_type() != RTE_PROC_PRIMARY)
        return 0;

    if (pi->started) {
        sfc7120_destroy(&pi->sfc);
        pi->started = false;
    }
    return 0;
}

static int
eth_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
                   uint16_t nb_rx_desc __rte_unused,
                   unsigned int socket_id __rte_unused,
                   const struct rte_eth_rxconf *rx_conf __rte_unused,
                   struct rte_mempool *mb_pool)
{
    struct capio_internals *pi = dev->data->dev_private;

    if (rx_queue_id != 0)
        return -ENODEV;

    pi->rxq.internals = pi;
    pi->rxq.mb_pool = mb_pool;
    pi->rxq.port_id = dev->data->port_id;
    dev->data->rx_queues[rx_queue_id] = &pi->rxq;
    return 0;
}

static int
eth_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
                   uint16_t nb_tx_desc __rte_unused,
                   unsigned int socket_id __rte_unused,
                   const struct rte_eth_txconf *tx_conf __rte_unused)
{
    struct capio_internals *pi = dev->data->dev_private;

    if (tx_queue_id != 0)
        return -ENODEV;

    pi->txq.internals = pi;
    dev->data->tx_queues[tx_queue_id] = &pi->txq;
    return 0;
}

static void
eth_queue_release(struct rte_eth_dev *dev __rte_unused,
                  uint16_t qid __rte_unused)
{
}

static int
eth_dev_info(struct rte_eth_dev *dev __rte_unused,
             struct rte_eth_dev_info *dev_info)
{
    dev_info->max_mac_addrs   = 1;
    dev_info->max_rx_queues   = 1;
    dev_info->max_tx_queues   = 1;
    dev_info->min_rx_bufsize  = 0;
    dev_info->max_rx_pktlen   = SFC7120_RX_BUFFER_SIZE - CAPIO_RX_PREFIX;
    dev_info->max_mtu         = dev_info->max_rx_pktlen - RTE_ETHER_HDR_LEN -
                                RTE_ETHER_CRC_LEN;
    dev_info->min_mtu         = RTE_ETHER_MIN_MTU;
    /* TXQ is created with hardware checksum insertion (INIT_TXQ csum
     * flags 0), so the NIC checksums every TX packet; advertising the
     * offload lets the stack skip software in_cksum. RX: EF10 always
     * verifies checksums in hardware (cannot be disabled), so flag them
     * good instead of re-verifying in software. */
    dev_info->rx_offload_capa = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
                                RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
                                RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
    dev_info->tx_offload_capa = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                                RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                                RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
    return 0;
}

static int
eth_mtu_set(struct rte_eth_dev *dev __rte_unused, uint16_t mtu __rte_unused)
{
    return 0;
}

static int
eth_link_update(struct rte_eth_dev *dev,
                int wait_to_complete __rte_unused)
{
    dev->data->dev_link.link_status = RTE_ETH_LINK_UP;
    return 0;
}

static int
eth_mac_address_set(struct rte_eth_dev *dev __rte_unused,
                    struct rte_ether_addr *addr __rte_unused)
{
    return 0;
}

static int
eth_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
    struct capio_internals *pi = dev->data->dev_private;

    stats->ipackets  = pi->rxq.rx_pkts;
    stats->ibytes    = pi->rxq.rx_bytes;
    stats->rx_nombuf = pi->rxq.rx_nombuf;
    stats->opackets  = pi->txq.tx_pkts;
    stats->obytes    = pi->txq.tx_bytes;
    stats->oerrors   = pi->txq.tx_dropped;
    return 0;
}

static int
eth_stats_reset(struct rte_eth_dev *dev)
{
    struct capio_internals *pi = dev->data->dev_private;

    memset(&pi->rxq.rx_pkts, 0, sizeof(uint64_t) * 3);
    memset(&pi->txq.tx_pkts, 0, sizeof(uint64_t) * 3);
    return 0;
}

static int
eth_promiscuous_enable(struct rte_eth_dev *dev __rte_unused)
{
    return 0;
}

static const struct eth_dev_ops ops = {
    .dev_start          = eth_dev_start,
    .dev_stop           = eth_dev_stop,
    .dev_close          = eth_dev_close,
    .dev_configure      = eth_dev_configure,
    .dev_infos_get      = eth_dev_info,
    .rx_queue_setup     = eth_rx_queue_setup,
    .tx_queue_setup     = eth_tx_queue_setup,
    .rx_queue_release   = eth_queue_release,
    .tx_queue_release   = eth_queue_release,
    .mtu_set            = eth_mtu_set,
    .link_update        = eth_link_update,
    .mac_addr_set       = eth_mac_address_set,
    .promiscuous_enable = eth_promiscuous_enable,
    .stats_get          = eth_stats_get,
    .stats_reset        = eth_stats_reset,
};

static int
eth_dev_capio_create(struct rte_vdev_device *dev, const char *dev_path)
{
    struct rte_eth_dev *eth_dev;
    struct capio_internals *pi;

    eth_dev = rte_eth_vdev_allocate(dev, sizeof(*pi));
    if (eth_dev == NULL)
        return -ENOMEM;

    pi = eth_dev->data->dev_private;
    memset(pi, 0, sizeof(*pi));
    if (dev_path != NULL)
        strlcpy(pi->dev_path, dev_path, sizeof(pi->dev_path));

    /* Initialise the CAPIO device at probe time: F-Stack reads the port MAC
     * before dev_start runs, and a zero MAC poisons its ARP replies. */
    pi->sfc.dev_path = pi->dev_path[0] ? pi->dev_path : NULL;
    if (sfc7120_init(&pi->sfc) != 0) {
        PMD_LOG(ERR, "sfc7120_init failed for %s",
                pi->dev_path[0] ? pi->dev_path : "(default)");
    pi->tx_copy_mode = (getenv("CAPIO_TX_COPY") != NULL);
        rte_eth_dev_release_port(eth_dev);
        return -EIO;
    }
    memcpy(pi->mac.addr_bytes, pi->sfc.mac_addr, RTE_ETHER_ADDR_LEN);
    pi->started = true;

    eth_dev->data->nb_rx_queues = 1;
    eth_dev->data->nb_tx_queues = 1;
    eth_dev->data->dev_link     = capio_link;
    eth_dev->data->mac_addrs    = &pi->mac;
    eth_dev->data->promiscuous  = 1;
    eth_dev->data->all_multicast = 1;
    eth_dev->dev_ops            = &ops;
    eth_dev->rx_pkt_burst       = eth_capio_rx;
    eth_dev->tx_pkt_burst       = eth_capio_tx;

    rte_eth_dev_probing_finish(eth_dev);
    return 0;
}

static int
capio_parse_dev(const char *key __rte_unused, const char *value, void *extra)
{
    const char **out = extra;

    *out = value;
    return 0;
}

static int
rte_pmd_capio_probe(struct rte_vdev_device *dev)
{
    static const char *const valid_args[] = { CAPIO_ARG_DEV, NULL };
    const char *name = rte_vdev_device_name(dev);
    const char *params = rte_vdev_device_args(dev);
    const char *dev_path = NULL;
    struct rte_kvargs *kvlist = NULL;
    int ret;

    PMD_LOG(INFO, "initialising %s", name);

    if (params != NULL && params[0] != '\0') {
        kvlist = rte_kvargs_parse(params, valid_args);
        if (kvlist == NULL)
            return -EINVAL;
        rte_kvargs_process(kvlist, CAPIO_ARG_DEV, capio_parse_dev, &dev_path);
    }

    ret = eth_dev_capio_create(dev, dev_path);
    rte_kvargs_free(kvlist);
    return ret;
}

static int
rte_pmd_capio_remove(struct rte_vdev_device *dev)
{
    struct rte_eth_dev *eth_dev;

    eth_dev = rte_eth_dev_allocated(rte_vdev_device_name(dev));
    if (eth_dev == NULL)
        return 0;

    eth_dev_close(eth_dev);
    rte_eth_dev_release_port(eth_dev);
    return 0;
}

static struct rte_vdev_driver pmd_capio_drv = {
    .probe  = rte_pmd_capio_probe,
    .remove = rte_pmd_capio_remove,
};

RTE_PMD_REGISTER_VDEV(net_capio, pmd_capio_drv);
RTE_PMD_REGISTER_ALIAS(net_capio, eth_capio);
RTE_PMD_REGISTER_PARAM_STRING(net_capio, CAPIO_ARG_DEV "=<path>");
