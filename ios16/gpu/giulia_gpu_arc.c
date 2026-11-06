/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independently written bounded decoder of public Command DB/ARC wire fields.
 * No allocator, I/O, global state, firmware/RM access or transport callbacks.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_arc.h"

static uint16_t arc16(const uint8_t *p)
{ return (uint16_t)((unsigned)p[0]|((unsigned)p[1]<<8)); }
static uint32_t arc32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int arc_bounds(size_t size,size_t offset,size_t bytes)
{ return offset<=size && bytes<=size-offset; }
static int arc_overlap(size_t a,size_t an,size_t b,size_t bn)
{ return an && bn && a<b+bn && b<a+an; } /* All ranges already bounded. */
static int arc_address(uint32_t address)
{ return address && !(address&3u) && !(address&0xfff00000u) && ((address>>16)&15u)==GGARC_HW_ARC; }
static int arc_levels(const uint16_t *levels,unsigned raw,unsigned *count)
{
    if(!raw || raw>GGARC_LEVELS)return GGARC_LEVEL_DATA;
    unsigned n=raw;
    for(unsigned i=1;i<raw;i++) {
        if(!levels[i]) {
            n=i;
            for(unsigned j=i+1;j<raw;j++)if(levels[j])return GGARC_LEVEL_DATA;
            break;
        }
        if(levels[i]<levels[i-1])return GGARC_LEVEL_DATA;
    }
    *count=n;return GGARC_OK;
}
__attribute__((used)) const char *ggarc_identity(void) { return GGARC_IDENTITY; }
__attribute__((used)) int ggarc_resolve(const void *image,size_t size,const struct ggarc_policy *policy,
    const char *name,size_t name_bytes,struct ggarc_table *out)
{
    if(!out)return GGARC_ARGUMENT;
    const struct ggarc_table zero={0,0,0,0,0,0,0,{0}};*out=zero;
    if(!image || !policy || !name || !name_bytes || name_bytes>8)return GGARC_ARGUMENT;
    uint8_t query[8]={0};
    for(size_t i=0;i<name_bytes;i++) {
        if((unsigned char)name[i]<0x21 || (unsigned char)name[i]>0x7e)return GGARC_ARGUMENT;
        query[i]=(uint8_t)name[i];
    }
    if(size<GGARC_HEADER_BYTES || size>GGARC_MAX_BYTES)return GGARC_BOUNDS;
    const uint8_t *p=(const uint8_t *)image;
    if(p[4]!=0xdb || p[5]!=0x30 || p[6]!=0x03 || p[7]!=0x0c)return GGARC_FORMAT;
    if(arc32(p)!=policy->db_version)return GGARC_VERSION;
    const uint32_t flags=arc32(p+140);
    if(flags&~1u)return GGARC_FORMAT;
    if(flags&1u)return GGARC_STANDALONE;
    struct arc_slave {unsigned id,header,data,count,version;} slaves[8];
    unsigned nslaves=0,seen=0,total=0,ended=0;
    const size_t payload=size-GGARC_HEADER_BYTES;
    for(unsigned i=0;i<8;i++) {
        const uint8_t *h=p+8+16*i;
        const unsigned id=arc16(h);
        if(!id) {
            for(unsigned j=0;j<16;j++)if(h[j])return GGARC_FORMAT;
            ended=1;continue;
        }
        if(ended || id>7 || (seen&(1u<<id)) || arc16(h+10) || arc16(h+12) || arc16(h+14))return GGARC_FORMAT;
        seen|=1u<<id;
        const unsigned count=arc16(h+6),header=arc16(h+2),data=arc16(h+4);
        if(count>GGARC_MAX_ENTRIES-total)return GGARC_BOUNDS;
        total+=count;
        if(!arc_bounds(payload,header,(size_t)count*24) || data>payload)return GGARC_BOUNDS;
        for(unsigned j=0;j<nslaves;j++)
            if(arc_overlap(header,(size_t)count*24,slaves[j].header,(size_t)slaves[j].count*24))return GGARC_FORMAT;
        slaves[nslaves].id=id;slaves[nslaves].header=header;slaves[nslaves].data=data;
        slaves[nslaves].count=count;slaves[nslaves].version=arc16(h+8);++nslaves;
    }
    struct ggarc_table table=zero;unsigned matches=0;
    for(unsigned i=0;i<nslaves;i++) {
        const struct arc_slave *s=&slaves[i];
        for(unsigned j=0;j<s->count;j++) {
            const uint8_t *entry=p+GGARC_HEADER_BYTES+s->header+24*j;
            const unsigned bytes=arc16(entry+20);
            const size_t aux=(size_t)s->data+arc16(entry+22);
            if(!arc_bounds(payload,aux,bytes))return GGARC_BOUNDS;
            for(unsigned k=0;k<nslaves;k++)
                if(arc_overlap(aux,bytes,slaves[k].header,(size_t)slaves[k].count*24))return GGARC_FORMAT;
            unsigned equal=1;
            for(unsigned k=0;k<8;k++)if(entry[k]!=query[k])equal=0;
            if(!equal)continue;
            if(++matches!=1)return GGARC_AMBIGUOUS;
            if(s->id!=GGARC_HW_ARC || !arc_address(arc32(entry+16)))return GGARC_TYPE;
            if(s->version!=policy->arc_version)return GGARC_VERSION;
            if(!bytes || bytes>2*GGARC_LEVELS || (bytes&1u))return GGARC_LEVEL_DATA;
            table.abi=GGARC_ABI;table.db_version=arc32(p);table.arc_version=(uint16_t)s->version;
            table.address=arc32(entry+16);table.raw_count=(uint16_t)(bytes/2);
            for(unsigned k=0;k<table.raw_count;k++)table.levels[k]=arc16(p+GGARC_HEADER_BYTES+aux+2*k);
            unsigned count=0;int code=arc_levels(table.levels,table.raw_count,&count);
            if(code)return code;
            table.count=(uint16_t)count;
        }
    }
    if(!matches)return GGARC_NOT_FOUND;
    *out=table;return GGARC_OK;
}
__attribute__((used)) int ggarc_select(const struct ggarc_table *table,uint32_t min,uint32_t max,struct ggarc_plan *out)
{
    if(!out)return GGARC_ARGUMENT;
    const struct ggarc_plan zero={0,0,0};*out=zero;
    if(!table || table->abi!=GGARC_ABI || table->reserved || min>max || max>65535u)return GGARC_ARGUMENT;
    if(!arc_address(table->address))return GGARC_TYPE;
    unsigned count=0;int code=arc_levels(table->levels,table->raw_count,&count);
    if(code || count!=table->count)return GGARC_LEVEL_DATA;
    for(unsigned i=table->raw_count;i<GGARC_LEVELS;i++)if(table->levels[i])return GGARC_LEVEL_DATA;
    for(unsigned i=0;i<count;i++) {
        const unsigned level=table->levels[i];
        if(level<min)continue;
        if(level>max)return GGARC_RANGE;
        out->address=table->address;out->selector=i;out->logical_level=level;
        return GGARC_OK;
    }
    return GGARC_RANGE;
}
#endif
