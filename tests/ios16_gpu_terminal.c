#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_terminal.h"

struct race {
    struct gg_terminal_mailbox mailbox;
    unsigned go;
    int accepted;
};
static void *requester(void *cookie)
{
    struct race *r=(struct race *)cookie;
    while(!__atomic_load_n(&r->go,__ATOMIC_ACQUIRE)) {}
    r->accepted=gg_terminal_request(&r->mailbox,GGT_OBSERVE);
    return NULL;
}
int main(void)
{
    for(int mode=-2;mode<=5;++mode)for(int isolation=-2;isolation<=3;++isolation) {
        int expected=(mode==1 || mode==2 || mode==3) &&
            (isolation==0 || (mode==1 && isolation==1));
        assert(gg_terminal_hostonly_valid(mode,isolation)==expected);
    }
    assert(!gg_terminal_hostonly_valid(1,(-2147483647-1)));
    assert(!gg_terminal_hostonly_valid(1,2147483647));
    puts("GPU host-only boot configuration: mode/isolation matrix and extreme values passed");
    struct gg_terminal_mailbox m={0};
    const char *bad[]={"gpu.status ","gpu.observe now","gpu.status\nping","gpu.observe;ping","gpu","gpu.start","GPU.status","",NULL};
    unsigned i;
    assert(gg_terminal_command("gpu.status")==GGT_STATUS);
    assert(gg_terminal_command("gpu.observe")==GGT_OBSERVE);
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!gg_terminal_command(bad[i]));
    assert(!gg_terminal_request(&m,GGT_STATUS));
    assert(gg_terminal_open(&m));assert(!gg_terminal_open(&m));
    assert(!gg_terminal_request(&m,GGT_STATUS|GGT_OBSERVE));
    assert(!gg_terminal_request(&m,GGT_ACTIVE));
    for(i=0;i<1000;++i)assert(gg_terminal_request(&m,GGT_STATUS));
    assert(gg_terminal_request(&m,GGT_OBSERVE));
    assert(gg_terminal_take(&m)==(GGT_STATUS|GGT_OBSERVE));
    assert(!gg_terminal_take(&m));
    assert(gg_terminal_request(&m,GGT_STATUS));
    assert(gg_terminal_close(&m)==GGT_STATUS);
    assert(!gg_terminal_request(&m,GGT_OBSERVE));
    assert(!gg_terminal_open(&m)); /* Joining owner still excludes a new one. */
    gg_terminal_finish(&m);assert(gg_terminal_open(&m));
    assert(!gg_terminal_close(&m));gg_terminal_finish(&m);
    for(i=0;i<2000;++i) {
        struct race r={{0},0,0};pthread_t thread;
        assert(gg_terminal_open(&r.mailbox));
        assert(!pthread_create(&thread,NULL,requester,&r));
        __atomic_store_n(&r.go,1u,__ATOMIC_RELEASE);
        unsigned cancelled=gg_terminal_close(&r.mailbox);
        assert(!pthread_join(thread,NULL));
        /* Every accepted request survives in the closed owner's cancellation
         * batch; a request racing after close cannot enter a later owner. */
        assert(!!(cancelled&GGT_OBSERVE)==r.accepted);
        assert(!gg_terminal_request(&r.mailbox,GGT_STATUS));
        assert(!gg_terminal_open(&r.mailbox));
        gg_terminal_finish(&r.mailbox);assert(gg_terminal_open(&r.mailbox));
        assert(!gg_terminal_take(&r.mailbox));
        gg_terminal_close(&r.mailbox);gg_terminal_finish(&r.mailbox);
    }
    assert(!strcmp(gg_terminal_stage_name(GGT_READY),"host-memory-prepared"));
    assert(!strcmp(gg_terminal_stage_name(GGT_FAILED),"failed"));
    assert(!strcmp(gg_terminal_stage_name(100),"invalid"));
    puts("GPU terminal exact commands, coalescing, ownership epochs and 2000 close/request races passed");
    return 0;
}
