/* Frozen-oracle differential test, not a panel FPS or WC bandwidth test. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fixtures/giulia_display_scalar.h"
#include "../ios16/display/giulia_display.h"

static uint32_t seed=0xace516U;
static uint32_t random32(void) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static double seconds(void) {
    struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return t.tv_sec+t.tv_nsec/1e9;
}
int main(int argc,char **argv) {
    const size_t bytes=(size_t)GD_STRIDE*GD_HEIGHT, guard=64;
    const unsigned width=GD_CANVAS_W,height=GD_CANVAS_H,stride=(width+7)*4;
    const size_t offset=32,source_bytes=offset+(size_t)stride*height;
    uint8_t *a=malloc(bytes+guard*2+8),*b=malloc(bytes+guard*2+8);
    uint8_t *s=malloc(source_bytes),*saved=malloc(source_bytes);
    assert(a && b && s && saved);
    for(size_t i=0;i<source_bytes;i++)s[i]=(uint8_t)random32();
    memcpy(saved,s,source_bytes);
    struct gd_rect full={0,0,width,height};
    if(argc==2 && strcmp(argv[1],"--benchmark")==0) {
        /* Host RAM timing only; cannot predict phone WC performance. */
        for(unsigned mode=0;mode<2;mode++) {
            double start=seconds();
            for(unsigned i=0;i<48;i++) {
                s[offset+i]^=1;
                assert(gd_present_mode(a+guard,bytes,s,source_bytes,offset,width,height,stride,full,full,mode));
            }
            printf("host_ram_only mode=%u ms_per_present=%.6f checksum=%u\n",
                   mode,(seconds()-start)*1000/48,a[guard+124]);
        }
    } else {
        unsigned cases=0;
        for(unsigned i=0;i<144;i++) {
            struct gd_rect src=full,dst=full;
            if(i>=4) {
                src.x=random32()%width;src.y=random32()%height;
                src.w=1+random32()%(width-src.x);src.h=1+random32()%(height-src.y);
                dst.x=(int)(random32()%3000)-1200;dst.y=(int)(random32()%5000)-2000;
                dst.w=1+random32()%4500;dst.h=1+random32()%6500;
            }
            if(i==1)dst=(struct gd_rect){-1,-1,1125,2436};
            if(i==2)dst=(struct gd_rect){1,1,1123,2434};
            if(i==3)dst=(struct gd_rect){1123,2434,2,2};
            struct gd_reference_rect rs={src.x,src.y,src.w,src.h},rd={dst.x,dst.y,dst.w,dst.h};
            for(unsigned align=0;align<=4;align+=4) {
                for(unsigned mode=0;mode<3;mode++) {
                    memset(a,0xa5,bytes+guard*2+8);memset(b,0xa5,bytes+guard*2+8);
                    int r=gd_reference_present(a+guard+align,bytes,s,source_bytes,offset,width,height,stride,rs,rd);
                    int q=gd_present_mode(b+guard+align,bytes,s,source_bytes,offset,width,height,stride,src,dst,mode);
                    assert(r==q && memcmp(a,b,bytes+guard*2+8)==0);
                    cases++;
                }
            }
        }
        /* Invalid byte sizes, alignment, stride and rectangle overflow must
         * preserve the entire destination, including all guard bytes. */
        for(unsigned i=0;i<10;i++) {
            size_t pb=bytes,sb=source_bytes,off=offset;unsigned w=width,h=height,st=stride;
            struct gd_rect src=full,dst=full;unsigned pa=0,sa=0;
            if(i==0)pb--;
            if(i==1)sb=offset+(size_t)(height-1)*stride+width*4-1;
            if(i==2)off=source_bytes+4;
            if(i==3)st=width*4-4;
            if(i==4)src.x=INT32_MAX;
            if(i==5)dst.w=0;
            if(i==6)w=0;
            if(i==7)h=16385;
            if(i==8)pa=1;
            if(i==9)sa=1;
            struct gd_reference_rect rs={src.x,src.y,src.w,src.h},rd={dst.x,dst.y,dst.w,dst.h};
            memset(a,0xa5,bytes+guard*2+8);memset(b,0xa5,bytes+guard*2+8);
            int r=gd_reference_present(a+guard+pa,pb,s+sa,sb,off,w,h,st,rs,rd);
            int q=gd_present_mode(b+guard+pa,pb,s+sa,sb,off,w,h,st,src,dst,1);
            assert(r==0 && q==0 && memcmp(a,b,bytes+guard*2+8)==0);cases++;
        }
        assert(memcmp(s,saved,source_bytes)==0);
        printf("copy64: %u frozen-oracle cases passed; guard bytes and source unchanged\n",cases);
    }
    free(a);free(b);free(s);free(saved);return 0;
}
