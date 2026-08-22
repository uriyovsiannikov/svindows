/*
 * hal/framebuffer.c - linear framebuffer graphics and a text console.
 *
 * Draws directly to the GRUB-provided 32-bpp framebuffer: pixels, rectangles,
 * bitmap-font glyphs, and a scrolling text console the kernel log renders into
 * once graphics are up.
 */
#include <ntos/gfx.h>
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>

extern const UINT8 Font8x16[95][16];

GFX_FRAMEBUFFER GfxFramebuffer;

BOOLEAN GfxAvailable(void)
{
    return GfxFramebuffer.Present && GfxFramebuffer.Base != NULL;
}

UINT32 GfxColor(UINT8 r, UINT8 g, UINT8 b)
{
    return ((UINT32)r << GfxFramebuffer.RedShift) |
           ((UINT32)g << GfxFramebuffer.GreenShift) |
           ((UINT32)b << GfxFramebuffer.BlueShift);
}

void GfxInitialize(void)
{
    if (!GfxFramebuffer.Present || GfxFramebuffer.Bpp != 32)
        return;

    /* Map the framebuffer into the direct-map window (it lives above RAM). */
    UINT64 phys = GfxFramebuffer.PhysAddr;
    UINT64 size = (UINT64)GfxFramebuffer.Pitch * GfxFramebuffer.Height;
    for (UINT64 off = 0; off < size; off += PAGE_SIZE)
        MmMapPage((UINT64)(ULONG_PTR)MmPhysToVirt(phys + off), phys + off,
                  PTE_WRITE);

    GfxFramebuffer.Base = (UINT32 *)MmPhysToVirt(phys);

    KeLog("[gfx]  framebuffer %ux%u x%u bpp @ 0x%lx -> %p\n",
          GfxFramebuffer.Width, GfxFramebuffer.Height, GfxFramebuffer.Bpp,
          (unsigned long)phys, (void *)GfxFramebuffer.Base);
}

static ALWAYS_INLINE UINT32 stride_px(void)
{
    return GfxFramebuffer.Pitch / 4;
}

void GfxPutPixel(UINT32 x, UINT32 y, UINT32 color)
{
    if (x < GfxFramebuffer.Width && y < GfxFramebuffer.Height)
        GfxFramebuffer.Base[y * stride_px() + x] = color;
}

void GfxFillRect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color)
{
    if (!GfxAvailable())
        return;
    UINT32 x1 = x + w, y1 = y + h;
    if (x1 > GfxFramebuffer.Width)  x1 = GfxFramebuffer.Width;
    if (y1 > GfxFramebuffer.Height) y1 = GfxFramebuffer.Height;

    UINT32 sp = stride_px();
    for (UINT32 yy = y; yy < y1; yy++) {
        UINT32 *row = GfxFramebuffer.Base + yy * sp;
        for (UINT32 xx = x; xx < x1; xx++)
            row[xx] = color;
    }
}

void GfxClear(UINT32 color)
{
    GfxFillRect(0, 0, GfxFramebuffer.Width, GfxFramebuffer.Height, color);
}

void GfxDrawChar(UINT32 x, UINT32 y, char c, UINT32 fg, UINT32 bg)
{
    if (!GfxAvailable())
        return;
    UINT8 uc = (UINT8)c;
    const UINT8 *glyph = (uc >= 32 && uc < 127) ? Font8x16[uc - 32] : Font8x16[0];

    UINT32 sp = stride_px();
    for (UINT32 row = 0; row < GFX_FONT_H; row++) {
        UINT8 bits = glyph[row];
        UINT32 *dst = GfxFramebuffer.Base + (y + row) * sp + x;
        for (UINT32 col = 0; col < GFX_FONT_W; col++)
            dst[col] = (bits & (1u << col)) ? fg : bg;
    }
}

void GfxDrawString(UINT32 x, UINT32 y, const char *s, UINT32 fg, UINT32 bg)
{
    while (*s) {
        GfxDrawChar(x, y, *s++, fg, bg);
        x += GFX_FONT_W;
    }
}
