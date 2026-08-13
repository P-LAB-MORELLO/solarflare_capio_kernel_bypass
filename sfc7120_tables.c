#include "sfc7120_mmio.h"

/*
 * CAPIO MMIO slice manifest for the Solarflare 7120 (Huntington / EF10).
 *
 * Each entry defines a sub-capability that userspace can derive into the
 * BAR0 capability. modmap enforces these bounds in hardware: registers
 * not listed cannot be accessed by userspace, even if the offsets are
 * known. Read-only registers get CHERI_PERM_LOAD only.
 *
 * This is the SHORT initial set covering the registers a CAPIO
 * userspace driver minimally needs:
 *
 *   - MC doorbell (RW)               — push MCDI requests (low word only)
 *   - EVQ read-pointer doorbell (RW) — ack events
 *   - RX/TX descriptor doorbells (RW) — push producer pointer
 *   - HW_REV_ID (RO)                 — sanity check at attach
 *
 * MC_EVENT and MC_STATUS are omitted: neither is a real BAR register on EF10.
 * MC events arrive via the EVQ DMA ring; MC status is a magic value in the
 * MCDI response buffer.
 *
 * Extend this table when adding multi-queue support. Mirror the per-queue
 * pattern from mlx5pol: one slice per channel, named with the channel
 * index suffix.
 */
slice_def_t sfc7120_reg_slices[] = {
    /* Order MUST match sfc7120_mmio_slice_idx_t in the userspace uapi:
     * MC_DOORBELL(0), DATA_EVQ_RPTR_DBL(1), RX_DESC_DBL(2),
     * TX_DESC_DBL(3), HW_REV_ID(4). */
    { SFC7120_REG_MCDB,          "MC_DOORBELL",       false, 4 },
    // { SFC7120_REG_MC_EVENT, "MC_EVENT", true, 4 },
    // Not a BAR register — MC events come via the EVQ DMA ring, not MMIO.
    /* Data EVQ (instance 1) ack — bug35388 indirect register for VI 1.
     * Userspace acks its polled EVQ with the two-write RPTR-high/low
     * sequence here. The control EVQ 0's registers are NOT exposed. */
    { SFC7120_REG_DATA_EVQ_IND,  "DATA_EVQ_IND",      false, 4 },
    { SFC7120_REG_RX_DESC_DBL,   "RX_DESC_DBL",       false, 4 },
    /* 12 B: dword [0] is the qword-push low half, dword [2] (+8) is the
     * WPTR-only DWORD push the userlib uses (tx_post writes [2]). */
    { SFC7120_REG_TX_DESC_DBL,   "TX_DESC_DBL",       false, 12 },
    { SFC7120_REG_BIU_HW_REV_ID, "HW_REV_ID",         true,  4 },
    // { SFC7120_REG_MC_STATUS, "MC_STATUS", true, 4 },
    // Not a BAR register — MC_STATUS is a magic value in the MCDI DMA response buffer.
};

const size_t SFC7120_MMIO_SLICE_COUNT =
    sizeof(sfc7120_reg_slices) / sizeof(sfc7120_reg_slices[0]);
