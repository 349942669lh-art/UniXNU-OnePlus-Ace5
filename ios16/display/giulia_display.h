/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 native panel presentation. UIKit keeps its supported D22 layout; every
 * logical pixel rectangle is mapped over the complete 1264x2780 panel. The
 * inverse HID transform is normalized over that same complete panel.
 */
#ifndef GIULIA_DISPLAY_H
#define GIULIA_DISPLAY_H
#include <stdint.h>
#include <stddef.h>
#define GD_WIDTH 1264u
#define GD_HEIGHT 2780u
#define GD_STRIDE 5056u
#define GD_CANVAS_W 1125u
#define GD_CANVAS_H 2436u
struct gd_rect { int32_t x,y,w,h; };
static inline unsigned gd_touch(int coordinate,unsigned extent) {
    if(coordinate<=0 || extent<2)return 0;
    if((unsigned)coordinate>=extent-1)return 65535;
    return (unsigned)((uint64_t)(unsigned)coordinate*65535/(extent-1));
}
static inline int64_t gd_ceil(int64_t value,unsigned mul,unsigned div) {
    return (value*mul+div-1)/div; /* called only with nonnegative values */
}
/* BGRA/32-bit pixels, no rotation. Bounds are checked before the first write.
 * A source rectangle may be scaled independently of the display transform.
 * Negative destination origins are clipped without shifting source content.
 * Out-of-range source rectangles are rejected rather than reading padding.
 */
static inline int gd_present_mode(void *panel,size_t panel_bytes,const void *source,
    size_t source_bytes,size_t offset,unsigned width,unsigned height,unsigned stride,
    struct gd_rect src,struct gd_rect dst,unsigned packed64) {
    if(!panel || !source || ((uintptr_t)panel&3) || ((uintptr_t)source&3) || (offset&3) || (stride&3) ||
        panel_bytes<(size_t)GD_STRIDE*GD_HEIGHT || !width || !height || width>16384 || height>16384 ||
        stride<(uint64_t)width*4 || src.x<0 || src.y<0 || src.w<=0 || src.h<=0 || dst.w<=0 || dst.h<=0 ||
        (uint64_t)src.x+src.w>width || (uint64_t)src.y+src.h>height || offset>source_bytes)return 0;
    uint64_t last=(uint64_t)(src.y+src.h-1)*stride+(uint64_t)(src.x+src.w)*4;
    if(last>source_bytes-offset)return 0;
    int64_t left=dst.x<0?0:dst.x,top=dst.y<0?0:dst.y;
    int64_t right=(int64_t)dst.x+dst.w,bottom=(int64_t)dst.y+dst.h;
    if(right>GD_CANVAS_W)right=GD_CANVAS_W;
    if(bottom>GD_CANVAS_H)bottom=GD_CANVAS_H;
    if(left>=right || top>=bottom)return 1;
    unsigned x0=(unsigned)gd_ceil(left,GD_WIDTH,GD_CANVAS_W),x1=(unsigned)gd_ceil(right,GD_WIDTH,GD_CANVAS_W);
    unsigned y0=(unsigned)gd_ceil(top,GD_HEIGHT,GD_CANVAS_H),y1=(unsigned)gd_ceil(bottom,GD_HEIGHT,GD_CANVAS_H);
    uint16_t columns[GD_WIDTH];
    for(unsigned x=x0;x<x1;x++) {
        int64_t logical=(uint64_t)x*GD_CANVAS_W/GD_WIDTH;
        columns[x]=(uint16_t)(src.x+(logical-dst.x)*src.w/dst.w);
    }
    const uint8_t *base=(const uint8_t *)source+offset;
    for(unsigned y=y0;y<y1;y++) {
        int64_t logical=(uint64_t)y*GD_CANVAS_H/GD_HEIGHT;
        unsigned sy=(unsigned)(src.y+(logical-dst.y)*src.h/dst.h);
        const uint32_t *row=(const uint32_t *)(base+(uint64_t)sy*stride);
        uint32_t *out=(uint32_t *)((uint8_t *)panel+(size_t)y*GD_STRIDE);
        unsigned x=x0;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        /* Normal WriteCombine panel RAM, NOT device registers. Keep every
         * 64-bit destination naturally aligned; never read panel memory.
         * Fixed-size builtin copies avoid type-punning/strict-aliasing UB.
         * No NEON/FPU is used by the kernel's general-register-only build.
         * The source gather, bounds, pixel order and transform are unchanged.
         */
        if(packed64 == 1) {
            if(x<x1 && ((uintptr_t)(out+x)&7)) {out[x]=row[columns[x]];x++;}
            for(;x+3<x1;x+=4) {
                uint64_t lo=(uint64_t)row[columns[x]] | ((uint64_t)row[columns[x+1]]<<32);
                uint64_t hi=(uint64_t)row[columns[x+2]] | ((uint64_t)row[columns[x+3]]<<32);
                __builtin_memcpy(out+x,&lo,sizeof(lo));
                __builtin_memcpy(out+x+2,&hi,sizeof(hi));
            }
        }
#else
        (void)packed64;
#endif
        for(;x<x1;x++)out[x]=row[columns[x]];
    }
    return 1;
}
/* Preserve the original scalar default. The kernel opts in explicitly. */
static inline int gd_present(void *panel,size_t panel_bytes,const void *source,
    size_t source_bytes,size_t offset,unsigned width,unsigned height,unsigned stride,
    struct gd_rect src,struct gd_rect dst) {
    return gd_present_mode(panel,panel_bytes,source,source_bytes,offset,width,height,
                           stride,src,dst,0);
}
#endif
