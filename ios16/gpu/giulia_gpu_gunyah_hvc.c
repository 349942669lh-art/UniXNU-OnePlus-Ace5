/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if defined(GG_GUNYAH_HVC_STANDALONE)
#include <stdint.h>
#include <stddef.h>
#else
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_GUNYAH_HVC_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gunyah_hvc.h"

#if defined(GG_GUNYAH_HVC_TEST)
#if !defined(GG_GUNYAH_HVC_STANDALONE)
#error Gunyah HVC test hook is standalone only
#endif
extern void ggh_hvc_test_invoke(struct ggh_hvc_regs *);
extern void ggh_hvc_test_doorbell_invoke(struct ggh_hvc_regs *);
/* Standalone fixtures that only exercise MSGQ/doorbell need no new symbol.
 * An absent probe hook fails closed and can never reach a native HVC. */
#if defined(__APPLE__)
extern void ggh_hvc_test_probe_invoke(struct ggh_hvc_regs *) __attribute__((weak_import));
#else
extern void ggh_hvc_test_probe_invoke(struct ggh_hvc_regs *) __attribute__((weak));
#endif
static void ggh_hvc_native(struct ggh_hvc_regs *r) { ggh_hvc_test_invoke(r); }
static void ggh_hvc_doorbell_native(struct ggh_hvc_regs *r)
{ ggh_hvc_test_doorbell_invoke(r); }
static int ggh_hvc_probe_native(struct ggh_hvc_regs *r)
{
    if(!ggh_hvc_test_probe_invoke)return 0;
    ggh_hvc_test_probe_invoke(r);return 1;
}
#else
#if !defined(__aarch64__)
#error Gunyah HVC requires AArch64
#endif
/* Private primitives: probe and MSGQ construct four fixed SMCCC functions;
 * doorbell has one fixed legacy immediate. No caller selects a function ID.
 * Canonical RM uses hvc #0 with the SMCCC function in x0. It does not use the
 * legacy hvc #0x601b/#0x601c ABI. Preserve Darwin's reserved x18 explicitly;
 * x19 holds the save-slot address and is preserved across the hypercall.
 * A separate stack slot avoids depending on a caller-clobbered register. */
#define GGH_NATIVE(name,instruction) \
static __attribute__((noinline)) void name(struct ggh_hvc_regs *r) \
{ \
    uint64_t saved_x18; \
    register uint64_t x0 __asm__("x0")=r->x[0]; \
    register uint64_t x1 __asm__("x1")=r->x[1]; \
    register uint64_t x2 __asm__("x2")=r->x[2]; \
    register uint64_t x3 __asm__("x3")=r->x[3]; \
    register uint64_t x4 __asm__("x4")=r->x[4]; \
    register uint64_t x5 __asm__("x5")=r->x[5]; \
    register uint64_t x6 __asm__("x6")=r->x[6]; \
    register uint64_t x7 __asm__("x7")=r->x[7]; \
    register uint64_t *slot __asm__("x19")=&saved_x18; \
    __asm__ volatile("str x18, [%[slot]]\n" instruction \
        "ldr x18, [%[slot]]\n" \
        "str xzr, [%[slot]]\n" \
        : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3), \
          "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) \
        : [slot] "r"(slot) \
        : "x8","x9","x10","x11","x12","x13","x14","x15", \
          "x16","x17","cc","memory"); \
    r->x[0]=x0;r->x[1]=x1;r->x[2]=x2;r->x[3]=x3; \
    r->x[4]=x4;r->x[5]=x5;r->x[6]=x6;r->x[7]=x7; \
}
GGH_NATIVE(ggh_hvc_native,"hvc #0\n")
GGH_NATIVE(ggh_hvc_doorbell_native,"hvc #0x6013\n")
#undef GGH_NATIVE
static int ggh_hvc_probe_native(struct ggh_hvc_regs *r)
{ggh_hvc_native(r);return 1;}
#endif

struct ggh_hvc_lease {
    uint64_t epoch,tx_cap,rx_cap;
    void *cookie;
    int (*live)(void *,uint64_t,uint64_t,uint64_t);
};

static int ggh_hvc_object(const void *data,size_t size)
{
    return data && size &&
        size-1<=(uintptr_t)-1-(uintptr_t)data;
}
static int ggh_hvc_range(const void *data,size_t size)
{return size<=GGH_HVC_MAX_BYTES && ggh_hvc_object(data,size);}

/* Called only after each range has been checked not to wrap. */
static int ggh_hvc_overlap(const void *a,size_t na,const void *b,size_t nb)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y ? y-x<na : x-y<nb;
}

struct ggh_hvc_probe_lease {
    uint64_t epoch;
    void *cookie;
    int (*live)(void *,uint64_t);
};
static int ggh_hvc_probe_same(const struct ggh_hvc_probe *h,
    const struct ggh_hvc_probe_lease *s)
{
    return h->epoch==s->epoch && h->lease_cookie==s->cookie && h->live==s->live;
}
static int ggh_hvc_probe_live(const struct ggh_hvc_probe *h,
    const struct ggh_hvc_probe_lease *s)
{
    return !h->quarantined && ggh_hvc_probe_same(h,s) &&
        s->live(s->cookie,s->epoch)==1 && !h->quarantined && ggh_hvc_probe_same(h,s);
}
static int ggh_hvc_probe_leave(struct ggh_hvc_probe *h,unsigned result)
{
    if(result!=GGH_PROBE_OK)h->quarantined=1;
    h->last_result=result;
    __atomic_store_n(&h->busy,0u,__ATOMIC_RELEASE);return (int)result;
}
static int ggh_hvc_probe_call(struct ggh_hvc_probe *h,
    const struct ggh_hvc_probe_lease *s,uint64_t function,struct ggh_hvc_regs *out)
{
    struct ggh_hvc_regs r={{0}};r.x[0]=function;
    if(!ggh_hvc_probe_live(h,s))return GGH_PROBE_LEASE;
    if(!ggh_hvc_probe_native(&r))return GGH_PROBE_UNAVAILABLE;
    h->last=r;*out=r;++h->attempted;
    return ggh_hvc_probe_live(h,s)?GGH_PROBE_OK:GGH_PROBE_LEASE;
}
int ggh_hvc_probe_compatibility(struct ggh_hvc_probe *h,struct ggh_hvc_compatibility *out)
{
    if(!ggh_hvc_object(h,sizeof(*h)) || (uintptr_t)h%__alignof__(*h) ||
       !ggh_hvc_object(out,sizeof(*out)) || (uintptr_t)out%__alignof__(*out) ||
       ggh_hvc_overlap(h,sizeof(*h),out,sizeof(*out)))return GGH_PROBE_ARGUMENT;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&h->busy,&expected,1u,0,__ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED))return GGH_PROBE_BUSY;
    if(h->quarantined || h->complete || h->attempted) {
        __atomic_store_n(&h->busy,0u,__ATOMIC_RELEASE);return GGH_PROBE_USED;
    }
    struct ggh_hvc_probe_lease s={h->epoch,h->lease_cookie,h->live};
    if(!s.epoch || !s.live)return ggh_hvc_probe_leave(h,GGH_PROBE_LEASE);
    h->observed.epoch=s.epoch;
    int result=ggh_hvc_probe_call(h,&s,UINT64_C(0x8600ff01),&h->observed.uid);
    if(result)return ggh_hvc_probe_leave(h,(unsigned)result);
    /* UUID_INIT serializes c1d58fcd-a453-5fdb-9265-ce36673d5f14 as bytes.
     * arch_is_gh_guest stores lower32(a0..a3) into little-endian u32 slots.
     * Pinned linux/{arm-smccc.h,uuid.h}, gunyah_hypercall.c:11-29. */
    const uint32_t uid[4]={UINT32_C(0xcd8fd5c1),UINT32_C(0xdb5f53a4),
        UINT32_C(0x36ce6592),UINT32_C(0x145f3d67)};
    for(unsigned i=0;i<4;i++)if((uint32_t)h->observed.uid.x[i]!=uid[i])
        return ggh_hvc_probe_leave(h,GGH_PROBE_IDENTITY);
    result=ggh_hvc_probe_call(h,&s,UINT64_C(0xc6008000),&h->observed.identify);
    if(result)return ggh_hvc_probe_leave(h,(unsigned)result);
    uint64_t api=h->observed.identify.x[0];
    /* include/linux/gunyah.h:154-169: low14 version, BE14, 64bit15,
     * variant63:56; other API-info bits are reserved and zero in API1.
     * rsc_mgr.c:839-858 accepts version1. This adapter additionally requires
     * the little-endian 64-bit ABI its native buffer/register code uses. */
    if((api&UINT64_C(0x3fff))!=1 || (api&UINT64_C(0x4000)) ||
       !(api&UINT64_C(0x8000)) || (api&~UINT64_C(0xff0000000000ffff)))
        return ggh_hvc_probe_leave(h,GGH_PROBE_ABI);
    struct ggh_hvc_compatibility snapshot=h->observed;
    if(!ggh_hvc_probe_live(h,&s))return ggh_hvc_probe_leave(h,GGH_PROBE_LEASE);
    *out=snapshot;h->complete=1;return ggh_hvc_probe_leave(h,GGH_PROBE_OK);
}

static int ggh_hvc_same(const struct ggh_hvc *h,const struct ggh_hvc_lease *s)
{
    return h->epoch==s->epoch && h->tx_cap==s->tx_cap && h->rx_cap==s->rx_cap &&
        h->lease_cookie==s->cookie && h->live==s->live;
}

static int ggh_hvc_live(const struct ggh_hvc *h,const struct ggh_hvc_lease *s)
{
    if(!ggh_hvc_same(h,s))return 0;
    return s->live(s->cookie,s->epoch,s->tx_cap,s->rx_cap)==1 &&
        ggh_hvc_same(h,s);
}

static int ggh_hvc_begin(struct ggh_hvc *h,struct ggh_hvc_lease *s)
{
    if(!h)return 0;
    h->attempted=0;
    s->epoch=h->epoch;s->tx_cap=h->tx_cap;s->rx_cap=h->rx_cap;
    s->cookie=h->lease_cookie;s->live=h->live;
    return s->epoch &&
        s->tx_cap!=(uint64_t)-1 && s->rx_cap!=(uint64_t)-1 &&
        s->live && ggh_hvc_live(h,s);
}

int ggh_hvc_valid(void *cookie,uint64_t epoch)
{
    struct ggh_hvc *h=(struct ggh_hvc *)cookie;
    struct ggh_hvc_lease s;
    if(!h || !epoch || h->epoch!=epoch)return 0;
    s.epoch=h->epoch;s.tx_cap=h->tx_cap;s.rx_cap=h->rx_cap;
    s.cookie=h->lease_cookie;s.live=h->live;
    return s.tx_cap!=(uint64_t)-1 && s.rx_cap!=(uint64_t)-1 &&
        s.live && ggh_hvc_live(h,&s);
}

static int ggh_hvc_finish(struct ggh_hvc *h,const struct ggh_hvc_lease *s,
    struct ggh_hvc_regs *r)
{
    if(!ggh_hvc_live(h,s))return 0;
    ggh_hvc_native(r);
    h->last=*r;h->native_code=r->x[0];h->attempted=1;
    return ggh_hvc_live(h,s);
}

int ggh_hvc_send(void *cookie,const void *data,size_t size,unsigned flags)
{
    struct ggh_hvc *h=(struct ggh_hvc *)cookie;
    struct ggh_hvc_lease s;
    struct ggh_hvc_regs r={{0}};
    uint8_t scratch[GGH_HVC_MAX_BYTES]={0};
    size_t i;
    if(h)h->attempted=0;
    if(!h || !ggh_hvc_range(data,size) ||
        ggh_hvc_overlap(data,size,h,sizeof(*h)) || (flags&~GGH_HVC_PUSH) ||
        !ggh_hvc_begin(h,&s))return GGH_HVC_ERROR;
    for(i=0;i<size;++i)scratch[i]=((const uint8_t *)data)[i];
    r.x[0]=0xc600801bULL;r.x[1]=s.tx_cap;r.x[2]=size;
    r.x[3]=(uintptr_t)scratch;r.x[4]=flags;
    if(!ggh_hvc_finish(h,&s,&r))return GGH_HVC_ERROR;
    if(r.x[0]==61)return GGH_HVC_AGAIN;
    if(r.x[0])return GGH_HVC_ERROR;
    return GGH_HVC_OK;
}

int ggh_hvc_recv(void *cookie,void *data,size_t capacity,size_t *received)
{
    struct ggh_hvc *h=(struct ggh_hvc *)cookie;
    struct ggh_hvc_lease s;
    struct ggh_hvc_regs r={{0}};
    uint8_t scratch[GGH_HVC_MAX_BYTES]={0};
    size_t i;
    if(h)h->attempted=0;
    if(!h || !ggh_hvc_range(data,capacity) ||
        !ggh_hvc_range(received,sizeof(*received)) ||
        (uintptr_t)received%__alignof__(size_t) ||
        ggh_hvc_overlap(data,capacity,h,sizeof(*h)) ||
        ggh_hvc_overlap(received,sizeof(*received),h,sizeof(*h)) ||
        ggh_hvc_overlap(data,capacity,received,sizeof(*received)) ||
        !ggh_hvc_begin(h,&s))return GGH_HVC_ERROR;
    r.x[0]=0xc600801cULL;r.x[1]=s.rx_cap;
    r.x[2]=(uintptr_t)scratch;r.x[3]=capacity;
    if(!ggh_hvc_finish(h,&s,&r))return GGH_HVC_ERROR;
    if(r.x[0]==60)return GGH_HVC_AGAIN;
    if(r.x[0] || !r.x[1] || r.x[1]>capacity)return GGH_HVC_ERROR;
    for(i=0;i<(size_t)r.x[1];++i)((uint8_t *)data)[i]=scratch[i];
    *received=(size_t)r.x[1];
    return GGH_HVC_OK;
}

struct ggh_hvc_doorbell_lease {
    uint64_t epoch,rx_cap;
    void *cookie;
    int (*live)(void *,uint64_t,uint64_t);
};
static int ggh_hvc_doorbell_same(const struct ggh_hvc_doorbell *h,
    const struct ggh_hvc_doorbell_lease *s)
{
    return h->epoch==s->epoch && h->rx_cap==s->rx_cap &&
        h->lease_cookie==s->cookie && h->live==s->live;
}
static int ggh_hvc_doorbell_live(const struct ggh_hvc_doorbell *h,
    const struct ggh_hvc_doorbell_lease *s)
{
    if(!ggh_hvc_doorbell_same(h,s))return 0;
    return s->live(s->cookie,s->epoch,s->rx_cap)==1 &&
        ggh_hvc_doorbell_same(h,s);
}
int ggh_hvc_doorbell_valid(void *cookie,uint64_t epoch)
{
    struct ggh_hvc_doorbell *h=(struct ggh_hvc_doorbell *)cookie;
    struct ggh_hvc_doorbell_lease s;
    if(!h || !epoch || h->epoch!=epoch)return 0;
    s.epoch=h->epoch;s.rx_cap=h->rx_cap;
    s.cookie=h->lease_cookie;s.live=h->live;
    return s.rx_cap!=(uint64_t)-1 && s.live && ggh_hvc_doorbell_live(h,&s);
}
int ggh_hvc_doorbell_read_clean(void *cookie,uint64_t clear_flags,uint64_t *previous)
{
    struct ggh_hvc_doorbell *h=(struct ggh_hvc_doorbell *)cookie;
    struct ggh_hvc_doorbell_lease s;
    struct ggh_hvc_regs r={{0}};
    if(h)h->attempted=0;
    if(!h || !ggh_hvc_range(previous,sizeof(*previous)) ||
        (uintptr_t)previous%__alignof__(uint64_t) ||
        ggh_hvc_overlap(previous,sizeof(*previous),h,sizeof(*h)))return GGH_HVC_ERROR;
    s.epoch=h->epoch;s.rx_cap=h->rx_cap;
    s.cookie=h->lease_cookie;s.live=h->live;
    if(!s.epoch || s.rx_cap==(uint64_t)-1 || !s.live ||
        !ggh_hvc_doorbell_live(h,&s))return GGH_HVC_ERROR;
    r.x[0]=s.rx_cap;r.x[1]=clear_flags;
    if(!ggh_hvc_doorbell_live(h,&s))return GGH_HVC_ERROR;
    ggh_hvc_doorbell_native(&r);
    h->last=r;h->native_code=r.x[0];h->attempted=1;
    if(!ggh_hvc_doorbell_live(h,&s) || r.x[0])return GGH_HVC_ERROR;
    *previous=r.x[1];return GGH_HVC_OK;
}
#endif
