/* Actual decoder with RAM fixtures, not current-device DBs or power votes.
 * Original Linux oracle is called ONLY on constructed valid aligned images. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_arc.h"
int arc_source_resolve(const void *,const char *,uint32_t *,uint16_t *,unsigned *);
static const struct ggarc_policy policy={1,0x100}; /* Synthetic policy, not board evidence. */
enum { BASE=144, ARC0=BASE, ARC1=BASE+24, AUX=BASE+48, VRM=BASE+112, SIZE=BASE+160 };
struct fixture {uint32_t align;uint8_t bytes[SIZE];};
static unsigned cases;
static uint32_t rng=0x9e378134;
static uint32_t random32(void){rng=rng*1664525u+1013904223u;return rng;}
static void put16(uint8_t *p,unsigned n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n){put16(p,n&65535);put16(p+2,n>>16);}
static int zeros(const void *p,size_t size){const uint8_t *b=p;for(size_t i=0;i<size;i++)if(b[i])return 0;return 1;}
static uint32_t hash(const void *p,size_t size){const uint8_t *b=p;uint32_t h=2166136261u;for(size_t i=0;i<size;i++)h=(h^b[i])*16777619u;return h;}
static void header(uint8_t *p,unsigned id,unsigned off,unsigned data,unsigned count){
    put16(p,id);put16(p+2,off);put16(p+4,data);put16(p+6,count);put16(p+8,0x100);
}
static void entry(uint8_t *p,const char *id,uint32_t addr,unsigned bytes,unsigned offset){
    memcpy(p,id,strlen(id));put32(p+16,addr);put16(p+20,bytes);put16(p+22,offset);
}
static void build(struct fixture *f){
    memset(f,0,sizeof(*f));uint8_t *p=f->bytes;
    put32(p,1);p[4]=0xdb;p[5]=0x30;p[6]=3;p[7]=0x0c;
    header(p+8,3,0,48,2);header(p+24,4,112,136,1);
    entry(p+ARC0,"mxa.lvl",0x30000,32,0);entry(p+ARC1,"cx.lvl",0x30004,32,32);
    entry(p+VRM,"vrm.x",0x40000,4,0);
    const unsigned levels[]={0,16,48,64,80,128,192,256,320,384,416,480,0,0,0,0};
    for(unsigned i=0;i<16;i++){put16(p+AUX+2*i,levels[i]);put16(p+AUX+32+2*i,levels[i]);}
    /* Checksums/priorities are intentionally opaque, not authentication. */
    put32(p+136,0x12345678);put32(p+ARC0+8,0xcdef0123);put32(p+ARC0+12,0x74527891);
}
static int resolve(const struct fixture *f,struct ggarc_table *out){
    uint32_t before=hash(f,sizeof(*f));int code=ggarc_resolve(f->bytes,SIZE,&policy,"mxa.lvl",7,out);
    assert(before==hash(f,sizeof(*f)));if(code)assert(zeros(out,sizeof(*out)));return code;
}
static void compare_original(const struct fixture *f,const struct ggarc_table *t){
    uint32_t address=0;uint16_t levels[16]={0};unsigned count=0;
    assert(arc_source_resolve(f->bytes,"mxa.lvl",&address,levels,&count)==0);
    assert(t->address==address && t->count==count);
    for(unsigned i=0;i<count;i++)assert(t->levels[i]==levels[i]);
}
int main(void){
    struct fixture f;struct ggarc_table t;struct ggarc_plan plan;
    build(&f);assert(resolve(&f,&t)==GGARC_OK && t.raw_count==16 && t.count==12 && t.address==0x30000);
    compare_original(&f,&t);assert(strcmp(ggarc_identity(),GGARC_IDENTITY)==0);
    assert(ggarc_select(&t,72,100,&plan)==GGARC_OK && plan.selector==4 && plan.logical_level==80);
    assert(ggarc_select(&t,0,0,&plan)==GGARC_OK && plan.selector==0 && !plan.logical_level);
    assert(ggarc_select(&t,481,65535,&plan)==GGARC_RANGE && zeros(&plan,sizeof(plan)));
    assert(ggarc_select(&t,72,79,&plan)==GGARC_RANGE && zeros(&plan,sizeof(plan)));++cases;
    for(unsigned fault=0;fault<35;fault++){
        build(&f);uint8_t *p=f.bytes;int expect=GGARC_FORMAT;
        switch(fault){
        case 0:p[4]^=1;break;
        case 1:put32(p,2);expect=GGARC_VERSION;break;
        case 2:put16(p+8+8,0x101);expect=GGARC_VERSION;break;
        case 3:put32(p+140,1);expect=GGARC_STANDALONE;break;
        case 4:put32(p+140,2);break;
        case 5:put16(p+24,3);break; /* duplicate slave */
        case 6:put16(p+8,8);break;
        case 7:memset(p+24,0,16);header(p+40,4,112,136,1);break; /* hole */
        case 8:p[40+2]=1;break; /* nonzero inactive header */
        case 9:put16(p+8+10,1);break;
        case 10:put16(p+8+12,1);break;
        case 11:put16(p+8+14,1);break;
        case 12:put16(p+8+6,65535);expect=GGARC_BOUNDS;break;
        case 13:put16(p+8+2,65535);expect=GGARC_BOUNDS;break;
        case 14:put16(p+8+4,65535);expect=GGARC_BOUNDS;break;
        case 15:put16(p+24+2,24);break; /* entry table overlap */
        case 16:put16(p+ARC0+22,65535);expect=GGARC_BOUNDS;break;
        case 17:put16(p+VRM+20,65535);expect=GGARC_BOUNDS;break; /* even unrelated aux must fit */
        case 18:put16(p+8+4,0);break; /* aux aliases metadata */
        case 19:memcpy(p+ARC1,p+ARC0,8);expect=GGARC_AMBIGUOUS;break;
        case 20:put32(p+ARC0+16,0x40000);expect=GGARC_TYPE;break;
        case 21:put32(p+ARC0+16,0xb0000);expect=GGARC_TYPE;break;
        case 22:put32(p+ARC0+16,0x130000);expect=GGARC_TYPE;break;
        case 23:put32(p+ARC0+16,0x30002);expect=GGARC_TYPE;break;
        case 24:put32(p+ARC0+16,0);expect=GGARC_TYPE;break;
        case 25:put16(p+ARC0+20,0);expect=GGARC_LEVEL_DATA;break;
        case 26:put16(p+ARC0+20,3);expect=GGARC_LEVEL_DATA;break;
        case 27:put16(p+ARC0+20,34);expect=GGARC_LEVEL_DATA;break;
        case 28:put16(p+AUX+8,32);expect=GGARC_LEVEL_DATA;break; /* descending */
        case 29:put16(p+AUX+26,48);expect=GGARC_LEVEL_DATA;break; /* nonzero after padding */
        case 30:memset(p+ARC0,0,8);memcpy(p+ARC0,"other",5);expect=GGARC_NOT_FOUND;break;
        case 31:put16(p+8+6,1024);expect=GGARC_BOUNDS;break;
        case 32:put16(p+ARC1+22,1000);expect=GGARC_BOUNDS;break;
        case 33:put16(p+24+4,0);break; /* unrelated aux aliases ARC headers */
        case 34:put16(p+8,2);expect=GGARC_TYPE;break;
        }
        memset(&t,0xa5,sizeof(t));assert(resolve(&f,&t)==expect);++cases;
    }
    for(unsigned fault=0;fault<12;fault++){
        build(&f);assert(resolve(&f,&t)==GGARC_OK);int expect=GGARC_LEVEL_DATA;
        switch(fault){case 0:t.abi++;expect=GGARC_ARGUMENT;break;case 1:t.reserved=1;expect=GGARC_ARGUMENT;break;
        case 2:t.address=0;expect=GGARC_TYPE;break;case 3:t.raw_count=17;break;case 4:t.raw_count=0;break;
        case 5:t.count--;break;case 6:t.levels[13]=10;break;case 7:t.levels[4]=1;break;
        case 8:t.raw_count=12;t.levels[12]=1;break;
        case 9:expect=GGARC_ARGUMENT;break;case 10:expect=GGARC_ARGUMENT;break;case 11:expect=GGARC_ARGUMENT;break;}
        uint32_t min=fault==9?100:0,max=fault==9?99:fault==10?65536:fault==11?600000:80;
        memset(&plan,0xa5,sizeof(plan));assert(ggarc_select(&t,min,max,&plan)==expect && zeros(&plan,sizeof(plan)));++cases;
    }
    /* Valid original-Linux differential fixtures; no malformed input to oracle. */
    for(unsigned iteration=0;iteration<4096;iteration++){
        build(&f);unsigned raw=1+random32()%16,count=1+random32()%raw,level=random32()%2?0:16;
        put16(f.bytes+ARC0+20,raw*2);
        for(unsigned i=0;i<raw;i++){
            if(i<count){if(i)level+=random32()%35+1;put16(f.bytes+AUX+2*i,level);}
            else put16(f.bytes+AUX+2*i,0);
        }
        assert(resolve(&f,&t)==GGARC_OK && t.count==count && t.raw_count==raw);compare_original(&f,&t);
        for(unsigned req=0;req<16;req++){
            uint32_t min=random32()%600,max=min+random32()%120;unsigned selector=count;
            for(unsigned i=0;i<count;i++)if(t.levels[i]>=min && t.levels[i]<=max){selector=i;break;}
            int code=ggarc_select(&t,min,max,&plan);
            if(selector==count)assert(code==GGARC_RANGE && zeros(&plan,sizeof(plan)));
            else assert(code==GGARC_OK && plan.address==t.address && plan.selector==selector && plan.logical_level==t.levels[selector]);
            ++cases;
        }
        ++cases;
    }
    build(&f);put16(f.bytes+ARC0+20,6);put16(f.bytes+AUX,16);put16(f.bytes+AUX+2,16);put16(f.bytes+AUX+4,65535);
    assert(resolve(&f,&t)==GGARC_OK);compare_original(&f,&t);
    assert(ggarc_select(&t,0,0,&plan)==GGARC_RANGE); /* No OFF level! */
    assert(ggarc_select(&t,0,16,&plan)==GGARC_OK && plan.selector==0 && plan.logical_level==16);
    assert(ggarc_select(&t,65535,65535,&plan)==GGARC_OK && plan.selector==2);++cases;
    /* Bounded maximum entry count and wide sum of the two u16 AUX offsets. */
    {
        const unsigned payload=1024*24+32,size=BASE+payload;
        uint8_t *p=calloc(1,size);assert(p);build(&f);memcpy(p,f.bytes,8);
        header(p+8,3,0,1024*24,1024);
        entry(p+BASE+1023*24,"mxa.lvl",0x30000,4,0);
        put16(p+BASE+1024*24,0);put16(p+BASE+1024*24+2,80);
        assert(ggarc_resolve(p,size,&policy,"mxa.lvl",7,&t)==GGARC_OK && t.count==2);
        header(p+24,4,0,0,1); /* 1025 total, reject BEFORE table traversal. */
        assert(ggarc_resolve(p,size,&policy,"mxa.lvl",7,&t)==GGARC_BOUNDS && zeros(&t,sizeof(t)));
        free(p);++cases;
    }
    {
        const unsigned aux=65534u+65534u,size=BASE+aux+4;
        uint8_t *p=calloc(1,size);assert(p);build(&f);memcpy(p,f.bytes,8);
        header(p+8,3,0,65534u,1);entry(p+BASE,"mxa.lvl",0x30000,4,65534u);
        put16(p+BASE+aux,0);put16(p+BASE+aux+2,80);
        assert(ggarc_resolve(p,size,&policy,"mxa.lvl",7,&t)==GGARC_OK && t.count==2);
        assert(ggarc_select(&t,80,80,&plan)==GGARC_OK && plan.selector==1);
        assert(ggarc_resolve(p,size-1,&policy,"mxa.lvl",7,&t)==GGARC_BOUNDS);
        free(p);++cases;
    }
    /* Exact 8-byte identity, reject truncated/embedded-NUL requests. */
    build(&f);memcpy(f.bytes+ARC0,"abcdefgh",8);
    assert(ggarc_resolve(f.bytes,SIZE,&policy,"abcdefgh",8,&t)==GGARC_OK);
    assert(ggarc_resolve(f.bytes,SIZE,&policy,"abcdefghi",9,&t)==GGARC_ARGUMENT && zeros(&t,sizeof(t)));
    assert(ggarc_resolve(f.bytes,SIZE,&policy,"mxa.lvl\0",8,&t)==GGARC_ARGUMENT);
    assert(ggarc_resolve(f.bytes,SIZE,&policy,"a b",3,&t)==GGARC_ARGUMENT);++cases;
    /* No retained pointers: overwriting the input after resolve cannot alter output. */
    build(&f);assert(resolve(&f,&t)==GGARC_OK);memset(&f,0,sizeof(f));
    assert(ggarc_select(&t,320,320,&plan)==GGARC_OK && plan.selector==8);++cases;
    /* Arbitrary byte alignment is supported by the actual decoder, not oracle. */
    build(&f);for(unsigned offset=0;offset<8;offset++){
        uint8_t *storage=malloc(SIZE+8);assert(storage);memcpy(storage+offset,f.bytes,SIZE);
        assert(ggarc_resolve(storage+offset,SIZE,&policy,"mxa.lvl",7,&t)==GGARC_OK);free(storage);++cases;
    }
    /* ASan exact allocations check every truncation without access beyond length. */
    for(unsigned size=0;size<SIZE;size++){
        uint8_t *p=malloc(size?size:1);assert(p);memcpy(p,f.bytes,size);
        int code=ggarc_resolve(p,size,&policy,"mxa.lvl",7,&t);
        if(code)assert(zeros(&t,sizeof(t)));free(p);++cases;
    }
    /* Deterministic mutation corpus. No unsafe oracle call for mutated buffers. */
    for(unsigned iteration=0;iteration<20000;iteration++){
        build(&f);unsigned mutations=1+random32()%4;
        for(unsigned j=0;j<mutations;j++)f.bytes[random32()%SIZE]^=(uint8_t)(1+random32()%255);
        int code=resolve(&f,&t);
        if(!code){assert(t.count && t.count<=16);assert(ggarc_select(&t,0,65535,&plan)==GGARC_OK);}
        ++cases;
    }
    build(&f);assert(ggarc_resolve(NULL,SIZE,&policy,"x",1,&t)==GGARC_ARGUMENT && zeros(&t,sizeof(t)));
    assert(ggarc_resolve(f.bytes,SIZE,NULL,"x",1,&t)==GGARC_ARGUMENT);
    assert(ggarc_resolve(f.bytes,SIZE,&policy,NULL,1,&t)==GGARC_ARGUMENT);
    assert(ggarc_resolve(f.bytes,SIZE,&policy,"x",0,&t)==GGARC_ARGUMENT);
    assert(ggarc_resolve(f.bytes,SIZE,&policy,"x",1,NULL)==GGARC_ARGUMENT);
    assert(ggarc_resolve(f.bytes,(size_t)-1,&policy,"x",1,&t)==GGARC_BOUNDS);
    assert(ggarc_select(NULL,0,0,&plan)==GGARC_ARGUMENT);
    assert(ggarc_select(&t,0,0,NULL)==GGARC_ARGUMENT);++cases;
    printf("GPU ARC: %u RAM-only cases passed; original Linux valid-fixture differential=4096; hardware_verified=0\n",cases);
    return 0;
}
