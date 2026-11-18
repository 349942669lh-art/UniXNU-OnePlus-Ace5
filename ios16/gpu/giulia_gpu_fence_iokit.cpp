/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include "giulia_gpu_fence_native.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

/* The carveout is device memory below the physical aperture, so
 * kIOMapInhibitCache selects the arm pmap's attr index 1 (Normal NC) instead of
 * a write-back or write-combining leaf. Both the descriptor and the map object
 * stay retained until reboot: there is deliberately no release path. */
#ifdef __cplusplus
extern "C" {
#endif
int
giulia_gpu_fence_native_map(void *cookie, const struct ggfc_envelope *envelope,
    struct ggfc_mapping *out)
{
	IOMemoryDescriptor *descriptor;
	IOMemoryMap *map;
	void *base;

	(void)cookie;
	if (!envelope || !out) {
		return 0;
	}
	descriptor = IOMemoryDescriptor::withPhysicalAddress(
	    (addr64_t)envelope->physical, (IOByteCount)envelope->bytes,
	    kIODirectionInOut);
	if (!descriptor) {
		return 0;
	}
	map = descriptor->createMappingInTask(kernel_task, 0,
	    kIOMapAnywhere | kIOMapInhibitCache, 0, envelope->bytes);
	descriptor->release();
	if (!map) {
		return 0;
	}
	map->retain();
	base = (void *)map->getVirtualAddress();
	if (!base || ((uintptr_t)base & (envelope->local_page_bytes - 1u))) {
		return 0;
	}
	out->handle = map;
	out->base = base;
	out->queue = (volatile void *)((uintptr_t)base + envelope->offset);
	out->physical = envelope->physical;
	out->bytes = envelope->bytes;
	out->local_page_bytes = envelope->local_page_bytes;
	return 1;
}

int
giulia_gpu_fence_native_audit(void *cookie, const struct ggfc_envelope *envelope,
    const struct ggfc_mapping *mapping, struct ggfc_cpu_audit *out)
{
	struct ggfpm_audit audit;

	(void)cookie;
	if (!envelope || !mapping || !out) {
		return 0;
	}
	if (!giulia_gpu_fence_cpu_audit((uintptr_t)mapping->base, envelope->physical,
	    envelope->bytes, 1, &audit)) {
		return 0;
	}
	if (audit.abi != GGFPM_ABI || audit.bytes != envelope->bytes ||
	    audit.cpu_page_bytes != envelope->local_page_bytes ||
	    audit.writable != 1 || audit.virtual_address != (uintptr_t)mapping->base ||
	    audit.physical != envelope->physical || audit.span != envelope->bytes) {
		return 0;
	}
	out->mapping = *mapping;
	out->proofs = GGFC_AUDIT_REQUIRED;
	return 1;
}
#ifdef __cplusplus
}
#endif
#endif
