/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Kernel-only CPU import of the registered hardware-fence provider.
 *
 * This unit is the only bridge between a live provider/ownership transcript and
 * the ggfc CPU state machine: provider proofs are re-asked on every call, the
 * share identity must match the provider epoch, and the real mapper/auditor are
 * supplied by the platform (in the kernel: IOMemoryDescriptor Normal NC plus the
 * native pmap audit). It never registers a provider, reads queue contents,
 * initializes the carveout or manufactures a proof bit.
 */
#ifndef GIULIA_GPU_FENCE_NATIVE_H
#define GIULIA_GPU_FENCE_NATIVE_H
#include "giulia_gpu_fence_cpu.h"
#include "giulia_gpu_fence_provider.h"
#include "giulia_gpu_pmap.h"

/* Authoritative identity of the current primary share transaction. The owner
 * fills this from the real RM/SCM transcript; constants are not accepted. */
struct ggfn_share {
    uint64_t epoch;
    uint32_t parcel, self_vmid, peer_vmid, complete, invalid;
};

struct ggfn_inputs {
    const struct ggf_provider *provider;
    int (*share)(void *, struct ggfn_share *);
    /* Real mapping backend: installs one Normal NC CPU mapping and returns the
     * frozen descriptor. In the kernel this is giulia_gpu_fence_native_map(). */
    int (*map)(void *, const struct ggfc_envelope *, struct ggfc_mapping *);
    /* Real audit backend: actual descriptor/PA/attribute inspection. */
    int (*audit)(void *, const struct ggfc_envelope *, const struct ggfc_mapping *,
        struct ggfc_cpu_audit *);
    void *cookie;
};

enum { GGFN_EMPTY = 0, GGFN_IMPORTED = 1, GGFN_BROKEN = 2 };

struct ggfn_status {
    uint32_t state, cpu_state, result, page_bytes, proofs;
    uint64_t epoch, envelope_physical;
    uint32_t envelope_bytes, offset, parcel, self_vmid, peer_vmid, invalidated;
};

struct ggfn_state {
    struct ggfc_state cpu;
    struct ggf_provider provider;   /* frozen copy used for every proof */
    int (*share)(void *, struct ggfn_share *);
    int (*map)(void *, const struct ggfc_envelope *, struct ggfc_mapping *);
    int (*audit)(void *, const struct ggfc_envelope *, const struct ggfc_mapping *,
        struct ggfc_cpu_audit *);
    void *cookie;
    unsigned invalidated;
};

/* One import attempt per state. The provider must be the registered, frozen
 * descriptor: ggfn freezes a private copy so later caller mutation cannot change
 * the proofs. Every failure is retained and quarantined. */
#ifdef __cplusplus
extern "C" {
#endif
int ggfn_import(struct ggfn_state *, const struct ggfn_inputs *, uint32_t local_page);
int ggfn_get(struct ggfn_state *, struct ggfc_mapping *);
int ggfn_invalidate(struct ggfn_state *);
int ggfn_status(const struct ggfn_state *, struct ggfn_status *);

/* Platform backend, implemented in giulia_gpu_fence_iokit.cpp for the target kernel. The
 * host build never defines it; tests inject their own map/audit callbacks. */
int giulia_gpu_fence_native_map(void *, const struct ggfc_envelope *, struct ggfc_mapping *);
int giulia_gpu_fence_native_audit(void *, const struct ggfc_envelope *,
    const struct ggfc_mapping *, struct ggfc_cpu_audit *);
#ifdef __cplusplus
}
#endif
#endif
