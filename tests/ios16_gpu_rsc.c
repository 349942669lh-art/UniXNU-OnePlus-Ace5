/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * RAM-only register model. Never run against a mapped device.
 */
#include "../ios16/gpu/giulia_gpu_rsc.h"
#include "rsc-source-layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

enum { WORDS=65536/4, EVENTS=512 };
struct event { unsigned write,offset,value; };
struct model {
    uint32_t regs[WORDS];
    struct event events[EVENTS];
    unsigned nevents,calls,live_calls,alive,lose_at,fail_at,ones_at;
    unsigned stuck_offset,stuck_enabled,callbacks_after_loss;
    uint64_t now;
};
static struct model m;
static struct ggrsc_state s;
static struct ggrsc_config cfg;
static unsigned cases;
static void finish_callback(void)
{
    ++m.calls;
    if(m.calls==m.lose_at)m.alive=0;
}
static int live(void *cookie,uint64_t epoch)
{ assert(cookie==&m && epoch==cfg.epoch);++m.live_calls;return (int)m.alive; }
static void event(unsigned wr,unsigned offset,unsigned value)
{
    assert(offset<65536 && !(offset&3));
    assert(m.nevents<EVENTS);
    m.events[m.nevents++]=(struct event){wr,offset,value};
    if(!m.alive)++m.callbacks_after_loss;
}
static int rd(void *cookie,uint32_t offset,uint32_t *value)
{
    assert(cookie==&m);event(0,offset,m.regs[offset/4]);
    *value=m.regs[offset/4];finish_callback();
    if(m.calls==m.ones_at)*value=0xffffffffu;
    return m.calls==m.fail_at ? -1 : 0;
}
static int wr(void *cookie,uint32_t offset,uint32_t value)
{
    assert(cookie==&m);event(1,offset,value);
    assert(offset!=0 && offset!=4); /* Never IRQ_ENABLE/IRQ_STATUS writes. */
    if(!(m.stuck_enabled && offset==m.stuck_offset)) {
        if(offset==8)m.regs[1]&=~value;
        else m.regs[offset/4]=value;
    }
    finish_callback();return m.calls==m.fail_at ? -1 : 0;
}
static int now_us(void *cookie,uint64_t *value)
{
    assert(cookie==&m);if(!m.alive)++m.callbacks_after_loss;
    *value=m.now;finish_callback();return m.calls==m.fail_at ? -1 : 0;
}
static int delay_us(void *cookie,unsigned us)
{
    assert(cookie==&m && us==1);if(!m.alive)++m.callbacks_after_loss;
    ++m.now;finish_callback();return m.calls==m.fail_at ? -1 : 0;
}
static struct ggrsc_io io={&m,live,rd,wr,now_us,delay_us};
static unsigned off(unsigned index)
{
    const unsigned *row=cfg.version==27 ? source_v27 : source_v30;
    return row[0]*cfg.tcs_id+row[index];
}
static void setup(unsigned version,unsigned id)
{
    memset(&m,0,sizeof(m));memset(&s,0,sizeof(s));memset(&cfg,0,sizeof(cfg));
    cfg.abi=GGRSC_ABI;cfg.version=version;cfg.mapped_bytes=65536;cfg.tcs_id=id;
    cfg.arc_address=0x30000;cfg.selector_mask=0xffff;cfg.timeout_us=10000;cfg.epoch=271;
    m.alive=1;m.now=100;m.regs[0]=1u<<id;m.regs[off(3)/4]=1;
    /* Foreign pending IRQ must remain untouched, even with id 31. */
    m.regs[1]=1u<<((id+1)%32);
}
static void complete(void)
{ m.regs[1]|=1u<<cfg.tcs_id;m.regs[off(8)/4]=source_done;m.regs[off(3)/4]=1; }
static void check_writes(uint32_t selector)
{
    /* Independent public-source register layout and expected full sequence.
     * v27/v30 offsets/constants parsed from HASH-PINNED Linux, not our core. */
    const unsigned offsets[]={off(2),off(2),off(5),off(6),off(7),off(1),off(4),off(2),off(2),
                              off(2),off(2),off(4),off(1),8};
    const unsigned values[]={0,0,source_message,cfg.arc_address,selector,1,1,1u<<16,(1u<<16)|(1u<<24),
                             1u<<16,0,0,0,1u<<cfg.tcs_id};
    unsigned j=0;
    for(unsigned i=0;i<m.nevents;i++)if(m.events[i].write) {
        assert(j<sizeof(values)/sizeof(values[0]));
        assert(m.events[i].offset==offsets[j] && m.events[i].value==values[j]);++j;
    }
    assert(j==sizeof(values)/sizeof(values[0]));
}
static unsigned success(unsigned version,unsigned id,unsigned selector)
{
    setup(version,id);assert(ggrsc_open(&s,&cfg,&io)==GGRSC_OK);
    assert(s.control==off(2) && s.cmd_status==off(8));
    uint64_t serial=0;assert(ggrsc_submit(&s,selector,&serial)==GGRSC_OK && serial==1);
    assert(s.inflight && s.trigger_attempted);
    assert(ggrsc_close(&s)==GGRSC_BUSY);
    assert(ggrsc_poll(&s,serial)==GGRSC_PENDING);
    complete();assert(ggrsc_poll(&s,serial)==GGRSC_OK);
    assert(!s.inflight && s.completed==1 && s.ready);
    assert(ggrsc_poll(&s,serial)==GGRSC_ARGUMENT);
    assert(m.regs[1]==(1u<<((id+1)%32)));
    assert(m.regs[0]==(1u<<id));check_writes(selector);
    assert(ggrsc_close(&s)==GGRSC_OK && s.closed);
    unsigned calls=m.calls;assert(ggrsc_open(&s,&cfg,&io)==GGRSC_USED);
    assert(m.calls==calls && !m.callbacks_after_loss);++cases;return calls;
}
static void scenario(void)
{
    int code=ggrsc_open(&s,&cfg,&io);
    if(code==GGRSC_OK) {
        uint64_t serial=0;code=ggrsc_submit(&s,3,&serial);
        if(code==GGRSC_OK) {complete();code=ggrsc_poll(&s,serial);}
        if(code==GGRSC_OK)code=ggrsc_close(&s);
    }
    assert(code!=GGRSC_OK && s.quarantined && !s.ready);
    unsigned calls=m.calls;
    uint64_t serial=99;
    assert(ggrsc_submit(&s,3,&serial)==GGRSC_QUARANTINED && !serial);
    assert(ggrsc_poll(&s,s.serial?s.serial:1)==GGRSC_QUARANTINED);
    assert(ggrsc_close(&s)==GGRSC_QUARANTINED);
    assert(m.calls==calls && !m.callbacks_after_loss);
    if(s.mutation_attempted)assert(s.serial==1);
    if(s.inflight)assert(s.serial==1 && !s.completed);
    ++cases;
}
static void clean_faults(void)
{
    for(unsigned version=27;version<=30;version+=3) {
        for(unsigned field=0;field<6;field++) {
            setup(version,3);
            if(field==0)m.regs[0]=0;
            if(field==1)m.regs[1]|=1u<<3;
            if(field==2)m.regs[off(2)/4]=1u<<24;
            if(field==3)m.regs[off(4)/4]=2;
            if(field==4)m.regs[off(1)/4]=1;
            if(field==5)m.regs[off(3)/4]=0;
            assert(ggrsc_open(&s,&cfg,&io)==GGRSC_PROTOCOL);
            assert(!s.mutation_attempted && s.quarantined);++cases;
        }
    }
}
static void pair_setup(unsigned version,unsigned id)
{
    setup(version,id);
    cfg.selector_mask=1u<<3;
    cfg.second_arc_address=0x30010;
    cfg.second_selector_mask=1u<<4;
}
static void first_resource(void)
{
    uint64_t serial=0;
    assert(!ggrsc_open(&s,&cfg,&io));
    assert(!ggrsc_submit(&s,3,&serial) && serial==1);
    complete();assert(!ggrsc_poll(&s,serial) && s.completed==1);
}
static int second_resource(void)
{
    uint64_t serial=0;
    int code=ggrsc_submit_arc(&s,0x30010,4,&serial);
    if(!code){complete();code=ggrsc_poll(&s,serial);}
    if(!code)code=ggrsc_close(&s);
    return code;
}
static void pair_tests(void)
{
    for(unsigned version=27;version<=30;version+=3) {
        for(unsigned id=0;id<32;id++) {
            pair_setup(version,id);first_resource();
            unsigned calls=m.calls;uint64_t serial=99;
            assert(ggrsc_submit_arc(&s,0,4,&serial)==GGRSC_ARGUMENT && !serial);
            assert(ggrsc_submit_arc(&s,0x30020,4,&serial)==GGRSC_ARGUMENT && !serial);
            assert(ggrsc_submit_arc(&s,0x30010,3,&serial)==GGRSC_ARGUMENT && !serial);
            assert(ggrsc_submit_arc(&s,0x30000,4,&serial)==GGRSC_ARGUMENT && !serial);
            assert(m.calls==calls && s.completed==1 && s.arc_address==0x30000);
            /* The copied binding survives caller changes. */
            cfg.second_arc_address=0x30020;cfg.second_selector_mask=1u<<5;
            assert(!ggrsc_submit_arc(&s,0x30010,4,&serial) && serial==2);
            assert(s.arc_address==0x30010 && m.regs[off(6)/4]==0x30010);
            assert(ggrsc_submit(&s,3,&serial)==GGRSC_BUSY && !serial);
            assert(s.serial==2 && s.arc_address==0x30010 && s.completed==1);
            complete();assert(!ggrsc_poll(&s,2));
            assert(!ggrsc_submit(&s,3,&serial) && serial==3 && s.arc_address==0x30000);
            complete();assert(!ggrsc_poll(&s,3) && !ggrsc_close(&s));++cases;
        }
        pair_setup(version,1);first_resource();unsigned before=m.calls;
        assert(!second_resource());unsigned boundaries=m.calls-before;
        for(unsigned boundary=1;boundary<=boundaries;boundary++) {
            for(unsigned loss=0;loss<2;loss++) {
                pair_setup(version,1);first_resource();
                if(loss)m.lose_at=m.calls+boundary;else m.fail_at=m.calls+boundary;
                assert(second_resource()!=GGRSC_OK && s.quarantined && s.completed>=1);
                assert(!m.callbacks_after_loss && s.serial<=2);
                if(s.serial==2)assert(s.arc_address==0x30010);
                unsigned calls=m.calls;uint64_t serial=99;
                assert(ggrsc_submit(&s,3,&serial)==GGRSC_QUARANTINED && !serial);
                assert(ggrsc_submit_arc(&s,0x30010,4,&serial)==GGRSC_QUARANTINED && !serial);
                assert(ggrsc_close(&s)==GGRSC_QUARANTINED && m.calls==calls);++cases;
            }
        }
    }
    for(unsigned bad=0;bad<6;bad++) {
        pair_setup(30,0);
        switch(bad) {
        case 0:cfg.second_arc_address=0;break;
        case 1:cfg.second_selector_mask=0;break;
        case 2:cfg.second_arc_address=cfg.arc_address;break;
        case 3:cfg.second_arc_address=0x40010;break;
        case 4:cfg.second_arc_address|=1;break;
        case 5:cfg.second_selector_mask=0x10000;break;
        }
        assert(ggrsc_open(&s,&cfg,&io)==GGRSC_ARGUMENT && !m.calls && !m.live_calls);++cases;
    }
    /* The old 40-byte config is accepted without reading ABI 2 fields. */
    setup(30,0);cfg.abi=GGRSC_LEGACY_ABI;
    size_t bytes=offsetof(struct ggrsc_config,second_arc_address);
    assert(bytes==40);void *legacy=malloc(bytes);assert(legacy);memcpy(legacy,&cfg,bytes);
    assert(!ggrsc_open(&s,legacy,&io));free(legacy);
    uint64_t serial=0;assert(!ggrsc_submit(&s,3,&serial));
    complete();assert(!ggrsc_poll(&s,serial) && !ggrsc_close(&s));++cases;
}
int main(void)
{
    pair_tests();
    assert(strcmp(ggrsc_identity(),GGRSC_IDENTITY)==0);
    for(unsigned ver=27;ver<=30;ver+=3)
        for(unsigned id=0;id<32;id++)for(unsigned selector=0;selector<16;selector++)success(ver,id,selector);
    /* Failure/loss on EVERY callback boundary in full successful lifecycle. */
    for(unsigned ver=27;ver<=30;ver+=3) {
        setup(ver,3);assert(!ggrsc_open(&s,&cfg,&io));uint64_t serial=0;
        assert(!ggrsc_submit(&s,3,&serial));complete();assert(!ggrsc_poll(&s,serial));assert(!ggrsc_close(&s));
        const unsigned calls=m.calls;
        for(unsigned i=1;i<=calls;i++) {
            setup(ver,3);m.lose_at=i;scenario();
            setup(ver,3);m.fail_at=i;scenario();
        }
        /* All-ones bus response on each READ boundary (other callback
         * positions do not count as an injected read failure). */
        for(unsigned i=1;i<=calls;i++) {
            setup(ver,3);m.ones_at=i;
            int code=ggrsc_open(&s,&cfg,&io);
            if(!code) {code=ggrsc_submit(&s,3,&serial);if(!code){complete();code=ggrsc_poll(&s,serial);}if(!code)code=ggrsc_close(&s);}
            if(code)assert(s.quarantined && !s.ready);
            ++cases;
        }
    }
    clean_faults();
    /* Bad config and callbacks never touch IO. */
    for(unsigned bad=0;bad<18;bad++) {
        setup(30,0);struct ggrsc_io test=io;
        switch(bad) {
        case 0:cfg.abi=GGRSC_ABI+1;break;case 1:cfg.version=31;break;case 2:cfg.tcs_id=32;break;
        case 3:cfg.epoch=0;break;case 4:cfg.arc_address=0x40000;break;
        case 5:cfg.arc_address=0x30001;break;case 6:cfg.selector_mask=0;break;
        case 7:cfg.selector_mask=0x10000;break;case 8:cfg.timeout_us=0;break;
        case 9:cfg.timeout_us=1000001;break;case 10:cfg.mapped_bytes=671;break;
        case 11:cfg.reserved=1;break;case 12:test.live=0;break;case 13:test.read32=0;break;
        case 14:test.write32=0;break;case 15:test.now_us=0;break;case 16:test.delay_us=0;break;
        case 17:cfg.mapped_bytes=65540;break;
        }
        assert(ggrsc_open(&s,&cfg,&test)==GGRSC_ARGUMENT && !m.calls && !m.live_calls);++cases;
    }
    /* Stale serial, busy and invalid selector are non-mutating. */
    setup(30,31);cfg.selector_mask=1u<<4;assert(!ggrsc_open(&s,&cfg,&io));uint64_t serial=0;
    unsigned calls=m.calls;assert(ggrsc_submit(&s,3,&serial)==GGRSC_ARGUMENT && !serial && m.calls==calls);
    assert(ggrsc_submit(&s,32,&serial)==GGRSC_ARGUMENT && m.calls==calls);
    assert(!ggrsc_submit(&s,4,&serial));calls=m.calls;
    uint64_t second=0;assert(ggrsc_submit(&s,4,&second)==GGRSC_BUSY && m.calls==calls);
    assert(ggrsc_poll(&s,serial+1)==GGRSC_ARGUMENT && m.calls==calls);++cases;
    complete();assert(!ggrsc_poll(&s,serial));assert(!ggrsc_submit(&s,4,&second) && second==2);
    calls=m.calls;assert(ggrsc_poll(&s,serial)==GGRSC_ARGUMENT && m.calls==calls);++cases;
    /* Completion evidence is strict; IRQ without issue+complete+idle isn't enough. */
    for(unsigned bad=0;bad<6;bad++) {
        setup(30,3);assert(!ggrsc_open(&s,&cfg,&io));assert(!ggrsc_submit(&s,3,&serial));complete();
        if(bad<4)m.regs[off(8)/4]=(unsigned[]){0,1u<<8,1u<<16,source_done|1}[bad];
        if(bad==4)m.regs[off(3)/4]=0;
        if(bad==5)m.regs[0]=0;
        assert(ggrsc_poll(&s,serial)==GGRSC_PROTOCOL && s.inflight && !s.completed);++cases;
    }
    /* Stalled readback stops after 16 attempts, preserving potentially written state. */
    setup(30,0);assert(!ggrsc_open(&s,&cfg,&io));m.stuck_enabled=1;m.stuck_offset=off(5);
    assert(ggrsc_submit(&s,1,&serial)==GGRSC_TIMEOUT && serial==1 && s.inflight && !s.trigger_attempted);
    assert(m.now==115 && s.quarantined);++cases;
    /* Timeouts, frozen clocks, clock reversal, epoch loss and sequence exhaustion. */
    setup(27,3);assert(!ggrsc_open(&s,&cfg,&io));assert(!ggrsc_submit(&s,3,&serial));
    m.now+=cfg.timeout_us;assert(ggrsc_poll(&s,serial)==GGRSC_TIMEOUT && s.inflight);++cases;
    setup(27,3);assert(!ggrsc_open(&s,&cfg,&io));assert(!ggrsc_submit(&s,3,&serial));
    for(unsigned i=0;i<4096;i++) {
        /* Bound per-call trace storage without altering device/state fixture. */
        m.nevents=0;assert(ggrsc_poll(&s,serial)==GGRSC_PENDING);
    }
    assert(ggrsc_poll(&s,serial)==GGRSC_TIMEOUT);++cases;
    setup(30,3);assert(!ggrsc_open(&s,&cfg,&io));assert(!ggrsc_submit(&s,3,&serial));
    --m.now;assert(ggrsc_poll(&s,serial)==GGRSC_PROTOCOL);++cases;
    setup(30,3);assert(!ggrsc_open(&s,&cfg,&io));s.serial=~(uint64_t)0;
    assert(ggrsc_submit(&s,3,&serial)==GGRSC_PROTOCOL && !serial);++cases;
    setup(30,3);assert(!ggrsc_open(&s,&cfg,&io));assert(!ggrsc_submit(&s,3,&serial));
    calls=m.calls;ggrsc_invalidate(&s);assert(s.quarantined && s.inflight && m.calls==calls);
    assert(ggrsc_poll(&s,serial)==GGRSC_QUARANTINED && m.calls==calls);++cases;
    printf("GPU RPMh RSC: %u RAM-only protocol/fault cases passed; no hardware authority, power or acceleration\n",cases);
    return 0;
}
