/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Command DB -> ARC logical level planning. CPU RAM ONLY, no power authority.
 * Caller supplies a private, stable, normal-RAM snapshot for the entire call.
 * Never pass live shared memory/MMIO, unowned firmware memory or user pointers.
 * Output must not alias input/name/policy. Tables/plans are DATA, not grants,
 * voltages in uV, actual votes, completion evidence or a native provider.
 */
#ifndef GIULIA_GPU_ARC_H
#define GIULIA_GPU_ARC_H
#include <stdint.h>
#include <stddef.h>
#define GGARC_ABI 1u
#define GGARC_HEADER_BYTES 144u
#define GGARC_MAX_BYTES (256u*1024u)
#define GGARC_MAX_ENTRIES 1024u
#define GGARC_LEVELS 16u
#define GGARC_HW_ARC 3u
#define GGARC_IDENTITY "GIULIA GPU ARC: snapshot-only no-hardware-authority"
enum { GGARC_OK=0,GGARC_ARGUMENT,GGARC_BOUNDS,GGARC_FORMAT,GGARC_VERSION,
       GGARC_STANDALONE,GGARC_NOT_FOUND,GGARC_AMBIGUOUS,GGARC_TYPE,
       GGARC_LEVEL_DATA,GGARC_RANGE };
/* Versions are explicit caller policy, not guessed from this board/source.
 * Matching versions/magic/checksum fields DO NOT authenticate a snapshot. */
struct ggarc_policy { uint32_t db_version; uint16_t arc_version; };
struct ggarc_table {
    uint32_t abi,db_version,address;
    uint16_t arc_version,raw_count,count,reserved;
    uint16_t levels[GGARC_LEVELS];
};
struct ggarc_plan {
    uint32_t address,selector,logical_level;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggarc_identity(void);
/* Exact 1..8-byte ASCII key, no truncation; no required trailing NUL.
 * Outputs zeroed on failure; all offsets are from the 144-byte header end.
 * Strict supported subset: <=1024 entries, contiguous unique slave headers,
 * disjoint entry tables, valid aux bounds, selected 20-bit aligned ARC address,
 * monotonic logical levels with trailing-only zero padding. Aux may alias aux.
 * Checksum is opaque upstream; it is not verified or treated as authenticity. */
int ggarc_resolve(const void *,size_t,const struct ggarc_policy *,
    const char *,size_t,struct ggarc_table *);
/* Lowest selector in logical interval [min,max], both <=65535 (NOT uV).
 * Index 0 may be NONZERO: never assume it is an OFF command. Caller must
 * independently validate provenance, epochs, owned RSC/TCS and aggregation
 * before any eventual transaction. No request submission is exposed here. */
int ggarc_select(const struct ggarc_table *,uint32_t,uint32_t,struct ggarc_plan *);
#ifdef __cplusplus
}
#endif
#endif
