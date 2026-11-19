/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_DEV_H
#define GIULIA_GPU_DEV_H
#include <stdint.h>
#include <stddef.h>
/* CPU-only dispatch policy for the explicit RM diagnostic profile. */
static inline int ggd_rm_diagnostic_allowed(unsigned operation,unsigned action)
{ return operation==22 && (action==0 || action==5); }
enum { GGD_CHUNK=0x400000, GGD_CHUNKS=16, GGD_BYTES=GGD_CHUNK*GGD_CHUNKS,
 GGD_BASE=0x61000000, GGD_CACHE=0x800000, GGD_CACHE_BYTES=0x400000, GGD_MAX=128 };
struct ggd_allocation { uint32_t offset,bytes,flags,kind; };
struct ggd_pool { unsigned count,next,cached; struct ggd_allocation allocations[GGD_MAX]; };
/* Fixed gen7 get_mem_alloc_entry reserves ordinary SYSRAM for every kind
 * except the local secure and singleton resource backends below. In
 * particular enum names MMIO_DPU/TCSR/QDSS_STM/IPCC_AOSS do not select fixed
 * resources in that implementation. The HFI host normalizes GMU-only SYSRAM
 * permissions before using this explicit debug allocator; raw debug flags
 * retain their existing per-master mapping semantics. See
 * hardware/gpu-hfi-allocation-source.json for the exact source branches.
 */
static inline int ggd_kind_supported(uint32_t kind)
{
    return kind<26 && kind!=5 && kind!=19 && kind!=22 && kind!=23;
}
static inline int ggd_allocation_valid(const struct ggd_allocation *a)
{
    if(!a || !a->bytes || ((a->offset|a->bytes)&4095u) ||
       a->offset>=GGD_BYTES || a->bytes>GGD_BYTES-a->offset ||
       (a->flags&~0x3ffu) || !ggd_kind_supported(a->kind))return 0;
    return (a->flags&0x80u)?a->offset<GGD_CACHE_BYTES &&
        a->bytes<=GGD_CACHE_BYTES-a->offset:a->offset>=GGD_CACHE_BYTES;
}
static inline int ggd_pool_valid(const struct ggd_pool *p)
{
    if(!p || p->count>GGD_MAX || p->next<GGD_CACHE_BYTES || p->next>GGD_BYTES ||
       p->cached>GGD_CACHE_BYTES || ((p->next|p->cached)&4095u))return 0;
    for(unsigned i=0;i<p->count;i++) {
        const struct ggd_allocation *a=&p->allocations[i];
        if(!ggd_allocation_valid(a) || a->offset+a->bytes>((a->flags&0x80u)?p->cached:p->next))return 0;
        for(unsigned j=0;j<i;j++) {
            const struct ggd_allocation *b=&p->allocations[j];
            if(a->offset<b->offset+b->bytes && b->offset<a->offset+a->bytes)return 0;
        }
    }
    return 1;
}
/* A single owner must cover the complete range; gaps and adjacent allocations
 * never authorize one cross-allocation CPU write or firmware copy. */
static inline int ggd_owner(const struct ggd_pool *p,uint32_t at,uint32_t bytes)
{
    if(!bytes || !ggd_pool_valid(p))return -1;
    for(unsigned i=0;i<p->count;i++) {
        const struct ggd_allocation *a=&p->allocations[i];
        if(at>=a->offset && bytes<=a->bytes && at-a->offset<=a->bytes-bytes)return (int)i;
    }
    return -1;
}
static inline int ggd_alloc(struct ggd_pool *p,uint32_t bytes,uint32_t align,uint32_t flags,uint32_t kind)
{
    if(!ggd_pool_valid(p) || !bytes || bytes>GGD_BYTES || p->count>=GGD_MAX ||
       (flags&~0x3ffu) || !ggd_kind_supported(kind))return -1;
    if(align<4096)align=4096;
    if(align>GGD_CHUNK || (align&(align-1)))return -1;
    uint32_t *cursor=(flags&0x80)?&p->cached:&p->next;
    uint32_t limit=(flags&0x80)?GGD_CACHE_BYTES:GGD_BYTES;
    uint32_t at=(*cursor+align-1)&~(align-1), size=(bytes+4095)&~4095u;
    if(at>limit || size>limit-at)return -1;
    unsigned n=p->count++;
    p->allocations[n]=(struct ggd_allocation){at,size,flags,kind};*cursor=at+size;
    return (int)n;
}
static inline int ggd_bounds(uint32_t at,uint32_t bytes)
{return bytes && at<GGD_BYTES && bytes<=GGD_BYTES-at;}
/* Six decimal/hex u32s fit the existing 63-character wire line. */
static inline int ggd_parse(const char *p,uint32_t out[6])
{
    for(unsigned i=0;i<6;i++) {
        while(*p==' ')p++;
        unsigned base=10,digits=0;uint32_t value=0;
        if(p[0]=='0' && p[1]=='x'){base=16;p+=2;}
        while(*p && *p!=' ') {
            unsigned d=*p>='0'&&*p<='9'?*p-'0':*p>='a'&&*p<='f'?*p-'a'+10:99;
            if(d>=base || value>(UINT32_MAX-d)/base)return 0;
            value=value*base+d;p++;digits++;
        }
        if(!digits)return 0;
        out[i]=value;
    }
    while(*p==' ')p++;
    return !*p;
}
#endif
