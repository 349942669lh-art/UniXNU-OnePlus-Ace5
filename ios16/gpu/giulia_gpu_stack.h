/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ordered bring-up framework for the Adreno 750 on giulia (SM8650).
 *
 * The stack sequences, gates, records evidence and unwinds. It performs no
 * hardware access itself: every stage is supplied by a real provider and may
 * only run when its declared prerequisites are already entered.
 *
 * Invariants:
 *   1. A stage is entered only when every prerequisite bit is entered. A
 *      missing or failing provider blocks its dependants; it is never skipped
 *      and never satisfied by a constant.
 *   2. probe() is read-only and runs immediately before enter(); there is no
 *      other path into enter(). Evidence from an earlier boot never authorises
 *      entry.
 *   3. Host mutation and device writes are authorised separately. Both
 *      default to refused, which leaves the stack in prepare/observe mode.
 *   4. A failed device entry latches quarantine for the whole stack and stops
 *      the forward walk. An unconfirmed leave does the same and stops the
 *      reverse walk, because resources may still own DMA.
 *   5. Topology freezes at the first advance(); one owner per stage.
 *   6. Counters are cumulative. A stage that wrote to hardware stays counted
 *      after retreat, and retreat never implies the hardware is idle.
 *
 * The dependency graph is a DAG, not a prefix: an independent branch such as
 * the Gunyah channel may be entered while the power lease is still blocked.
 * Reviewers see that split directly in the per-stage log.
 */
#ifndef GIULIA_GPU_STACK_H
#define GIULIA_GPU_STACK_H
#include <stdint.h>

#define GGSK_BIT(s) (1u<<(unsigned)(s))

enum ggsk_stage {
    GGSK_BOOT=0,        /* ADT handoff reports: UEFI power + Gunyah RM resources */
    GGSK_FIRMWARE,      /* pinned GMU/SQE/AQE/ZAP pack verified                 */
    GGSK_HOST_MEMORY,   /* GMU RAM, 4 KiB tables, cache publication           */
    GGSK_POWER,         /* runtime CX/GMU/hub/SMMU leases                      */
    GGSK_GUNYAH,        /* HVC/MSGQ/RM session, IRQ lease, owned epoch         */
    GGSK_SHARE,         /* carveout MEM_SHARE + peer acceptance               */
    GGSK_SMMU,          /* SID 5 + GPU context banks translating              */
    GGSK_GMU_LOAD,      /* firmware into TCM/RAM, FW_READY                    */
    GGSK_GMU_START,     /* reset release, HFI transport ready                 */
    GGSK_HFI,           /* feature/performance/bandwidth/hwsched start        */
    GGSK_CONTROLLER,    /* hardware fence table, CTX0, adjacent page grant    */
    GGSK_FENCE,         /* provider registered, completion observable         */
    GGSK_SUBMIT,        /* first owned IB reaching a real fence               */
    GGSK_SCANOUT,       /* GPU frame offered to the display swap              */
    GGSK_METAL,         /* accelerator published to the render server         */
    GGSK_STAGES
};

enum ggsk_class {
    GGSK_CLASS_RO=0,    /* read-only observation, never mutates anything      */
    GGSK_CLASS_HOST=1,  /* CPU/page-table mutation only, no device writes     */
    GGSK_CLASS_DEVICE=2 /* device MMIO, HVC, SMC or firmware-visible memory   */
};

struct ggsk_desc {
    const char *name;
    uint32_t prereq;
    unsigned klass;
};

static inline const struct ggsk_desc *ggsk_desc_of(unsigned stage)
{
    static const struct ggsk_desc table[GGSK_STAGES]={
        {"boot-handoff",0,GGSK_CLASS_RO},
        {"firmware",GGSK_BIT(GGSK_BOOT),GGSK_CLASS_RO},
        {"host-memory",GGSK_BIT(GGSK_FIRMWARE),GGSK_CLASS_HOST},
        {"power-lease",GGSK_BIT(GGSK_BOOT)|GGSK_BIT(GGSK_HOST_MEMORY),GGSK_CLASS_DEVICE},
        {"gunyah-channel",GGSK_BIT(GGSK_BOOT),GGSK_CLASS_DEVICE},
        {"share",GGSK_BIT(GGSK_GUNYAH)|GGSK_BIT(GGSK_HOST_MEMORY),GGSK_CLASS_DEVICE},
        {"smmu-active",GGSK_BIT(GGSK_POWER)|GGSK_BIT(GGSK_HOST_MEMORY),GGSK_CLASS_DEVICE},
        {"gmu-load",GGSK_BIT(GGSK_SMMU)|GGSK_BIT(GGSK_FIRMWARE)|GGSK_BIT(GGSK_HOST_MEMORY),GGSK_CLASS_DEVICE},
        {"gmu-start",GGSK_BIT(GGSK_GMU_LOAD),GGSK_CLASS_DEVICE},
        {"hfi-client",GGSK_BIT(GGSK_GMU_START),GGSK_CLASS_DEVICE},
        {"controller",GGSK_BIT(GGSK_SHARE)|GGSK_BIT(GGSK_SMMU)|GGSK_BIT(GGSK_GUNYAH),GGSK_CLASS_DEVICE},
        {"fence-provider",GGSK_BIT(GGSK_CONTROLLER)|GGSK_BIT(GGSK_GMU_START),GGSK_CLASS_DEVICE},
        {"submit",GGSK_BIT(GGSK_FENCE)|GGSK_BIT(GGSK_HFI),GGSK_CLASS_DEVICE},
        {"scanout",GGSK_BIT(GGSK_SUBMIT),GGSK_CLASS_DEVICE},
        {"metal-publish",GGSK_BIT(GGSK_SUBMIT)|GGSK_BIT(GGSK_SCANOUT),GGSK_CLASS_DEVICE}
    };
    return stage<GGSK_STAGES?&table[stage]:0;
}

enum ggsk_state { GGSK_UNSET=0, GGSK_READY, GGSK_ENTERED, GGSK_STOPPED, GGSK_BLOCKED, GGSK_FAILED };

enum ggsk_code { GGSK_OK=0, GGSK_ARGUMENT, GGSK_NO_PROVIDER, GGSK_BOUND, GGSK_FROZEN,
    GGSK_PREREQ, GGSK_POLICY, GGSK_NOT_READY, GGSK_ENTER, GGSK_LEAVE, GGSK_QUARANTINED };

struct ggsk_ops {
    void *cookie;
    /* Read-only and bounded. Returns 0 only when the live hardware, VM and
     * ownership state satisfies this stage's entry evidence, and writes that
     * evidence into *bits. A non-zero return leaves the stage blocked; the
     * framework keeps no earlier "ready" answer. */
    int (*probe)(void *cookie,uint64_t *bits);
    /* Forward transition. Reachable only through ggsk_advance(), after this
     * call's probe succeeded. Must not assume any later stage. */
    int (*enter)(void *cookie);
    /* Reverse transition. 0 means the stop is confirmed. Anything else means
     * the stop is unconfirmed, so the stack quarantines and stops unwinding. */
    int (*leave)(void *cookie);
};

struct ggsk_slot {
    const struct ggsk_ops *ops;
    uint64_t evidence;
    unsigned state,code,probes,enters,leaves;
};

struct ggsk_policy {
    unsigned allow_host_mutation;
    unsigned allow_device_writes;
};

struct ggsk {
    struct ggsk_policy policy;
    unsigned frozen,quarantined,entered_mask,reached,blocked,attempts;
    unsigned writes,host_mutations; /* cumulative, never decremented */
    struct ggsk_slot slot[GGSK_STAGES];
};

static inline const char *ggsk_stage_name(unsigned stage)
{
    const struct ggsk_desc *d=ggsk_desc_of(stage);
    return d?d->name:"invalid";
}

static inline const char *ggsk_class_name(unsigned klass)
{
    return klass==GGSK_CLASS_RO?"read-only":klass==GGSK_CLASS_HOST?"host-mutation":
        klass==GGSK_CLASS_DEVICE?"device-write":"invalid";
}

static inline const char *ggsk_state_name(unsigned state)
{
    const char *const names[]={"unset","ready","entered","stopped","blocked","failed"};
    return state<sizeof(names)/sizeof(names[0])?names[state]:"invalid";
}

static inline const char *ggsk_code_name(unsigned code)
{
    const char *const names[]={"ok","argument","no-provider","already-bound","frozen",
        "prerequisite","policy","not-ready","enter-failed","leave-unconfirmed","quarantined"};
    return code<sizeof(names)/sizeof(names[0])?names[code]:"invalid";
}

static inline unsigned ggsk_entered(const struct ggsk *s,unsigned stage)
{
    return s && stage<GGSK_STAGES?((s->entered_mask>>stage)&1u):0u;
}

static inline int ggsk_init(struct ggsk *s,const struct ggsk_policy *policy)
{
    unsigned stage;
    if (!s || !policy) return GGSK_ARGUMENT;
    s->policy.allow_host_mutation=policy->allow_host_mutation?1u:0u;
    s->policy.allow_device_writes=policy->allow_device_writes?1u:0u;
    s->frozen=s->quarantined=s->entered_mask=s->attempts=0;
    s->reached=0; s->blocked=0; s->writes=s->host_mutations=0;
    for (stage=0;stage<GGSK_STAGES;++stage) {
        struct ggsk_slot *slot=&s->slot[stage];
        slot->ops=0; slot->evidence=0; slot->state=GGSK_UNSET; slot->code=GGSK_NO_PROVIDER;
        slot->probes=slot->enters=slot->leaves=0;
    }
    return GGSK_OK;
}

/* One owner per stage. Registration closes at the first advance(): a provider
 * can never be swapped underneath an already executed chain. */
static inline int ggsk_register(struct ggsk *s,unsigned stage,const struct ggsk_ops *ops)
{
    if (!s || stage>=GGSK_STAGES || !ops || !ops->probe || !ops->enter || !ops->leave)
        return GGSK_ARGUMENT;
    if (s->frozen) return GGSK_FROZEN;
    if (s->slot[stage].ops) return GGSK_BOUND;
    s->slot[stage].ops=ops;
    s->slot[stage].code=GGSK_OK;
    return GGSK_OK;
}

static inline int ggsk_probe(struct ggsk *s,unsigned stage)
{
    struct ggsk_slot *slot;
    uint64_t evidence=0;
    int error,live;
    if (!s || stage>=GGSK_STAGES) return GGSK_ARGUMENT;
    slot=&s->slot[stage];
    if (!slot->ops) { slot->state=GGSK_UNSET; slot->code=GGSK_NO_PROVIDER; return GGSK_NO_PROVIDER; }
    live=(int)ggsk_entered(s,stage);
    ++slot->probes;
    error=slot->ops->probe(slot->ops->cookie,&evidence);
    if (error) {
        slot->evidence=0; slot->code=GGSK_NOT_READY;
        if (!live) slot->state=GGSK_BLOCKED;
        return GGSK_NOT_READY;
    }
    slot->evidence=evidence; slot->code=GGSK_OK;
    if (!live) slot->state=GGSK_READY;
    return GGSK_OK;
}

/* Status sweep: every registered stage probes itself, nothing is entered. */
static inline int ggsk_probe_all(struct ggsk *s)
{
    unsigned stage;
    int first=GGSK_OK;
    if (!s) return GGSK_ARGUMENT;
    for (stage=0;stage<GGSK_STAGES;++stage) {
        int error;
        if (!s->slot[stage].ops) continue;
        error=ggsk_probe(s,stage);
        if (error && first==GGSK_OK) first=error;
    }
    return first;
}

static inline void ggsk_summarise(struct ggsk *s)
{
    unsigned stage;
    s->reached=0; s->blocked=GGSK_STAGES;
    for (stage=GGSK_STAGES;stage>0;--stage)
        if (ggsk_entered(s,stage-1)) { s->reached=stage; break; }
    for (stage=0;stage<GGSK_STAGES;++stage)
        if (!ggsk_entered(s,stage)) { s->blocked=stage; break; }
}

/* Enter every stage up to and including target, in index order.
 *
 * A stage whose prerequisites are not entered is marked GGSK_PREREQ and the
 * walk continues with the next independent stage. The call succeeds only when
 * target itself is entered; otherwise it returns target's own code and the
 * summary points at the first gap. A device failure latches quarantine and
 * ends the walk immediately. */
static inline int ggsk_advance(struct ggsk *s,unsigned target)
{
    unsigned stage;
    if (!s || target>=GGSK_STAGES) return GGSK_ARGUMENT;
    if (s->quarantined) return GGSK_QUARANTINED;
    s->frozen=1;
    for (stage=0;stage<=target;++stage) {
        struct ggsk_slot *slot=&s->slot[stage];
        const struct ggsk_desc *desc=ggsk_desc_of(stage);
        int error;
        if (ggsk_entered(s,stage)) continue;
        if (!slot->ops) { slot->state=GGSK_UNSET; slot->code=GGSK_NO_PROVIDER; continue; }
        if (desc->prereq&~s->entered_mask) { slot->state=GGSK_BLOCKED; slot->code=GGSK_PREREQ; continue; }
        if (desc->klass==GGSK_CLASS_HOST && !s->policy.allow_host_mutation) {
            slot->state=GGSK_BLOCKED; slot->code=GGSK_POLICY; continue;
        }
        if (desc->klass==GGSK_CLASS_DEVICE && !s->policy.allow_device_writes) {
            slot->state=GGSK_BLOCKED; slot->code=GGSK_POLICY; continue;
        }
        ++s->attempts;
        if (ggsk_probe(s,stage)) continue; /* fresh read-only evidence before entry */
        ++slot->enters;
        error=slot->ops->enter(slot->ops->cookie);
        if (error) {
            slot->state=GGSK_FAILED; slot->code=GGSK_ENTER;
            if (desc->klass==GGSK_CLASS_DEVICE) { s->quarantined=1; break; }
            continue;
        }
        slot->state=GGSK_ENTERED; slot->code=GGSK_OK;
        s->entered_mask|=GGSK_BIT(stage);
        if (desc->klass==GGSK_CLASS_HOST) ++s->host_mutations;
        else if (desc->klass==GGSK_CLASS_DEVICE) ++s->writes;
    }
    ggsk_summarise(s);
    if (ggsk_entered(s,target)) return GGSK_OK;
    /* Report the first gap, so a blocked target never hides its real cause. */
    return s->blocked<GGSK_STAGES?s->slot[s->blocked].code:(unsigned)GGSK_PREREQ;
}

/* Leave entered stages in descending index order. The first unconfirmed stop
 * latches quarantine and abandons the remaining stages: they may still own
 * memory or DMA, so neither a retry nor an optimistic release is allowed. */
static inline int ggsk_retreat(struct ggsk *s)
{
    unsigned stage;
    if (!s) return GGSK_ARGUMENT;
    for (stage=GGSK_STAGES;stage>0;--stage) {
        struct ggsk_slot *slot=&s->slot[stage-1];
        int error;
        if (!ggsk_entered(s,stage-1)) continue;
        if (!slot->ops) return GGSK_NO_PROVIDER;
        ++slot->leaves;
        error=slot->ops->leave(slot->ops->cookie);
        if (error) {
            slot->state=GGSK_FAILED; slot->code=GGSK_LEAVE;
            s->quarantined=1; s->blocked=stage-1;
            return GGSK_LEAVE;
        }
        slot->state=GGSK_STOPPED; slot->code=GGSK_OK;
        s->entered_mask&=~GGSK_BIT(stage-1);
    }
    ggsk_summarise(s);
    return GGSK_OK;
}

/* Kernel entry points (giulia_gpu_stack.c): the same transitions plus bounded
 * logging, so the service never formats stage state itself. */
#ifdef __cplusplus
extern "C" {
#endif
int giulia_gpu_stack_init(struct ggsk *s,const struct ggsk_policy *policy);
int giulia_gpu_stack_register(struct ggsk *s,unsigned stage,const struct ggsk_ops *ops);
int giulia_gpu_stack_plan(struct ggsk *s);
int giulia_gpu_stack_advance(struct ggsk *s,unsigned target);
int giulia_gpu_stack_retreat(struct ggsk *s);
void giulia_gpu_stack_log(const struct ggsk *s);
#ifdef __cplusplus
}
#endif
#endif
