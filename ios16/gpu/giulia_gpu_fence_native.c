/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if !defined(GG_FENCE_NATIVE_STANDALONE)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_FENCE_NATIVE_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_fence_native.h"

static void
ggfn_zero(void *p, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++) {
		((unsigned char *)p)[i] = 0;
	}
}

/* The provider is re-asked for its full proof on every transition; the share
 * transcript must describe the same epoch, dynamic participants and parcel. */
static int
ggfn_observe(void *cookie, const struct ggfc_envelope *envelope,
    struct ggfc_source *out)
{
	struct ggfn_state *s = (struct ggfn_state *)cookie;
	struct ggfc_source source;
	struct ggfn_share share;

	if (!s || !envelope || !out || s->invalidated || !s->share) {
		return 0;
	}
	if (!ggfp_live(&s->provider)) {
		return 0;
	}
	ggfn_zero(&share, sizeof(share));
	if (s->share(s->cookie, &share) != 1) {
		return 0;
	}
	if (!share.complete || share.invalid || !share.parcel || !share.epoch ||
	    share.epoch != s->provider.epoch || !share.self_vmid ||
	    share.self_vmid == UINT16_MAX || !share.peer_vmid ||
	    share.peer_vmid == UINT16_MAX || share.self_vmid == share.peer_vmid) {
		return 0;
	}
	if (envelope->layout.physical != s->provider.layout.physical ||
	    envelope->layout.bytes != s->provider.layout.bytes ||
	    envelope->layout.page_bytes != s->provider.layout.page_bytes) {
		return 0;
	}
	ggfn_zero(&source, sizeof(source));
	source.identity = s;
	source.epoch = s->provider.epoch;
	source.carveout_physical = GGF_CARVEOUT_PHYSICAL;
	source.carveout_bytes = GGF_CARVEOUT_BYTES;
	source.envelope_physical = envelope->physical;
	source.envelope_bytes = envelope->bytes;
	source.parcel_handle = share.parcel;
	source.self_vmid = (uint16_t)share.self_vmid;
	source.peer_vmid = (uint16_t)share.peer_vmid;
	source.layout = envelope->layout;
	source.proofs = GGFC_SOURCE_REQUIRED;
	*out = source;
	return 1;
}

static int
ggfn_map(void *cookie, const struct ggfc_envelope *envelope,
    struct ggfc_mapping *out)
{
	struct ggfn_state *s = (struct ggfn_state *)cookie;

	if (!s || !envelope || !out || s->invalidated || !s->map) {
		return 0;
	}
	return s->map(s->cookie, envelope, out) == 1;
}

static int
ggfn_audit(void *cookie, const struct ggfc_envelope *envelope,
    const struct ggfc_mapping *mapping, struct ggfc_cpu_audit *out)
{
	struct ggfn_state *s = (struct ggfn_state *)cookie;

	if (!s || !envelope || !mapping || !out || s->invalidated || !s->audit) {
		return 0;
	}
	if (s->audit(s->cookie, envelope, mapping, out) != 1) {
		return 0;
	}
	return out->proofs == GGFC_AUDIT_REQUIRED &&
	       ggfc_same_mapping(&out->mapping, mapping);
}

int
ggfn_import(struct ggfn_state *s, const struct ggfn_inputs *in, uint32_t local_page)
{
	struct ggfc_owner owner;

	if (!s || (uintptr_t)s % __alignof__(*s) || !in || !in->provider ||
	    !in->share || !in->map || !in->audit) {
		return GGFC_ARGUMENT;
	}
	if (s->invalidated) {
		return GGFC_QUARANTINED;
	}
	{
		unsigned state = __atomic_load_n(&s->cpu.state, __ATOMIC_ACQUIRE);
		if (state != GGFC_EMPTY) {
			return state == GGFC_BROKEN ? GGFC_QUARANTINED :
			       state == GGFC_MAPPED ? GGFC_ALREADY : GGFC_BUSY;
		}
	}
	if (!ggfp_valid(in->provider)) {
		return GGFC_ARGUMENT;
	}
	/* Freeze the provider so a later caller mutation cannot change the
	 * proofs, then hand the actual callbacks to the CPU state machine. */
	s->provider = *in->provider;
	s->share = in->share;
	s->map = in->map;
	s->audit = in->audit;
	s->cookie = in->cookie;
	ggfn_zero(&owner, sizeof(owner));
	owner.identity = s;
	owner.epoch = s->provider.epoch;
	owner.cookie = s;
	owner.observe = ggfn_observe;
	owner.map = ggfn_map;
	owner.audit = ggfn_audit;
	return ggfc_import(&s->cpu, &owner, &s->provider.layout, local_page);
}

int
ggfn_get(struct ggfn_state *s, struct ggfc_mapping *out)
{
	if (!s || !out || s->invalidated) {
		return GGFC_QUARANTINED;
	}
	return ggfc_get(&s->cpu, out);
}

int
ggfn_invalidate(struct ggfn_state *s)
{
	int result;

	if (!s) {
		return GGFC_ARGUMENT;
	}
	__atomic_store_n(&s->invalidated, 1u, __ATOMIC_RELEASE);
	result = ggfc_invalidate(&s->cpu);
	return result == GGFC_ARGUMENT ? GGFC_LOST : result;
}

int
ggfn_status(const struct ggfn_state *s, struct ggfn_status *out)
{
	struct ggfn_status status;

	if (!s || !out) {
		return 0;
	}
	ggfn_zero(&status, sizeof(status));
	status.cpu_state = s->cpu.state;
	status.state = s->cpu.state == GGFC_MAPPED ? GGFN_IMPORTED :
	    s->cpu.state == GGFC_BROKEN ? GGFN_BROKEN : GGFN_EMPTY;
	status.result = s->cpu.last_result;
	status.page_bytes = s->cpu.envelope.local_page_bytes;
	status.envelope_physical = s->cpu.envelope.physical;
	status.envelope_bytes = s->cpu.envelope.bytes;
	status.offset = s->cpu.envelope.offset;
	status.epoch = s->provider.epoch;
	status.invalidated = s->invalidated ? 1u : 0u;
	if (s->cpu.state != GGFC_EMPTY) {
		status.parcel = s->cpu.source.parcel_handle;
		status.self_vmid = s->cpu.source.self_vmid;
		status.peer_vmid = s->cpu.source.peer_vmid;
	}
	if (s->cpu.state == GGFC_MAPPED) {
		status.proofs = GGFC_AUDIT_REQUIRED;
	}
	*out = status;
	return 1;
}
#endif
