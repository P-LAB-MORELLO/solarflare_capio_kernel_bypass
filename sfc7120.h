#ifndef SFC7120_POL_H
#define SFC7120_POL_H

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include <sys/bus.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/ioccom.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/rwlock.h>
#include <sys/taskqueue.h>
#include <sys/selinfo.h>
#include <sys/poll.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/param.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <cheri/cheric.h>
#include <cheri/cheri.h>

#include "capio.h"

/*
 * Solarflare SFN7000-series (Huntington / EF10) is event-driven, not
 * descriptor-DD-driven. The classic queue layout:
 *
 *   - One Event Queue (EVQ) per channel: hardware writes events here
 *   - One TX queue per channel: descriptors point at TX packet buffers
 *   - One RX queue per channel: descriptors point at RX packet buffers
 *
 * For a first stub we expose a single channel (q0) with one EVQ + TX + RX,
 * mirroring the e1000 stub's 3-region split. Multi-queue (mlx5pol-style) can
 * be added later by extending the region enum and softc.
 */

/* EF10 requires each descriptor ring to be at least one full 4K page. With
 * 8-byte descriptors that's 512 entries minimum — anything smaller makes
 * INIT_RXQ/INIT_TXQ EINVAL. */
#define SFC7120_NUM_TX_DESC    512
#define SFC7120_NUM_RX_DESC    512
#define SFC7120_NUM_EVQ_ENTRY  512
#define SFC7120_TX_BUFFER_SIZE 2048
#define SFC7120_RX_BUFFER_SIZE 2048

#define SFC7120_EVQ_ENTRY_SIZE 8        /* EF10 events are 64-bit */
#define SFC7120_TX_DESC_SIZE   8        /* 8-byte TX descriptor */
#define SFC7120_RX_DESC_SIZE   8        /* 8-byte RX descriptor */

typedef enum {
    /* Order and count must match sfc7120_vm_map_type_t in
     * sfc7120_uapi.h — userspace indexes smem by these enum values. */
    SFC7120_TX_BUFFER,        /* 0 */
    SFC7120_RX_BUFFER,        /* 1 */
    SFC7120_MMIO_REGION,      /* 2 */
    SFC7120_TX_DESC_RING,     /* 3 — TX descriptor ring (4 KB) */
    SFC7120_RX_DESC_RING,     /* 4 — RX descriptor ring (4 KB) */
    SFC7120_EVQ_RING,         /* 5 — data EVQ ring (4 KB) */
    SFC7120_REGION_COUNT      /* keep last */
} sfc7120_vm_map_type_t;

/* User-facing IOCTL request structs.
 * Every CAPIO IOCTL struct MUST begin with user_cap + sealed_cap so that
 * capio_ioctl_handler can validate the token at offset 0. */

typedef struct sfc7120_mac_req {
    void* __capability user_cap;
    void* __capability sealed_cap;

    uint8_t mac_addr[6];
} sfc7120_mac_req_t;

typedef struct sfc7120_user_packet_descriptor {
    uint8_t *start_ptr;
    size_t   length;
} sfc7120_user_packet_descriptor_t;

typedef struct sfc7120_tx_req {
    void* __capability user_cap;
    void* __capability sealed_cap;

    uint8_t* __capability tx_buf_addr;
    size_t  length;
    uint8_t status;
} sfc7120_tx_req_t;

typedef struct sfc7120_rx_req {
    void* __capability user_cap;
    void* __capability sealed_cap;

    uint8_t *raw_buffer;
    sfc7120_user_packet_descriptor_t descriptors[SFC7120_NUM_RX_DESC];
    size_t   descriptor_length;
    size_t   length_received;

    uint8_t  status;
    uint8_t  error;
} sfc7120_rx_req_t;

/* Per-buffer metadata, mirrors the e1000 pattern. */
typedef struct sfc7120_rx_buf_info {
    uint8_t   *buf;
    bus_addr_t paddr;
} sfc7120_rx_buf_info_t;

typedef struct sfc7120_tx_buf_info {
    uint8_t   *buf;
    bus_addr_t paddr;
} sfc7120_tx_buf_info_t;

/*
 * Per-instance software context.
 *
 * MANDATORY: dev MUST be field 0 and capio_sc MUST be field 1.
 * capio.c casts (void *)sc -> (capio_softc_header_t *) and indexes
 * directly. Anything before dev or between dev and capio_sc breaks every
 * cast and panics the kernel.
 */
typedef struct sfc7120_softc {
    device_t        dev;        /* MUST be first */
    capio_softc_t   capio_sc;   /* MUST be second */

    shared_mem_region_t smem[SFC7120_REGION_COUNT];

    /* PCI / BAR resources */
    struct resource    *mem_resource;       /* BAR0 — function MMIO window */
    int                 mem_res_id;
    bus_space_tag_t     mem_bus_tag;
    bus_space_handle_t  mem_bsh;

    uint8_t             mac_addr[6];

    /* Interrupt resources (MSI-X is the EF10 norm; legacy fallback omitted).
     * BAR4 backs the MSI-X table + PBA (16 KB on SFN7322F-R2). We allocate it
     * explicitly so the kernel can map the table, then ask for one MSI-X
     * vector at RID 1. */
    struct resource    *msix_bar_resource;
    int                 msix_bar_rid;
    int                 msix_nvec;
    struct resource    *irq_resource;
    int                 irq_res_id;
    void               *irq_handle;
    bool                intr_initialized;

    /* Control event queue (EVQ 0, interrupting) — kernel-owned. Receives
     * LINKCHANGE / MCDI async events; walked by the ISR. */
    void               *evq_ring;
    bus_addr_t          evq_ring_paddr;
    bus_dma_tag_t       evq_dtag;
    bus_dmamap_t        evq_dmamap;
    int                 evq_read_ptr;

    /* Data event queue (EVQ 1, non-interrupting) — userspace-polled.
     * RXQ 0 / TXQ 0 target this EVQ; the ring is exposed to userspace as
     * smem[SFC7120_EVQ_RING] and acked via the 0x2400 RPTR doorbell slice. */
    void               *data_evq_ring;
    bus_addr_t          data_evq_ring_paddr;
    bus_dma_tag_t       data_evq_dtag;
    bus_dmamap_t        data_evq_dmamap;

    /* TX resources */
    void               *tx_desc_ring;
    bus_addr_t          tx_desc_ring_paddr;
    bus_dma_tag_t       tx_desc_dtag;
    bus_dmamap_t        tx_desc_dmamap;
    sfc7120_tx_buf_info_t *tx_buf_infos;
    void               *tx_buffer;
    bus_addr_t          tx_buffer_paddr;
    bus_dma_tag_t       tx_buffer_dtag;
    bus_dmamap_t        tx_buffer_dmamap;
    bool                tx_buffer_mapped;
    int                 tx_head;
    int                 tx_tail;
    int                 tx_descriptors_free;

    /* RX resources */
    volatile bool       rx_received;
    volatile bool       rx_teardown;
    struct task         rx_task;
    struct taskqueue   *rx_taskqueue;
    struct mtx          rx_mtx;
    struct selinfo      selinfo;

    void               *rx_desc_ring;
    bus_addr_t          rx_desc_ring_paddr;
    bus_dma_tag_t       rx_desc_dtag;
    bus_dmamap_t        rx_desc_dmamap;

    sfc7120_rx_buf_info_t *rx_buf_infos;
    void               *rx_buffer;
    bus_addr_t          rx_buffer_paddr;
    bus_dma_tag_t       rx_buffer_dtag;
    bus_dmamap_t        rx_buffer_dmamap;
    bool                rx_buffer_mapped;
    int                 rx_head;

    /* Lifecycle */
    bool                device_attached;
    struct cdev        *cdev;
    struct mtx          sc_mtx;
    bool                dying;
    bool                mapped;

    /* MCDI: DMA-resident mailbox + bookkeeping for the MC firmware command
     * channel. Populated by sfc7120_mcdi_init() during hw_init. The mailbox
     * MUST be 256-byte aligned (EF10 doorbell recovery requirement, sfxge
     * bug24769) — sfc7120_mcdi.c enforces that via bus_dma_tag_create. */
    void               *mcdi_buf;
    bus_addr_t          mcdi_buf_paddr;
    bus_dma_tag_t       mcdi_dtag;
    bus_dmamap_t        mcdi_dmamap;
    struct mtx          mcdi_mtx;
    uint8_t             mcdi_seq;        /* 4-bit sequence, increments per request */
    bool                mcdi_new_epoch;  /* next request starts a new epoch */
    bool                mcdi_initialized;

    //finn: this is for failure preventions later
    uint32_t mcdi_prev_reboot_status;

    /* DRV_ATTACH / ALLOC_VIS results, populated during hw_init. */
    uint32_t            mcdi_func_flags;  /* MC_CMD_DRV_ATTACH_EXT_OUT_FUNC_FLAGS */
    uint32_t            vi_base;          /* base absolute VI for this function */
    uint32_t            vi_count;         /* VIs allocated to this function */

    /* GET_CAPABILITIES FLAGS1 cache. Populated by sfc7120_mcdi_dump_func_info.
     * Used downstream to gate optional MCDI flags (e.g. the PERMIT_SET_MAC_*
     * bit in VADAPTOR_ALLOC) that older firmware variants reject with EINVAL
     * when the cap isn't advertised. Mirrors sfxge's enc_* capability cache.*/
    uint32_t            mcdi_cap_flags1;
    bool                mcdi_caps_valid;

    /* GET_VERSION result (firmware build identifier). */
    uint32_t            fw_version[2];    /* hi/lo pair */
    bool                drv_attached;
    bool                vis_allocated;
    bool                evq_initialized;

    /* vadaptor, tx/rx init flags */
    bool vadaptor_allocated;
    bool rxq_initialized;
    bool txq_initialized;

    /* PHY supported-capabilities mask harvested from MC_CMD_GET_PHY_CFG.
     * Used as the SET_LINK advertisement mask. The MC firmware doesn't
     * report the *_FEC_REQUESTED bits — we OR them in manually post-read
     * to mirror sfxge ef10_nic.c:1869-1877. */
    uint32_t            phy_supported_cap_mask;
    uint32_t            phy_adv_cap_mask;
    uint32_t            phy_media_type;

    /* Link state populated by MC_CMD_GET_LINK. Updated by the
     * post-SET_LINK poll in hw_init today, and (eventually) by the EVQ
     * event handler when interrupts are wired. */
    bool                link_up;
    bool                full_duplex;
    uint32_t            link_speed_mbps;
    uint32_t            link_fcntl;

    bool                mac_configured;
    bool                link_configured;

    /* MCDI FILTER_OP handle for the unicast DST_MAC filter installed after
     * queue init. Zeroed until sfc7120_mcdi_install_mac_filter succeeds;
     * teardown removes it via MC_CMD_FILTER_OP OP=REMOVE. */
    uint64_t            mac_filter_handle;
    bool                mac_filter_installed;

    /* Direct-path (userspace) ring head pointers. Seeded to 0 at attach
     * (fresh queues after INIT_*) and updated by SFC7120_SET_EVQ_RPTR at
     * userspace teardown so the next open sees the current NIC state.
     * Also carry the "kernel pre-posted this many RX descriptors" hint so
     * userspace's rx_head starts past the pre-post window. */
    uint32_t            direct_evq_rptr;
    uint32_t            direct_tx_head;
    uint32_t            direct_rx_head;

    bool                debug_reg_ops;
} sfc7120_softc_t;

/*
 * sfc7120_vi_info_req_t — VI geometry handed to userspace for the direct
 * data path. Layout MUST match sfc7120_uapi.h in the userspace tree.
 */
typedef struct sfc7120_vi_info_req {
    void * __capability user_cap;
    void * __capability sealed_cap;

    uint64_t tx_buffer_paddr;
    uint64_t rx_buffer_paddr;

    uint32_t vi_base;
    uint32_t evq_instance;
    uint32_t rxq_instance;
    uint32_t txq_instance;

    uint32_t num_tx_desc;
    uint32_t num_rx_desc;
    uint32_t num_evq_entry;

    uint32_t tx_head;
    uint32_t rx_head;
    uint32_t evq_read_ptr;
} sfc7120_vi_info_req_t;

/*
 * sfc7120_evq_sync_req_t — userspace hands its final data-path pointers
 * back to the kernel at teardown so the next open sees fresh state.
 */
typedef struct sfc7120_evq_sync_req {
    void * __capability user_cap;
    void * __capability sealed_cap;

    uint32_t evq_read_ptr;
    uint32_t tx_head;
    uint32_t rx_head;
} sfc7120_evq_sync_req_t;

/* Driver-specific IOCTLs.
 *   'S' picks a non-conflicting group letter (E1000 uses 'E', CAPIO 'C'). */
#define SFC7120_RX          _IOWR('S', 1, sfc7120_rx_req_t)
#define SFC7120_TX          _IOWR('S', 2, sfc7120_tx_req_t)
#define SFC7120_GET_MAC     _IOWR('S', 3, sfc7120_mac_req_t)
#define SFC7120_GET_VI_INFO _IOWR('S', 4, sfc7120_vi_info_req_t)
#define SFC7120_SET_EVQ_RPTR _IOWR('S', 5, sfc7120_evq_sync_req_t)

#endif /* SFC7120_POL_H */
