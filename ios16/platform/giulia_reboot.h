/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5: PMK8550 SID 0 SDAM 0x7148[7:1] stores restart reason 2 (bootloader).
 * SPMI arbiter v7, primary bus, APPS EE0. See hardware/sm8650-reboot.json.
 * Only an EE0-owned channel for PPID 0x071 may write this one byte.
 */
#ifndef GIULIA_REBOOT_H
#define GIULIA_REBOOT_H
#include <stdint.h>
enum grb_space { GRB_CORE,GRB_OWNER,GRB_OBSERVER,GRB_WRITER };
enum grb_error { GRB_OK,GRB_ARGUMENT,GRB_VERSION,GRB_CHANNEL,GRB_MAP,
    GRB_IO,GRB_TIMEOUT,GRB_READBACK,GRB_CHANGED };
struct grb_io {
    void *context;
    uint32_t (*read)(void *,unsigned,unsigned);
    void (*write)(void *,unsigned,unsigned,uint32_t);
    void (*delay)(void *,unsigned);
    int (*map_channel)(void *,unsigned);
};
struct grb_state {
    uint32_t version,count,apid,mapping,status;
    unsigned before,expected,observed,changed,armed,error;
};
/* A completed root-selection read probe keeps BSD startup parked. Its explicit
 * host-only GPU profile has no mounted filesystem writer or GPU device work. */
static inline int grb_readonly_probe_allowed(unsigned completed,int probe,int writable,
    int gpu_mode,int host_only,int mmio,int power_test)
{
    return completed==1 && probe==1 && writable==0 && gpu_mode==1 &&
        host_only==1 && mmio==0 && power_test==0;
}
/* RM diagnostics retain their native channel until the whole-SoC reset.
 * This explicit rootless profile excludes GPU and raw-MMIO dispatch. */
static inline int grb_readonly_rm_probe_allowed(unsigned completed,int probe,int writable,
    int gpu_mode,int host_only,int mmio,int power_test,int rm_diagnostic)
{
    return completed==1 && probe==1 && writable==0 && gpu_mode==3 &&
        host_only==0 && mmio==0 && power_test==0 && rm_diagnostic==1;
}
static inline int grb_owned(const struct grb_io *b,const struct grb_state *s)
{
    return b->read(b->context,GRB_CORE,0x2000+4*s->apid)==s->mapping &&
        (b->read(b->context,GRB_OWNER,4*s->apid)&7)==0;
}
static inline int grb_wait(const struct grb_io *b,struct grb_state *s,unsigned space)
{
    for(unsigned i=0;i<1000;i++) {
        s->status=b->read(b->context,space,8);
        if(s->status==0xffffffffu || (s->status&14))return GRB_IO;
        if(s->status&1)return GRB_OK;
        b->delay(b->context,1);
    }
    return GRB_TIMEOUT;
}
static inline int grb_read_reason(const struct grb_io *b,struct grb_state *s,unsigned *value)
{
    if(!grb_owned(b,s))return GRB_CHANGED;
    b->write(b->context,GRB_OBSERVER,0,0x08000480u); /* EXT_READL, byte 0x48 */
    int e=grb_wait(b,s,GRB_OBSERVER);if(e)return e;
    *value=b->read(b->context,GRB_OBSERVER,0x18)&255u;return GRB_OK;
}
static inline int grb_write_reason(const struct grb_io *b,struct grb_state *s,unsigned value)
{
    if(!grb_owned(b,s))return GRB_CHANGED;
    b->write(b->context,GRB_WRITER,0x10,value&255u);
    b->write(b->context,GRB_WRITER,0,0x480u); /* EXT_WRITEL, exactly one byte */
    return grb_wait(b,s,GRB_WRITER);
}
static inline int grb_restore(const struct grb_io *b,struct grb_state *s)
{
    s->armed=0;
    if(!s->changed)return GRB_OK;
    /* Do not issue another command into a writer with unknown completion. */
    if(b->read(b->context,GRB_WRITER,8)!=1)return GRB_IO;
    unsigned value;int e=grb_read_reason(b,s,&value);if(e)return e;
    if(value!=s->expected && value!=s->before)return GRB_CHANGED;
    if(value!=s->before) {
        e=grb_write_reason(b,s,s->before);if(e)return e;
        e=grb_read_reason(b,s,&value);if(e)return e;
    }
    if(value!=s->before)return GRB_READBACK;
    s->changed=0;return GRB_OK;
}
static inline int grb_prepare(const struct grb_io *b,struct grb_state *s)
{
    if(!b || !s || !b->read || !b->write || !b->delay || !b->map_channel || s->changed || s->armed)
        return GRB_ARGUMENT;
    s->error=0;s->apid=1024;
    s->version=b->read(b->context,GRB_CORE,0);
    uint32_t features=b->read(b->context,GRB_CORE,4);s->count=features&0x7ff;
    if((s->version>>28)!=7 || features==0xffffffffu || !s->count || s->count>1024)
        return (int)(s->error=GRB_VERSION);
    for(unsigned i=0;i<s->count;i++) {
        uint32_t mapping=b->read(b->context,GRB_CORE,0x2000+4*i);
        if(mapping==0xffffffffu)return (int)(s->error=GRB_IO);
        if(mapping && ((mapping>>8)&0xfff)==0x71 &&
           (b->read(b->context,GRB_OWNER,4*i)&7)==0) {
            s->apid=i;s->mapping=mapping;
        }
    }
    if(s->apid==1024)return (int)(s->error=GRB_CHANNEL);
    if(!b->map_channel(b->context,s->apid))return (int)(s->error=GRB_MAP);
    int e=grb_read_reason(b,s,&s->before);if(e)return (int)(s->error=(unsigned)e);
    s->expected=(s->before&1u)|(2u<<1);s->changed=1;
    e=grb_write_reason(b,s,s->expected);
    if(!e)e=grb_read_reason(b,s,&s->observed);
    if(!e && s->observed!=s->expected)e=GRB_READBACK;
    if(e) {s->error=(unsigned)e;(void)grb_restore(b,s);return e;}
    s->armed=1;return GRB_OK;
}
#endif
