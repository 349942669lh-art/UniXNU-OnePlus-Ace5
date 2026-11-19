/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_BOOT_STATE_H
#define GIULIA_GPU_BOOT_STATE_H
#if defined(GBS_FIRMWARE_TYPES)
typedef UINT8 GBS_U8;
typedef UINT16 GBS_U16;
typedef UINT32 GBS_U32;
typedef UINT64 GBS_U64;
#else
#include <stdint.h>
typedef uint8_t GBS_U8;
typedef uint16_t GBS_U16;
typedef uint32_t GBS_U32;
typedef uint64_t GBS_U64;
#endif

#define GBS_MAGIC 0x31534247u
#define GBS_VERSION 1u
#define GBS_BYTES 256u
#define GBS_PROPERTY "giulia-gpu-boot-state"
enum { GBS_REQUEST=1, GBS_CAPTURED, GBS_EBS_RETURNED };
enum { GBS_OK, GBS_ARGUMENT, GBS_PROTOCOL, GBS_COMMAND_DB_ERROR, GBS_CACHE };
enum { GBS_CACHE_AFTER_EBS=1u };

/* These are copied observations. No firmware pointers, native grant or
 * function pointers cross into XNU. The fixed two-resource snapshot carries
 * the actual resource address, numeric slave ID and complete ARC aux table.
 * A missing cached vote remains unknown. It must never become an OFF base. */
struct gbs_rail {
    GBS_U8 name[8];
    GBS_U32 address,slave,level_count;
    GBS_U16 levels[16];
    GBS_U32 cached_selector,cached_valid,cache_code;
};
struct gbs_report {
    GBS_U32 magic,version,bytes,phase,code,flags,crc32,rail_count;
    GBS_U64 status,capture_ticks,ebs_ticks,counter_frequency;
    GBS_U64 rpmh_version,command_db_version;
    GBS_U32 drv_id,reserved0;
    GBS_U64 reserved1;
    struct gbs_rail rail[2];
    GBS_U32 reserved[8];
};

static inline void gbs_zero(void *memory,unsigned bytes)
{
    GBS_U8 *p=(GBS_U8 *)memory;
    for(unsigned i=0;i<bytes;i++)p[i]=0;
}
static inline void gbs_copy(void *to,const void *from,unsigned bytes)
{
    GBS_U8 *d=(GBS_U8 *)to;
    const GBS_U8 *s=(const GBS_U8 *)from;
    for(unsigned i=0;i<bytes;i++)d[i]=s[i];
}
static inline GBS_U32 gbs_crc(const struct gbs_report *r)
{
    const GBS_U8 *p=(const GBS_U8 *)r;
    GBS_U32 crc=0xffffffffu;
    for(unsigned i=0;i<GBS_BYTES;i++) {
        GBS_U32 value=i>=24 && i<28 ? 0 : p[i];
        crc^=value;
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}
static inline void gbs_init(struct gbs_report *r)
{
    gbs_zero(r,sizeof(*r));
    r->magic=GBS_MAGIC;r->version=GBS_VERSION;r->bytes=GBS_BYTES;r->phase=GBS_REQUEST;
}
static inline int gbs_requested(const struct gbs_report *r)
{
    struct gbs_report pristine;
    const GBS_U8 *a=(const GBS_U8 *)r,*b=(const GBS_U8 *)&pristine;
    gbs_init(&pristine);
    for(unsigned i=0;i<sizeof(pristine);i++)if(a[i]!=b[i])return 0;
    return 1;
}
static inline int gbs_rail_valid(const struct gbs_rail *rail,unsigned index)
{
    static const GBS_U8 names[2][8]={{'c','x','.','l','v','l',0,0},{'m','x','.','l','v','l',0,0}};
    if(index>=2)return 0;
    for(unsigned i=0;i<8;i++)if(rail->name[i]!=names[index][i])return 0;
    if(!rail->address || (rail->address&0xfff00003u) ||
       ((rail->address>>16)&15u)!=3u || rail->slave!=3 ||
       !rail->level_count || rail->level_count>16 || rail->cached_valid>1 || rail->cache_code)
        return 0;
    for(unsigned i=0;i<16;i++) {
        if(i>=rail->level_count) {if(rail->levels[i])return 0;}
        else if(i && rail->levels[i]<=rail->levels[i-1])return 0;
    }
    if(rail->cached_valid)return rail->cached_selector<rail->level_count;
    return rail->cached_selector==0;
}
static inline int gbs_valid(const struct gbs_report *r)
{
    if(!r || sizeof(*r)!=GBS_BYTES || r->magic!=GBS_MAGIC || r->version!=GBS_VERSION ||
       r->bytes!=GBS_BYTES || (r->phase!=GBS_CAPTURED && r->phase!=GBS_EBS_RETURNED) ||
       r->code>GBS_CACHE || (r->flags&~GBS_CACHE_AFTER_EBS) || r->rail_count>2 ||
       r->reserved0 || r->reserved1 || r->crc32!=gbs_crc(r))return 0;
    for(unsigned i=0;i<8;i++)if(r->reserved[i])return 0;
    if(r->phase==GBS_CAPTURED && (r->ebs_ticks || r->flags))return 0;
    if(r->phase==GBS_EBS_RETURNED && (r->ebs_ticks<r->capture_ticks ||
       (r->code==GBS_OK && r->flags!=GBS_CACHE_AFTER_EBS)))return 0;
    if(r->code!=GBS_OK)return r->status!=0;
    return !r->status && r->rail_count==2 && r->drv_id==2 &&
        r->rpmh_version==0x10001 && r->command_db_version==0x10000 &&
        r->counter_frequency && r->counter_frequency<=1000000000u &&
        gbs_rail_valid(&r->rail[0],0) && gbs_rail_valid(&r->rail[1],1) &&
        r->rail[0].address!=r->rail[1].address;
}
static inline void gbs_seal(struct gbs_report *r)
{ r->crc32=gbs_crc(r); }
static inline int gbs_decode(const void *data,unsigned bytes,struct gbs_report *out)
{
    if(!out)return 0;
    gbs_zero(out,sizeof(*out));
    if(!data || bytes!=sizeof(*out))return 0;
    struct gbs_report value;
    gbs_copy(&value,data,sizeof(value));
    if(!gbs_valid(&value))return 0;
    *out=value;return 1;
}
#endif
