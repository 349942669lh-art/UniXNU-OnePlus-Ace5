/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_TERMINAL_H
#define GIULIA_GPU_TERMINAL_H
/* The USB command path only touches this static mailbox, never an IOService
 * pointer or an MMIO mapping. Repeated pending requests coalesce. Closing
 * excludes new requests until the old owner has joined its retained worker. */
enum { GGT_STATUS=1, GGT_OBSERVE=2, GGT_ACTIVE=4, GGT_CLOSING=8 };
enum { GGT_HFI_PIN=16, GGT_HFI_STATUS=32, GGT_HFI_QUERY=64,
    GGT_HFI_POLL=128, GGT_HFI_LOG=256,
    GGT_GMU_LOAD=512, GGT_GMU_START=1024, GGT_GMU_STATUS=2048,
    GGT_STACK=4096, GGT_SHARE=8192,
    GGT_GMU_COMMANDS=GGT_GMU_LOAD|GGT_GMU_START|GGT_GMU_STATUS,
    GGT_COMMANDS=GGT_STATUS|GGT_OBSERVE|GGT_HFI_PIN|GGT_HFI_STATUS|GGT_HFI_QUERY|GGT_HFI_POLL|GGT_HFI_LOG|GGT_GMU_COMMANDS|GGT_STACK|GGT_SHARE };
enum { GGT_UNAVAILABLE, GGT_PREPARING, GGT_READY, GGT_STOPPING, GGT_STOPPED, GGT_FAILED };
struct gg_terminal_mailbox { unsigned requests, stage, mode; };
/* A qualification boot must not silently combine host-only and MMIO modes. */
static inline int gg_terminal_hostonly_valid(int mode,int isolation)
{
    return mode>=1 && mode<=3 && (isolation==0 || (isolation==1 && mode==1));
}
static inline int gg_terminal_equal(const char *a,const char *b)
{
    if(!a)return 0;
    while(*a && *a==*b){++a;++b;}
    return *a==*b;
}
static inline unsigned gg_terminal_command(const char *command)
{
    return gg_terminal_equal(command,"gpu.status")?GGT_STATUS:
        gg_terminal_equal(command,"gpu.observe")?GGT_OBSERVE:
        gg_terminal_equal(command,"gpu.hfi.pin")?GGT_HFI_PIN:
        gg_terminal_equal(command,"gpu.hfi.status")?GGT_HFI_STATUS:
        gg_terminal_equal(command,"gpu.hfi.query")?GGT_HFI_QUERY:
        gg_terminal_equal(command,"gpu.hfi.poll")?GGT_HFI_POLL:
        gg_terminal_equal(command,"gpu.hfi.log")?GGT_HFI_LOG:
        gg_terminal_equal(command,"gpu.gmu.load")?GGT_GMU_LOAD:
        gg_terminal_equal(command,"gpu.gmu.start")?GGT_GMU_START:
        gg_terminal_equal(command,"gpu.gmu.status")?GGT_GMU_STATUS:
        gg_terminal_equal(command,"gpu.stack")?GGT_STACK:
        gg_terminal_equal(command,"gpu.share")?GGT_SHARE:0;
}
static inline int gg_terminal_open(struct gg_terminal_mailbox *m)
{
    unsigned expected=0;
    return __atomic_compare_exchange_n(&m->requests,&expected,GGT_ACTIVE,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static inline int gg_terminal_request(struct gg_terminal_mailbox *m,unsigned request)
{
    unsigned expected=__atomic_load_n(&m->requests,__ATOMIC_ACQUIRE);
    if(!request || (request&(request-1)) || (request&~GGT_COMMANDS))return 0;
    do {
        if(!(expected&GGT_ACTIVE))return 0;
    } while(!__atomic_compare_exchange_n(&m->requests,&expected,expected|request,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    return 1;
}
static inline unsigned gg_terminal_take(struct gg_terminal_mailbox *m)
{
    return __atomic_fetch_and(&m->requests,~GGT_COMMANDS,__ATOMIC_ACQ_REL)&GGT_COMMANDS;
}
static inline unsigned gg_terminal_close(struct gg_terminal_mailbox *m)
{
    return __atomic_exchange_n(&m->requests,GGT_CLOSING,__ATOMIC_ACQ_REL)&GGT_COMMANDS;
}
static inline void gg_terminal_finish(struct gg_terminal_mailbox *m)
{
    __atomic_store_n(&m->requests,0u,__ATOMIC_RELEASE);
}
static inline const char *gg_terminal_stage_name(unsigned stage)
{
    const char *const names[]={"unavailable","preparing","host-memory-prepared","stopping","stopped","failed"};
    return stage<sizeof(names)/sizeof(names[0])?names[stage]:"invalid";
}
#endif
