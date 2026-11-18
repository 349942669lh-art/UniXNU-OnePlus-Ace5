/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Gen7 GMU register preparation from the pinned OnePlus adreno_gen7_gmu.c:
 * gen7_gmu_register_config() plus the HFI queue-table handoff. Register numbers
 * are word offsets in the vendor GPU register namespace (giulia_gpu_gmu.h), all
 * inside the GMU window. Every write is read back; no reset release, TCM write,
 * HFI submission or DMA is performed here.
 */
#ifndef GIULIA_GPU_GMU_BOOT_H
#define GIULIA_GPU_GMU_BOOT_H
#include "giulia_gpu_gmu.h"

enum {
    GGB_BOOT_CONFIG = 0x1f801, GGB_FW_INIT_RESULT = 0x1f81c,
    GGB_CM3_CFG = 0x1f82d, GGB_FAL_INTF = 0x1f8f0, GGB_FALNEXT_INTF = 0x1f8f1,
    GGB_HFI_STATUS = 0x1f980, GGB_QTBL_INFO = 0x1f984,
    GGB_QTBL_ADDR = 0x1f985, GGB_HFI_INIT = 0x1f986,
    GGB_GENERAL_8 = 0x1f9cd, GGB_GENERAL_9 = 0x1f9ce,
    GGB_GENERAL_10 = 0x1f9cf, GGB_GENERAL_11 = 0x1f9d0,
    GGB_AHB_FENCE_RANGE = 0x23b11,
    GGB_CM3_CFG_RESET = 0x4052,        /* vendor reset value without NMI */
    GGB_BOOT_CONFIG_VALUE = 0x2,
    GGB_AHB_FENCE_VALUE = (1u<<31) | (0x32u<<18) | 0x8a0u
};
struct ggb_config {
    uint32_t hfi_iova, vrb_iova, log_iova, log_bytes, chip_rev;
};

/* A configuration write is only accepted after an independent readback of the
 * same register. FW_INIT_RESULT is deliberately not verified: it is written to
 * zero and later updated by the GMU firmware itself. */
static inline int ggb_verify_write(const struct gg_gmu_ops *o,
    struct gg_gmu_state *s, uint32_t reg, uint32_t value, uint32_t mask)
{
    uint32_t readback = 0;
    if (gg_gmu_write(o, s, reg, value, 0) || gg_gmu_barrier(o, s, 0)) return 0;
    if (gg_gmu_read(o, s, reg, &readback, 0)) return 0;
    if ((readback & mask) != (value & mask)) {
        gg_record_masked(&s->diag, o->trace, o->cookie, GGD_GMU, s->phase,
            GGD_VALIDATE, 0x03d00000ull + 4ull * reg, value, readback, 7, mask,
            GGM_READBACK, 0);
        return 0;
    }
    return 1;
}
static inline int ggb_config_valid(const struct ggb_config *c)
{
    return c && !(c->hfi_iova & 4095u) && !(c->vrb_iova & 4095u) &&
        !(c->log_iova & 4095u) && c->log_bytes && !(c->log_bytes & 4095u) &&
        c->log_bytes <= 4096u * 256u && c->chip_rev;
}
/* gen7_gmu_register_config(): FAL veto, boot config, HFI queue table, AHB fence
 * range, CM3 reset value and the general-purpose registers the firmware reads
 * before it sets FW_INIT_RESULT. */
static inline int ggb_configure(const struct gg_gmu_ops *o,
    struct gg_gmu_state *s, const struct ggb_config *c)
{
    uint32_t log_reg;
    if (!gg_gmu_ops_valid(o) || !s || !ggb_config_valid(c)) return GGM_ARGUMENT;
    if (s->quarantined || !s->attempted) return GGM_BUSY;
    s->phase = 6;
    log_reg = (c->log_iova & ~4095u) | ((c->log_bytes / 4096u - 1u) & 0xffu);
    if (!ggb_verify_write(o, s, GGB_FAL_INTF, 1, ~0u) ||
        !ggb_verify_write(o, s, GGB_FALNEXT_INTF, 1, ~0u) ||
        gg_gmu_write(o, s, GGB_FW_INIT_RESULT, 0, 0) ||
        !ggb_verify_write(o, s, GGB_BOOT_CONFIG, GGB_BOOT_CONFIG_VALUE, ~0u) ||
        !ggb_verify_write(o, s, GGB_QTBL_ADDR, c->hfi_iova, ~0u) ||
        !ggb_verify_write(o, s, GGB_QTBL_INFO, 1, ~0u) ||
        !ggb_verify_write(o, s, GGB_AHB_FENCE_RANGE, GGB_AHB_FENCE_VALUE, ~0u) ||
        !ggb_verify_write(o, s, GGB_CM3_CFG, GGB_CM3_CFG_RESET, ~0u) ||
        !ggb_verify_write(o, s, GGB_GENERAL_11, c->vrb_iova, ~0u) ||
        !ggb_verify_write(o, s, GGB_GENERAL_10, c->chip_rev, ~0u) ||
        !ggb_verify_write(o, s, GGB_GENERAL_8, log_reg, ~0u) ||
        !ggb_verify_write(o, s, GGB_GENERAL_9, 0, ~0u)) {
        return GGM_READBACK;
    }
    return GGM_OK;
}
/* The HFI table must be exactly the owned queue table and the firmware must not
 * have started HFI yet. The state machine performs the HFI_INIT write and the
 * bounded CTRL_STATUS poll. */
static inline int ggb_prepare_hfi(const struct gg_gmu_ops *o,
    struct gg_gmu_state *s, const struct ggb_config *c)
{
    uint32_t value = 0;
    if (!gg_gmu_ops_valid(o) || !s || !ggb_config_valid(c)) return GGM_ARGUMENT;
    if (s->quarantined || !s->fw_ready) return GGM_BUSY;
    s->phase = 5;
    if (gg_gmu_read(o, s, GGB_QTBL_ADDR, &value, 0) || value != c->hfi_iova)
        return GGM_READBACK;
    if (gg_gmu_read(o, s, GGB_QTBL_INFO, &value, 0) || (value & 0xffu) != 1u)
        return GGM_READBACK;
    if (gg_gmu_read(o, s, GGB_HFI_STATUS, &value, 0)) return GGM_IO;
    if (value & 1u) {
        gg_record_masked(&s->diag, o->trace, o->cookie, GGD_GMU, s->phase,
            GGD_VALIDATE, 0x03d00000ull + 4ull * GGB_HFI_STATUS, 0, value, 7, 1u,
            GGM_READBACK, 0);
        return GGM_READBACK;
    }
    return GGM_OK;
}
#endif
