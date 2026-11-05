/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_DIAG_H
#define GIULIA_GPU_DIAG_H
#include <stdint.h>
enum { GGD_POWER=1, GGD_SMMU, GGD_GMU };
enum { GGD_READ=1, GGD_WRITE, GGD_BARRIER, GGD_ACQUIRE, GGD_RELEASE,
       GGD_VALIDATE, GGD_POLL, GGD_CONFIGURE, GGD_RAM, GGD_WAIT };
enum { GGD_ADDRESS=1, GGD_EXPECTED=2, GGD_OBSERVED=4, GGD_NATIVE=8 };
struct gg_event {
    uint64_t sequence, address, expected, observed, mask;
    unsigned subsystem, phase, operation, valid, cleanup;
    int error, native_error;
};
typedef void (*gg_trace_fn)(void *,const struct gg_event *);
struct gg_diagnostics {
    uint64_t sequence;
    struct gg_event last, failure, cleanup_failure;
};
/* Presence flags distinguish an unavailable value from an observed zero.
 * Preserve the first primary failure while cleanup continues to emit events.
 * This is an observation journal, not a root-cause classifier.
 */
static inline void gg_record_full(struct gg_diagnostics *d,gg_trace_fn sink,void *cookie,
    unsigned subsystem,unsigned phase,unsigned op,uint64_t address,
    uint64_t expected,uint64_t observed,unsigned valid,uint64_t mask,int error,int native_error,unsigned cleanup)
{
    struct gg_event e={++d->sequence,address,expected,observed,mask,subsystem,phase,op,valid,cleanup,error,native_error};
    d->last=e;
    if (error && cleanup && !d->cleanup_failure.sequence) d->cleanup_failure=e;
    if (error && !cleanup && !d->failure.sequence) d->failure=e;
    if (sink) sink(cookie,&e);
}
static inline void gg_record_masked(struct gg_diagnostics *d,gg_trace_fn sink,void *cookie,
    unsigned subsystem,unsigned phase,unsigned op,uint64_t address,
    uint64_t expected,uint64_t observed,unsigned valid,uint64_t mask,int error,unsigned cleanup)
{
    gg_record_full(d,sink,cookie,subsystem,phase,op,address,expected,observed,valid,mask,error,0,cleanup);
}
static inline void gg_record(struct gg_diagnostics *d,gg_trace_fn sink,void *cookie,
    unsigned subsystem,unsigned phase,unsigned op,uint64_t address,
    uint64_t expected,uint64_t observed,unsigned valid,int error,unsigned cleanup)
{
    gg_record_masked(d,sink,cookie,subsystem,phase,op,address,expected,observed,valid,
        ~(uint64_t)0,error,cleanup);
}
static inline const char *gg_phase_name(unsigned subsystem,unsigned phase)
{
    static const char *const power[]={"cx-domain","gmu-rate","hub-rate","gmu-clock","cxo-clock",
        "axi-clock","memnoc-clock","ahb-clock","hub-clock","smmu-clock","dvm-clock","ready-poll","resolve","quiescence"};
    static const char *const smmu[]={"none","lease-preflight","program-context","attach-stream","active","detach"};
    static const char *const gmu[]={"none","load-tcm","configure-boot","release-reset","firmware-ready","prepare-hfi","hfi-ready","abort-reset"};
    if(subsystem==GGD_POWER && phase<sizeof(power)/sizeof(power[0])) return power[phase];
    if(subsystem==GGD_SMMU && phase<sizeof(smmu)/sizeof(smmu[0])) return smmu[phase];
    if(subsystem==GGD_GMU && phase<sizeof(gmu)/sizeof(gmu[0])) return gmu[phase];
    return "unknown";
}
static inline const char *gg_subsystem_name(unsigned n)
{
    switch(n) {case GGD_POWER:return "power";case GGD_SMMU:return "smmu";case GGD_GMU:return "gmu";default:return "unknown";}
}
static inline const char *gg_operation_name(unsigned n)
{
    switch(n) {case GGD_READ:return "read";case GGD_WRITE:return "write";case GGD_BARRIER:return "barrier";
    case GGD_ACQUIRE:return "acquire";case GGD_RELEASE:return "release";case GGD_VALIDATE:return "validate";
    case GGD_POLL:return "poll";case GGD_CONFIGURE:return "configure";case GGD_RAM:return "ram-verify";case GGD_WAIT:return "wait-us";default:return "unknown";}
}
static inline const char *gg_error_name(unsigned subsystem,int code)
{
    static const char *const power[]={"ok","argument","busy","provider","timeout","release-failed"};
    static const char *const smmu[]={"ok","argument","bad-range","overlap","capacity","budget","unsupported","readback-mismatch","io","timeout","busy","fault"};
    static const char *const gmu[]={"ok","argument","busy","firmware","prerequisite","io","timeout","readback-mismatch"};
    if(code<0)return "unknown";
    if(subsystem==GGD_POWER && (unsigned)code<sizeof(power)/sizeof(power[0]))return power[code];
    if(subsystem==GGD_SMMU && (unsigned)code<sizeof(smmu)/sizeof(smmu[0]))return smmu[code];
    if(subsystem==GGD_GMU && (unsigned)code<sizeof(gmu)/sizeof(gmu[0]))return gmu[code];
    return "unknown";
}
#endif
