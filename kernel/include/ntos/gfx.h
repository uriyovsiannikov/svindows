/*
 * ntos/gfx.h - linear framebuffer graphics.
 *
 * When GRUB provides a framebuffer (requested in the Multiboot2 header), the
 * kernel draws to it directly: pixels, rectangles, and an 8x16 bitmap-font text
 * console. This is the foundation the desktop is built on.
 */
#ifndef _NTOS_GFX_H_
#define _NTOS_GFX_H_

#include <nt/ntdef.h>

typedef struct _GFX_FRAMEBUFFER {
    UINT64  PhysAddr;
    UINT32 *Base;      /* mapped virtual address (32-bpp assumed)      */
    UINT32  Pitch;     /* bytes per scanline                           */
    UINT32  Width;
    UINT32  Height;
    UINT8   Bpp;
    UINT8   RedShift;
    UINT8   GreenShift;
    UINT8   BlueShift;
    BOOLEAN Present;
} GFX_FRAMEBUFFER;

extern GFX_FRAMEBUFFER GfxFramebuffer;

/* Pack an (r,g,b) triple into a pixel for the current framebuffer format. */
UINT32 GfxColor(UINT8 r, UINT8 g, UINT8 b);

BOOLEAN GfxAvailable(void);
void GfxInitialize(void);   /* map the framebuffer (needs Mm) */

void GfxPutPixel(UINT32 x, UINT32 y, UINT32 color);
void GfxFillRect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color);
void GfxClear(UINT32 color);
void GfxDrawChar(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg);
void GfxDrawString(UINT32 x, UINT32 y, const char *s, UINT32 fg, UINT32 bg);

#define GFX_FONT_W 8
#define GFX_FONT_H 16

#endif /* _NTOS_GFX_H_ */
