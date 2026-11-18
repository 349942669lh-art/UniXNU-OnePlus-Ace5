/* Real branch-vote core, RAM-only register/provider doubles, NOT hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_parents.h"
struct fixture {
    struct ggcp_state s;
    uint32_t reg,change_quiet;
    unsigned events,lose_at,reads,writes,waits,quiets,live_calls;
    int valid,read_error,write_error,wait_error,quiet,ignore_write,corrupt;
};
static void event(struct fixture *f){if(++f->events==f->lose_at)f->valid=0;}
static int live(void *p){struct fixture *f=p;++f->live_calls;event(f);return f->valid;}
static int read32(void *p,uint32_t *v){struct fixture *f=p;assert(f->valid);++f->reads;*v=f->reg;event(f);return f->read_error;}
static int write32(void *p,uint32_t v){
    struct fixture *f=p;assert(f->valid);++f->writes;
    assert(f->s.write_attempted && (f->s.refs[0] || f->s.refs[1]));
    int ticket=0;for(unsigned i=0;i<GGCP_SLOTS;i++)ticket|=!!f->s.tickets[i].serial;
    assert(ticket);if(!f->ignore_write)f->reg=v^(f->corrupt?1u:0);event(f);return f->write_error;
}
static int settle(void *p,unsigned us){struct fixture *f=p;assert(f->valid && us==10);++f->waits;event(f);return f->wait_error;}
static int quiet(void *p){struct fixture *f=p;assert(f->valid);++f->quiets;f->reg^=f->change_quiet;event(f);return f->quiet;}
static struct ggcp_io io(struct fixture *f){struct ggcp_io out={f,live,read32,write32,settle,quiet};return out;}
static void reset(struct fixture *f,uint32_t value){memset(f,0,sizeof(*f));f->valid=1;f->quiet=1;f->reg=value;}
static void init(struct fixture *f,uint32_t value){reset(f,value);struct ggcp_io o=io(f);assert(ggcp_init(&f->s,&o)==GGCP_OK);o.read32=NULL;}
static unsigned cases;
int main(void){
    struct fixture f;uint64_t a=0,b=0,c=0;struct ggcp_io o;
    reset(&f,0);o=io(&f);
    assert(ggcp_init(NULL,&o)==GGCP_ARGUMENT && ggcp_init(&f.s,NULL)==GGCP_ARGUMENT);
    for(unsigned i=0;i<5;i++){
        reset(&f,0);o=io(&f);switch(i){case 0:o.live=NULL;break;case 1:o.read32=NULL;break;
            case 2:o.write32=NULL;break;case 3:o.settle=NULL;break;default:o.quiescent=NULL;}
        assert(ggcp_init(&f.s,&o)==GGCP_ARGUMENT && !f.events && !f.s.attempted);++cases;
    }
    for(unsigned bits=1;bits<4;bits++){
        reset(&f,bits<<15);o=io(&f);assert(ggcp_init(&f.s,&o)==GGCP_INHERITED);
        assert(!f.writes && !f.s.ready && !f.s.quarantined && f.reg==(bits<<15));
        assert(ggcp_init(&f.s,&o)==GGCP_USED && ggcp_acquire(&f.s,0,&a)==GGCP_USED);
        assert(ggcp_close(&f.s)==GGCP_OK && !f.writes);++cases;
    }
    uint32_t rng=0x94ea5371;
    for(unsigned i=0;i<1024;i++){
        rng=rng*1664525u+1013904223u;
        uint32_t original=(rng&~GGCP_MASK)&~1u; /* Avoid all-ones bus failure sentinel. */
        init(&f,original);
        assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK && a && f.reg==(original|(1u<<15)));
        assert(ggcp_acquire(&f.s,0,&b)==GGCP_OK && b!=a && f.writes==1);
        f.reg^=2;original^=2; /* Another native owner between serialized transactions. */
        assert(ggcp_acquire(&f.s,1,&c)==GGCP_OK && f.reg==(original|GGCP_MASK));
        assert(ggcp_close(&f.s)==GGCP_BUSY);
        assert(ggcp_release(&f.s,b)==GGCP_OK && f.writes==2 && !f.quiets);
        assert(ggcp_release(&f.s,b)==GGCP_ARGUMENT);
        f.change_quiet=4;
        assert(ggcp_release(&f.s,a)==GGCP_OK && f.reg==((original^4)|(1u<<16)));
        assert(ggcp_release(&f.s,c)==GGCP_OK && f.reg==original);
        assert(f.waits==4 && f.writes==4 && f.quiets==2);
        assert(ggcp_close(&f.s)==GGCP_OK && !f.s.ready && f.s.closed);
        assert(ggcp_close(&f.s)==GGCP_USED && ggcp_acquire(&f.s,0,&a)==GGCP_USED);++cases;
    }
    init(&f,0);uint64_t tickets[GGCP_SLOTS];
    for(unsigned i=0;i<GGCP_SLOTS;i++)assert(ggcp_acquire(&f.s,i%2,&tickets[i])==GGCP_OK);
    assert(ggcp_acquire(&f.s,0,&a)==GGCP_FULL && !a && f.writes==2);
    assert(ggcp_release(&f.s,tickets[0])==GGCP_OK);
    assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK && a>tickets[15]);
    assert(ggcp_release(&f.s,tickets[0])==GGCP_ARGUMENT);
    for(unsigned i=1;i<GGCP_SLOTS;i++)assert(ggcp_release(&f.s,tickets[i])==GGCP_OK);
    assert(ggcp_release(&f.s,a)==GGCP_OK && ggcp_close(&f.s)==GGCP_OK);++cases;
    init(&f,0);f.s.serial=UINT64_MAX;
    assert(ggcp_acquire(&f.s,0,&a)==GGCP_FULL && !a && !f.writes);++cases;
    init(&f,0);assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK);f.quiet=0;
    assert(ggcp_release(&f.s,a)==GGCP_BUSY && f.s.refs[0]==1 && f.writes==1 && !f.s.quarantined);
    f.quiet=1;assert(ggcp_release(&f.s,a)==GGCP_OK && f.writes==2);++cases;
    for(unsigned fault=0;fault<10;fault++){
        init(&f,0);
        if(fault==0)f.read_error=1;
        if(fault==1)f.reg=UINT32_MAX;
        if(fault==2)f.write_error=1;
        if(fault==3)f.wait_error=1;
        if(fault==4)f.ignore_write=1;
        if(fault==5)f.corrupt=1;
        if(fault>=6){assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK);
            if(fault==6)f.quiet=-1;
            if(fault==7)f.reg=0; /* Managed bit stolen. */
            if(fault==8)f.change_quiet=1u<<16; /* Quiet callback changed managed bits. */
            if(fault==9)f.write_error=1;
            assert(ggcp_release(&f.s,a)!=GGCP_OK && f.s.refs[0]==1);
        }else{
            assert(ggcp_acquire(&f.s,0,&a)!=GGCP_OK);
            if(fault>=2)assert(a && f.s.refs[0]==1);else assert(!a);
        }
        assert(f.s.quarantined && !f.s.ready);unsigned events=f.events;
        assert(ggcp_acquire(&f.s,0,&b)==GGCP_QUARANTINED && !b);
        assert(ggcp_release(&f.s,a?a:1)==GGCP_QUARANTINED);
        assert(ggcp_close(&f.s)==GGCP_QUARANTINED && f.events==events);++cases;
    }
    /* Every callback boundary in init, acquire, and final release loses authority. */
    for(unsigned operation=0;operation<3;operation++){
        reset(&f,0);o=io(&f);
        if(operation)assert(ggcp_init(&f.s,&o)==GGCP_OK);
        if(operation==2)assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK);
        unsigned start=f.events;
        if(!operation)assert(ggcp_init(&f.s,&o)==GGCP_OK);
        else if(operation==1)assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK);
        else assert(ggcp_release(&f.s,a)==GGCP_OK);
        unsigned total=f.events-start;
        for(unsigned boundary=1;boundary<=total;boundary++){
            reset(&f,0);o=io(&f);
            if(operation)assert(ggcp_init(&f.s,&o)==GGCP_OK);
            if(operation==2)assert(ggcp_acquire(&f.s,0,&a)==GGCP_OK);
            f.lose_at=f.events+boundary;
            int result=!operation?ggcp_init(&f.s,&o):operation==1?ggcp_acquire(&f.s,0,&a):ggcp_release(&f.s,a);
            assert(result==GGCP_LOST && f.s.quarantined && !f.valid);
            if(operation==2)assert(f.s.refs[0]==1);
            unsigned events=f.events;
            assert(ggcp_close(&f.s)==GGCP_QUARANTINED && f.events==events);++cases;
        }
    }
    init(&f,0);assert(ggcp_acquire(NULL,0,&a)==GGCP_ARGUMENT && !a);
    assert(ggcp_acquire(&f.s,2,&a)==GGCP_ARGUMENT && !a);
    assert(ggcp_acquire(&f.s,0,NULL)==GGCP_ARGUMENT);
    assert(ggcp_release(NULL,1)==GGCP_ARGUMENT && ggcp_release(&f.s,0)==GGCP_ARGUMENT);
    assert(ggcp_release(&f.s,999)==GGCP_ARGUMENT && ggcp_close(NULL)==GGCP_ARGUMENT);
    ggcp_quarantine(NULL);ggcp_quarantine(&f.s);assert(ggcp_close(&f.s)==GGCP_QUARANTINED);++cases;
    printf("GCC GPU parents core: %u RAM-only cases passed; hardware_verified=0\n",cases);
    return 0;
}
