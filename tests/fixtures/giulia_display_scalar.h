/* Frozen scalar oracle from 783ae50; identical to the booted CI header. */
/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 native panel presentation. UIKit keeps its supported D22 layout; every
 * logical pixel rectangle is mapped over the complete 1264x2780 panel. The
 * inverse HID transform is normalized over that same complete panel.
 */
#ifndef GIULIA_DISPLAY_SCALAR_REFERENCE_H
#define GIULIA_DISPLAY_SCALAR_REFERENCE_H
#include <stdint.h>
#include <stddef.h>
#define GD_WIDTH 1264u
#define GD_HEIGHT 2780u
#define GD_STRIDE 5056u
#define GD_CANVAS_W 1125u
#define GD_CANVAS_H 2436u
struct gd_reference_rect { int32_t x,y,w,h; };
static inline unsigned gd_reference_touch(int coordinate,unsigned extent) {
    if(coordinate<=0 || extent<2)return 0;
    if((unsigned)coordinate>=extent-1)return 65535;
    return (unsigned)((uint64_t)(unsigned)coordinate*65535/(extent-1));
}
static inline int64_t gd_reference_ceil(int64_t value,unsigned mul,unsigned div) {
    return (value*mul+div-1)/div; /* called only with nonnegative values */
}
/* BGRA/32-bit pixels, no rotation. Bounds are checked before the first write.
 * A source rectangle may be scaled independently of the display transform.
 * Negative destination origins are clipped without shifting source content.
 * Out-of-range source rectangles are rejected rather than reading padding.
 */
static inline int gd_reference_present(void *panel,size_t panel_bytes,const void *source,
    size_t source_bytes,size_t offset,unsigned width,unsigned height,unsigned stride,
    struct gd_reference_rect src,struct gd_reference_rect dst) {
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
    unsigned x0=(unsigned)gd_reference_ceil(left,GD_WIDTH,GD_CANVAS_W),x1=(unsigned)gd_reference_ceil(right,GD_WIDTH,GD_CANVAS_W);
    unsigned y0=(unsigned)gd_reference_ceil(top,GD_HEIGHT,GD_CANVAS_H),y1=(unsigned)gd_reference_ceil(bottom,GD_HEIGHT,GD_CANVAS_H);
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
        for(unsigned x=x0;x<x1;x++)out[x]=row[columns[x]];
    }
    return 1;
}
#endif
