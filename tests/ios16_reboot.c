/* Model the stock SPMI v7 one-byte SDAM operation and failure boundaries. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/platform/giulia_reboot.h"
static struct {
    uint32_t version,features,mapping[1024],owner[1024],status[4];
    unsigned byte,data,reads,writes,commands,mapped,deny_read,deny_write,timeout_read,timeout_write;
    unsigned map_fail,owner_race,corrupt,delay;
} h;
static uint32_t read32(void *ctx,unsigned space,unsigned offset)
{
    (void)ctx;
    if(space==GRB_CORE) {
        if(!offset)return h.version;
        if(offset==4)return h.features;
        assert(offset>=0x2000 && offset<0x3000 && !(offset&3));return h.mapping[(offset-0x2000)/4];
    }
    if(space==GRB_OWNER) {assert(offset<0x1000 && !(offset&3));return h.owner[offset/4];}
    assert(h.mapped && (space==GRB_OBSERVER || space==GRB_WRITER));
    if(offset==8)return h.status[space];
    assert(space==GRB_OBSERVER && offset==0x18);return h.byte;
}
static void write32(void *ctx,unsigned space,unsigned offset,uint32_t value)
{
    (void)ctx;assert(h.mapped);h.writes++;
    if(space==GRB_OBSERVER) {
        assert(offset==0 && value==0x08000480u);h.reads++;
        h.status[space]=h.timeout_read?0:h.deny_read?h.deny_read:1;
    } else {
        assert(space==GRB_WRITER);
        if(offset==0x10) {assert(value<256);h.data=value;return;}
        assert(offset==0 && value==0x480);h.commands++;
        h.status[space]=h.timeout_write?0:h.deny_write?h.deny_write:1;
        if(h.status[space]==1)h.byte=h.corrupt?0xab:h.data;
    }
}
static void delay_us(void *ctx,unsigned us){(void)ctx;assert(us==1);h.delay+=us;}
static int map_channel(void *ctx,unsigned apid)
{
    (void)ctx;assert(apid==3 || apid==7);h.mapped=apid+1;
    if(h.owner_race)h.owner[apid]=1;
    return !h.map_fail;
}
static struct grb_io io={0,read32,write32,delay_us,map_channel};
static void init(struct grb_state *s)
{
    memset(&h,0,sizeof(h));memset(s,0,sizeof(*s));
    h.version=0x70000000;h.features=16;h.mapping[3]=0x01007100;h.byte=0xa1;
    h.status[GRB_WRITER]=1;
}
int main(void)
{
    struct grb_state s;unsigned cases=0;
    for(unsigned mask=0;mask<128;mask++) {
        int allowed=grb_readonly_probe_allowed(mask&1,(mask>>1)&1,(mask>>2)&1,
            (mask>>3)&1,(mask>>4)&1,(mask>>5)&1,(mask>>6)&1);
        assert(allowed==(mask==27));cases++;
    }
    assert(!grb_readonly_probe_allowed(1,1,0,2,1,0,0));cases++;
    assert(!grb_readonly_probe_allowed(1,1,0,3,1,0,0));cases++;
    assert(!grb_readonly_probe_allowed(2,1,0,1,1,0,0));cases++;
    assert(!grb_readonly_probe_allowed(1,1,0,1,-1,0,0));cases++;
    const unsigned bytes[]={0,1,2,3,0x7e,0x7f,0xfe,0xff};
    for(unsigned i=0;i<sizeof(bytes)/sizeof(bytes[0]);i++) {
        init(&s);h.byte=bytes[i];assert(grb_prepare(&io,&s)==GRB_OK && s.armed);
        assert((h.byte>>1)==2 && (h.byte&1)==(bytes[i]&1));
        assert(h.commands==1 && h.reads==2);
        assert(grb_restore(&io,&s)==GRB_OK && h.byte==bytes[i] && !s.changed && !s.armed);cases++;
    }
    init(&s);h.version=0x50000000;assert(grb_prepare(&io,&s)==GRB_VERSION && !h.writes);cases++;
    init(&s);h.features=1025;assert(grb_prepare(&io,&s)==GRB_VERSION && !h.writes);cases++;
    init(&s);h.mapping[3]=0;assert(grb_prepare(&io,&s)==GRB_CHANNEL && !h.writes);cases++;
    init(&s);h.owner[3]=1;assert(grb_prepare(&io,&s)==GRB_CHANNEL && !h.writes);cases++;
    init(&s);h.mapping[7]=h.mapping[3];h.owner[3]=2;
    assert(grb_prepare(&io,&s)==GRB_OK && s.apid==7);cases++;
    init(&s);h.mapping[7]=h.mapping[3];h.owner[7]=2;
    assert(grb_prepare(&io,&s)==GRB_OK && s.apid==3);cases++;
    init(&s);h.map_fail=1;assert(grb_prepare(&io,&s)==GRB_MAP && !h.writes);cases++;
    init(&s);h.owner_race=1;assert(grb_prepare(&io,&s)==GRB_CHANGED && !h.writes);cases++;
    const unsigned status[]={3,5,9,0xffffffffu};
    for(unsigned i=0;i<sizeof(status)/sizeof(status[0]);i++) {
        init(&s);h.deny_read=status[i];assert(grb_prepare(&io,&s)==GRB_IO && !s.armed && !h.commands);cases++;
        init(&s);h.deny_write=status[i];assert(grb_prepare(&io,&s)==GRB_IO && !s.armed && h.commands==1);cases++;
    }
    init(&s);h.timeout_read=1;assert(grb_prepare(&io,&s)==GRB_TIMEOUT && !s.armed && h.delay==1000);cases++;
    init(&s);h.timeout_write=1;assert(grb_prepare(&io,&s)==GRB_TIMEOUT && !s.armed && h.commands==1 && h.delay==1000);cases++;
    init(&s);h.corrupt=1;assert(grb_prepare(&io,&s)==GRB_READBACK && !s.armed && h.commands==1);cases++;
    init(&s);assert(grb_prepare(&io,&s)==GRB_OK);unsigned writes=h.writes;
    assert(grb_prepare(&io,&s)==GRB_ARGUMENT && h.writes==writes);cases++;
    h.byte=0xab;assert(grb_restore(&io,&s)==GRB_CHANGED && h.byte==0xab && h.commands==1);cases++;
    init(&s);assert(grb_prepare(&io,&s)==GRB_OK);h.owner[3]=1;writes=h.writes;
    assert(grb_restore(&io,&s)==GRB_CHANGED && h.writes==writes);cases++;
    printf("Fastboot reboot: %u reason-bit, ownership, transaction failure, timeout and rollback cases passed\n",cases);
}
