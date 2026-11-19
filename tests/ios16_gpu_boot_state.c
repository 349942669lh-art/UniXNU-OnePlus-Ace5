/* RAM-only firmware protocol and ADT fixture. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef char CHAR8;
typedef void VOID;
typedef unsigned char BOOLEAN;
typedef UINT64 EFI_STATUS;
typedef UINTN EFI_TPL;
typedef struct {UINT32 a;UINT16 b,c;UINT8 d[8];} EFI_GUID;
#define CONST const
#define STATIC static
#define EFIAPI
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS UINT64_C(0)
#define EFI_INVALID_PARAMETER UINT64_C(0x8000000000000002)
#define EFI_UNSUPPORTED UINT64_C(0x8000000000000003)
#define EFI_NOT_READY UINT64_C(0x8000000000000006)
#define EFI_DEVICE_ERROR UINT64_C(0x8000000000000007)
#define EFI_NOT_FOUND UINT64_C(0x800000000000000e)
#define EFI_COMPROMISED_DATA UINT64_C(0x8000000000000021)
#define EFI_ERROR(x) (((x)>>63)!=0)
#define DEBUG(x) ((void)0)
#define DEBUG_ERROR 0
#define TPL_NOTIFY 16
#define GBS_HOST_TEST 1
static uint64_t ticks=1000,frequency=19200000;
static unsigned masks;
static uint64_t GbsHostTicks(void){return ticks++;}
static uint64_t GbsHostFrequency(void){return frequency;}
static void GbsHostMaskInterrupts(void){++masks;}
static EFI_STATUS locate(EFI_GUID *,VOID *,VOID **);
static EFI_TPL raise_tpl(EFI_TPL);
static VOID restore_tpl(EFI_TPL);
static struct {
    EFI_STATUS (*LocateProtocol)(EFI_GUID *,VOID *,VOID **);
    EFI_TPL (*RaiseTPL)(EFI_TPL);
    VOID (*RestoreTPL)(EFI_TPL);
} services={locate,raise_tpl,restore_tpl},*gBS=&services;
#include "GpuBootStateHandoff.h"
_Static_assert(sizeof(struct gbs_rail)==64,"rail ABI");
_Static_assert(sizeof(struct gbs_report)==256,"report ABI");
_Static_assert(offsetof(struct gbs_report,crc32)==24,"CRC offset");

static GBS_RPMH rpmh;
static GBS_COMMAND_DB db;
static GW_COMMAND commands[2];
static GW_VOTER voters[2];
static _Alignas(8) UINT8 unaligned_commands[2][sizeof(GW_COMMAND)+8];
static _Alignas(8) UINT8 unaligned_voters[2][sizeof(GW_VOTER)+8];
static unsigned scenario,calls,fail_at,post_ebs,cases;
static EFI_TPL current_tpl;
static EFI_TPL raise_tpl(EFI_TPL requested)
{assert(!post_ebs && !current_tpl && requested==TPL_NOTIFY);current_tpl=requested;return 0;}
static VOID restore_tpl(EFI_TPL previous)
{assert(!post_ebs && current_tpl==TPL_NOTIFY && !previous);current_tpl=previous;}
static EFI_STATUS call(void)
{
    assert(!post_ebs);++calls;
    return calls==fail_at?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static unsigned rail(const CHAR8 *name)
{
    assert(!strcmp(name,"cx.lvl") || !strcmp(name,"mx.lvl"));
    return name[0]=='m';
}
static EFI_STATUS query_address(GBS_COMMAND_DB *p,const CHAR8 *name,UINT32 *out)
{
    assert(p==&db);unsigned index=rail(name);EFI_STATUS status=call();
    *out=scenario==10?0x30000:0x30000+index*16;return status;
}
static EFI_STATUS query_slave(GBS_COMMAND_DB *p,const CHAR8 *name,UINT32 *out)
{assert(p==&db);(void)rail(name);*out=scenario==5?4:3;return call();}
static EFI_STATUS query_size(GBS_COMMAND_DB *p,const CHAR8 *name,UINT32 *out)
{assert(p==&db);(void)rail(name);*out=scenario==6?33:32;return call();}
static EFI_STATUS aux(GBS_COMMAND_DB *p,const CHAR8 *name,UINT8 *length,VOID *out,UINT32 *result)
{
    assert(p==&db && *length==32);(void)rail(name);
    static const UINT16 levels[16]={0,16,64,128,192,256,320,384,416,480};
    memcpy(out,levels,32);*result=scenario==7?1:0;
    if(scenario==8)*length=30;
    if(scenario==9)((UINT8 *)out)[8]=1; /* breaks ascending levels */
    return call();
}
static EFI_STATUS valid_drv(UINT32 drv,BOOLEAN *valid)
{assert(drv==2);*valid=scenario==4?FALSE:TRUE;return call();}
static EFI_STATUS find(UINT32 address,GW_COMMAND **out)
{
    unsigned index=address==0x30010;
    assert((address==0x30000 || address==0x30010) && current_tpl==TPL_NOTIFY);
    *out=scenario==11?NULL:&commands[index];
    if(scenario==19)*out=(GW_COMMAND *)(void *)(unaligned_commands[index]+4);
    return call();
}
static EFI_STATUS locate(EFI_GUID *guid,VOID *registration,VOID **out)
{
    assert(!registration);
    assert(guid->a==0xe1d0b573 || guid->a==0x1c34f691);
    if(guid->a==0xe1d0b573)*out=scenario==1?NULL:&rpmh;
    else *out=&db;
    return call();
}
static void word(UINT8 *p,UINT32 value)
{for(unsigned i=0;i<4;i++)p[i]=(UINT8)(value>>(i*8));}
static unsigned property(UINT8 *out,const char *key,const void *data,unsigned bytes)
{
    memset(out,0,36);memcpy(out,key,strlen(key));word(out+32,bytes);
    memcpy(out+36,data,bytes);unsigned end=36+((bytes+3)&~3u);
    memset(out+36+bytes,0,end-36-bytes);return end;
}
static UINT8 adt[1024];
static unsigned adt_bytes,payload_offset;
static void setup(unsigned which)
{
    scenario=which;calls=fail_at=post_ebs=masks=0;current_tpl=0;ticks=1000;frequency=19200000;
    rpmh=(GBS_RPMH){0x10001,{0}};db=(GBS_COMMAND_DB){0x10000,{0}};
    rpmh.Methods[1]=(VOID *)valid_drv;rpmh.Methods[9]=(VOID *)find;
    db.Methods[1]=(VOID *)query_address;db.Methods[5]=(VOID *)query_size;
    db.Methods[6]=(VOID *)aux;db.Methods[7]=(VOID *)query_slave;
    if(which==2)rpmh.Version++;
    if(which==3)db.Version++;
    if(which==20)rpmh.Methods[9]=NULL;
    for(unsigned i=0;i<2;i++) {
        memset(&voters[i],0,sizeof(voters[i]));
        voters[i].Driver=2;voters[i].Hardware.Valid=1;voters[i].Hardware.Data=3;
        voters[i].Local[0].Valid=1;voters[i].Local[0].Data=3;
        commands[i]=(GW_COMMAND){0x30000+i*16,&voters[i],NULL};
        GW_VOTER copy=voters[i];copy.Next=NULL;
        memcpy(unaligned_voters[i]+4,&copy,sizeof(copy));
        GW_COMMAND command={commands[i].Address,(GW_VOTER *)(void *)(unaligned_voters[i]+4),NULL};
        memcpy(unaligned_commands[i]+4,&command,sizeof(command));
    }
    struct gbs_report request;gbs_init(&request);assert(gbs_requested(&request));
    memset(adt,0,sizeof(adt));word(adt,2);word(adt+4,1);unsigned at=8;
    at+=property(adt+at,"name","device-tree",12);
    at+=property(adt+at,"physical-model","OnePlus Ace 5",14);
    word(adt+at,2);word(adt+at+4,0);at+=8;
    at+=property(adt+at,"name","chosen",7);
    payload_offset=at+36;at+=property(adt+at,GBS_PROPERTY,&request,sizeof(request));adt_bytes=at;
    assert(payload_offset%4==0);
}
static struct gbs_report decode(void)
{
    struct gbs_report r;assert(gbs_decode(adt+payload_offset,256,&r));return r;
}
static void finalize(void)
{
    unsigned before=calls;post_ebs=1;GiuliaGpuBootStateFinish();assert(before==calls && masks==1);
    struct gbs_report r=decode();assert(r.phase==GBS_EBS_RETURNED && !mGbsTree);
    GiuliaGpuBootStateFinish();assert(before==calls && masks==1);++cases;
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"--placeholder")) {
        struct gbs_report request;gbs_init(&request);FILE *out=fopen(argv[2],"wb");assert(out);
        assert(fwrite(&request,1,sizeof(request),out)==sizeof(request));assert(!fclose(out));return 0;
    }
    for(unsigned which=0;which<=21;which++) {
        setup(which);EFI_STATUS status=GiuliaGpuBootStatePrepare(adt,adt_bytes);
        struct gbs_report before=decode();assert(before.phase==GBS_CAPTURED && !before.flags);
        if(which==0 || (which>=11 && which<=19) || which==21) {
            assert(status==EFI_SUCCESS && before.code==GBS_OK && calls==13);
            voters[0].Hardware.Data=voters[0].Local[0].Data=5;
            if(which==21)memset(commands,0,sizeof(commands)); /* resource table moved */
            if(which==12)voters[0].State=1;
            if(which==13)voters[0].Local[0].Dirty=1;
            if(which==14)voters[0].Hardware.Data=4;
            if(which==15)voters[0].Hardware.Valid=0;
            if(which==16)voters[0].Local[0].Valid=2;
            if(which==17)voters[0].Local[0].Data=99;
            if(which==18){voters[0].Driver=7;voters[0].Next=&voters[0];}
            finalize();struct gbs_report after=decode();
            if(which>=12 && which<=18)assert(after.code==GBS_CACHE && after.status==EFI_NOT_READY);
            else {
                assert(after.code==GBS_OK && after.flags==GBS_CACHE_AFTER_EBS);
                if(which==11)assert(!after.rail[0].cached_valid && !after.rail[0].cached_selector);
                else assert(after.rail[0].cached_valid && after.rail[0].cached_selector==(which==19?3u:5u));
            }
        }else {assert(EFI_ERROR(status) && before.code!=GBS_OK);finalize();assert(decode().code!=GBS_OK);}
    }
    setup(0);assert(!GiuliaGpuBootStatePrepare(adt,adt_bytes));unsigned full_calls=calls;GiuliaGpuBootStateAbort();
    for(unsigned fail=1;fail<=full_calls;fail++) {
        setup(0);fail_at=fail;assert(GiuliaGpuBootStatePrepare(adt,adt_bytes)==EFI_DEVICE_ERROR);
        assert(decode().code!=GBS_OK);finalize();assert(decode().status==EFI_DEVICE_ERROR);
    }
    setup(0);adt[payload_offset+16]=1;
    assert(GiuliaGpuBootStatePrepare(adt,adt_bytes)==EFI_COMPROMISED_DATA && !calls);
    finalize();assert(decode().code==GBS_ARGUMENT);
    setup(0);assert(!GiuliaGpuBootStatePrepare(adt,adt_bytes));finalize();
    struct gbs_report valid=decode(),out;
    for(unsigned byte=0;byte<256;byte++) {
        struct gbs_report corrupt=valid;((UINT8 *)&corrupt)[byte]^=1;
        assert(!gbs_decode(&corrupt,256,&out));++cases;
    }
    setup(0);assert(!GiuliaGpuBootStatePrepare(adt,adt_bytes));GiuliaGpuBootStateAbort();
    unsigned before=calls;post_ebs=1;GiuliaGpuBootStateFinish();assert(calls==before);
    assert(decode().phase==GBS_CAPTURED);++cases;
    printf("GPU boot state: %u RAM-only cases passed; post-EBS firmware calls=0\n",cases);
    return 0;
}
