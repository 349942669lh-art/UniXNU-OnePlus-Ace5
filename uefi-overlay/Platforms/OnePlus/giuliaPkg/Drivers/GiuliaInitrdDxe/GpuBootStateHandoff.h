/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pinned candidate RPMh/CmdDb observation, followed by a CPU-only copy after
 * successful ExitBootServices. No GPU/RSC MMIO or voltage request occurs.
 */
#ifndef GIULIA_GPU_BOOT_STATE_HANDOFF_H
#define GIULIA_GPU_BOOT_STATE_HANDOFF_H
#define GBS_FIRMWARE_TYPES 1
#include "GpuBootState.h"
#include "GpuPowerAdt.h"
#include "WifiPowerCache.h"

typedef struct { UINT64 Version; VOID *Methods[24]; } GBS_RPMH;
typedef struct { UINT64 Version; VOID *Methods[8]; } GBS_COMMAND_DB;
typedef EFI_STATUS (EFIAPI *GBS_QUERY)(GBS_COMMAND_DB *,CONST CHAR8 *,UINT32 *);
typedef EFI_STATUS (EFIAPI *GBS_AUX)(GBS_COMMAND_DB *,CONST CHAR8 *,UINT8 *,VOID *,UINT32 *);
typedef EFI_STATUS (EFIAPI *GBS_VALID_DRV)(UINT32,BOOLEAN *);
typedef EFI_STATUS (EFIAPI *GBS_FIND)(UINT32,GW_COMMAND **);

STATIC struct gbs_report mGbsReport;
STATIC UINT8 *mGbsTree;
STATIC UINTN mGbsOffset;
STATIC GW_COMMAND mGbsCommands[2];
STATIC BOOLEAN mGbsCommandPresent[2];

STATIC UINT64 GbsTicks(VOID)
{
#if defined(GBS_HOST_TEST)
  return GbsHostTicks();
#else
  UINT64 Ticks;
  __asm__ volatile("mrs %0,cntpct_el0":"=r"(Ticks));
  return Ticks;
#endif
}
STATIC UINT64 GbsFrequency(VOID)
{
#if defined(GBS_HOST_TEST)
  return GbsHostFrequency();
#else
  UINT64 Frequency;
  __asm__ volatile("mrs %0,cntfrq_el0":"=r"(Frequency));
  return Frequency;
#endif
}
STATIC VOID GbsMaskInterrupts(VOID)
{
#if defined(GBS_HOST_TEST)
  GbsHostMaskInterrupts();
#else
  __asm__ volatile("msr daifset, #15; dsb sy; isb":::"memory");
#endif
}

STATIC EFI_STATUS GbsCaptureRail(GBS_COMMAND_DB *Db,GBS_RPMH *Rpmh,unsigned Index)
{
  STATIC CONST CHAR8 Names[2][8]={"cx.lvl","mx.lvl"};
  if(Index>=2)return EFI_INVALID_PARAMETER;
  struct gbs_rail *Rail=&mGbsReport.rail[Index];
  UINT8 Aux[32],Length=0;
  UINT32 Size=0,Result=0xffffffffu;
  gbs_copy(Rail->name,Names[Index],sizeof(Rail->name));
  EFI_STATUS Status=((GBS_QUERY)Db->Methods[1])(Db,Names[Index],&Rail->address);
  if(EFI_ERROR(Status))return Status;
  Status=((GBS_QUERY)Db->Methods[7])(Db,Names[Index],&Rail->slave);
  if(EFI_ERROR(Status))return Status;
  Status=((GBS_QUERY)Db->Methods[5])(Db,Names[Index],&Size);
  if(EFI_ERROR(Status))return Status;
  if(!Size || Size>sizeof(Aux) || (Size&1))return EFI_COMPROMISED_DATA;
  Length=(UINT8)Size;gbs_zero(Aux,sizeof(Aux));
  Status=((GBS_AUX)Db->Methods[6])(Db,Names[Index],&Length,Aux,&Result);
  if(EFI_ERROR(Status))return Status;
  if(Result || Length!=Size)return EFI_COMPROMISED_DATA;
  unsigned Padding=0;
  for(unsigned I=0;I<Size/2;I++) {
    UINT16 Level=(UINT16)(Aux[I*2]|((UINT16)Aux[I*2+1]<<8));
    if(I && !Level)Padding=1;
    else {
      if(Padding)return EFI_COMPROMISED_DATA;
      Rail->levels[Rail->level_count++]=Level;
    }
  }
  if(!gbs_rail_valid(Rail,Index))return EFI_COMPROMISED_DATA;
  /* The resource table can realloc/move. Only voter nodes have independent
   * allocations retained through EBS. Copy the command while notification
   * callbacks cannot grow the resource table between lookup and copy. */
  GW_COMMAND *Pointer=NULL;
  EFI_TPL Previous=gBS->RaiseTPL(TPL_NOTIFY);
  Status=((GBS_FIND)Rpmh->Methods[9])(Rail->address,&Pointer);
  if(!EFI_ERROR(Status) && Pointer) {
    if((UINTN)Pointer&3)Status=EFI_COMPROMISED_DATA;
    else {
      gw_cache_copy(&mGbsCommands[Index],Pointer,sizeof(GW_COMMAND));
      if(mGbsCommands[Index].Address!=Rail->address)Status=EFI_COMPROMISED_DATA;
      else mGbsCommandPresent[Index]=TRUE;
    }
  }
  gBS->RestoreTPL(Previous);
  if(EFI_ERROR(Status))return Status;
  ++mGbsReport.rail_count;
  return EFI_SUCCESS;
}

/* gw_cached_vote handles the pinned provider's four-byte-aligned objects.
 * Require the completed hardware cache and the active aggregate to agree.
 * The caller invokes this only immediately after EBS, before kernel entry or
 * memory reclamation. Candidate rpmh_exit leaves these records allocated. */
STATIC int GbsCompletedCache(CONST VOID *Pointer,unsigned Address,unsigned *Data,int *Valid)
{
  int Code=gw_cached_vote(Pointer,Address,Data,Valid);
  if(Code || !*Valid)return Code;
  GW_COMMAND Command;gw_cache_copy(&Command,Pointer,sizeof(Command));
  CONST VOID *Next=Command.Voters;
  for(unsigned Count=0;Next && Count<32;Count++) {
    GW_VOTER Voter;
    if((UINTN)Next&3)return GW_CACHE_BAD;
    gw_cache_copy(&Voter,Next,sizeof(Voter));
    if(Voter.Driver==2) {
      if(Voter.State || Voter.Local[0].Dirty || Voter.Hardware.Valid!=1 ||
         Voter.Hardware.Data!=*Data)return GW_CACHE_BUSY;
      return GW_CACHE_OK;
    }
    Next=Voter.Next;
  }
  return GW_CACHE_BAD;
}

STATIC VOID GiuliaGpuBootStateAbort(VOID)
{
  mGbsTree=NULL;mGbsOffset=0;
  gbs_zero(mGbsCommands,sizeof(mGbsCommands));
  mGbsCommandPresent[0]=mGbsCommandPresent[1]=FALSE;
}
STATIC EFI_STATUS GiuliaGpuBootStatePrepare(VOID *Tree,UINTN Bytes)
{
  STATIC EFI_GUID RpmhGuid={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
  STATIC EFI_GUID DbGuid={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
  GBS_RPMH *Rpmh=NULL;
  GBS_COMMAND_DB *Db=NULL;
  BOOLEAN Valid=FALSE;
  UINTN Offset=0;
  EFI_STATUS Status=EFI_COMPROMISED_DATA;
  GiuliaGpuBootStateAbort();
  int Found=gpa_adt_find_named(Tree,Bytes,GBS_BYTES,GBS_PROPERTY,&Offset);
  if(Found!=GPA_ADT_PRESENT)return Found==GPA_ADT_ABSENT?EFI_NOT_FOUND:EFI_COMPROMISED_DATA;
  mGbsTree=(UINT8 *)Tree;mGbsOffset=Offset;
  gbs_copy(&mGbsReport,mGbsTree+Offset,sizeof(mGbsReport));
  if(!gbs_requested(&mGbsReport)) {
    gbs_init(&mGbsReport);mGbsReport.code=GBS_ARGUMENT;goto Done;
  }
  mGbsReport.capture_ticks=GbsTicks();mGbsReport.counter_frequency=GbsFrequency();
  mGbsReport.drv_id=2;mGbsReport.code=GBS_PROTOCOL;
  if(!mGbsReport.counter_frequency || mGbsReport.counter_frequency>1000000000u)goto Done;
  Status=gBS->LocateProtocol(&RpmhGuid,NULL,(VOID **)&Rpmh);
  if(EFI_ERROR(Status))goto Done;
  Status=gBS->LocateProtocol(&DbGuid,NULL,(VOID **)&Db);
  if(EFI_ERROR(Status))goto Done;
  Status=EFI_UNSUPPORTED;
  if(!Rpmh || !Db || Rpmh->Version!=0x10001 || Db->Version!=0x10000 ||
     !Rpmh->Methods[1] || !Rpmh->Methods[9] || !Db->Methods[1] ||
     !Db->Methods[5] || !Db->Methods[6] || !Db->Methods[7])goto Done;
  mGbsReport.rpmh_version=Rpmh->Version;mGbsReport.command_db_version=Db->Version;
  Status=((GBS_VALID_DRV)Rpmh->Methods[1])(2,&Valid);
  if(EFI_ERROR(Status))goto Done;
  if(!Valid){Status=EFI_UNSUPPORTED;goto Done;}
  mGbsReport.code=GBS_COMMAND_DB_ERROR;
  for(unsigned I=0;I<2;I++) {
    Status=GbsCaptureRail(Db,Rpmh,I);
    if(EFI_ERROR(Status))goto Done;
  }
  if(mGbsReport.rail[0].address==mGbsReport.rail[1].address){Status=EFI_COMPROMISED_DATA;goto Done;}
  mGbsReport.code=GBS_OK;Status=EFI_SUCCESS;
Done:
  mGbsReport.phase=GBS_CAPTURED;mGbsReport.status=Status;gbs_seal(&mGbsReport);
  gbs_copy(mGbsTree+mGbsOffset,&mGbsReport,sizeof(mGbsReport));
  DEBUG((DEBUG_ERROR,"GIULIA GPU BOOT: pre-EBS code=%u rails=%u status=%lx capture-only=1\n",
    mGbsReport.code,mGbsReport.rail_count,(UINT64)Status));
  return Status;
}

/* Called only on the successful EBS return path. No Boot Services, protocol
 * invocation, allocation, logging or MMIO is available in this function. */
STATIC VOID GiuliaGpuBootStateFinish(VOID)
{
  if(!mGbsTree)return;
  /* DarwinEnter masks DAIF on entry as well. Move that kernel-entry action
   * ahead of the snapshot so firmware IRQ handlers cannot mutate its cache. */
  GbsMaskInterrupts();
  mGbsReport.phase=GBS_EBS_RETURNED;mGbsReport.ebs_ticks=GbsTicks();
  if(mGbsReport.code==GBS_OK) {
    for(unsigned I=0;I<2;I++) {
      unsigned Data=0;int Valid=0;
      CONST VOID *Command=mGbsCommandPresent[I]?&mGbsCommands[I]:NULL;
      int Code=GbsCompletedCache(Command,mGbsReport.rail[I].address,&Data,&Valid);
      mGbsReport.rail[I].cache_code=(UINT32)Code;
      mGbsReport.rail[I].cached_selector=Data;mGbsReport.rail[I].cached_valid=(UINT32)Valid;
      if(Code || !gbs_rail_valid(&mGbsReport.rail[I],I)) {
        mGbsReport.code=GBS_CACHE;mGbsReport.status=EFI_NOT_READY;
      }
    }
    if(mGbsReport.code==GBS_OK)mGbsReport.flags|=GBS_CACHE_AFTER_EBS;
  }
  gbs_seal(&mGbsReport);
  gbs_copy(mGbsTree+mGbsOffset,&mGbsReport,sizeof(mGbsReport));
  GiuliaGpuBootStateAbort();
}
#endif
