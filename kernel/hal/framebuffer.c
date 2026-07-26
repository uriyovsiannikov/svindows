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

/* ------------------------------------------------------------------ */
/* Framebuffer text console                                           */
/* ------------------------------------------------------------------ */

static UINT32 g_con_x, g_con_y;     /* pixel origin of the text area */
static UINT32 g_con_w, g_con_h;     /* text area size in pixels      */
static UINT32 g_con_cols, g_con_rows;
static UINT32 g_cur_col, g_cur_row;
static UINT32 g_fg, g_bg;

void GfxConsoleInit(void)
{
    /* The text area sits below a title bar (see the desktop composition). */
    g_con_x = 8;
    g_con_y = 44;
    g_con_w = GfxFramebuffer.Width - 16;
    g_con_h = GfxFramebuffer.Height - g_con_y - 40; /* leave room for a taskbar */
    g_con_cols = g_con_w / GFX_FONT_W;
    g_con_rows = g_con_h / GFX_FONT_H;
    g_cur_col = g_cur_row = 0;
    g_fg = GfxColor(0xD0, 0xD8, 0xE0);
    g_bg = GfxColor(0x10, 0x14, 0x20);

    GfxFillRect(g_con_x, g_con_y, g_con_w, g_con_h, g_bg);
}

static void con_scroll(void)
{
    /* Shift the text area up by one glyph row and clear the last row. */
    UINT32 sp = stride_px();
    UINT32 line = GFX_FONT_H;
    for (UINT32 y = 0; y < g_con_h - line; y++) {
        UINT32 *dst = GfxFramebuffer.Base + (g_con_y + y) * sp + g_con_x;
        UINT32 *src = GfxFramebuffer.Base + (g_con_y + y + line) * sp + g_con_x;
        for (UINT32 x = 0; x < g_con_w; x++)
            dst[x] = src[x];
    }
    GfxFillRect(g_con_x, g_con_y + g_con_h - line, g_con_w, line, g_bg);
    g_cur_row = g_con_rows - 1;
}

void GfxConsolePutChar(char c)
{
    if (!GfxAvailable())
        return;

    if (c == '\n') {
        g_cur_col = 0;
        g_cur_row++;
    } else if (c == '\r') {
        g_cur_col = 0;
    } else if (c == '\t') {
        g_cur_col = (g_cur_col + 4) & ~3u;
    } else {
        GfxDrawChar(g_con_x + g_cur_col * GFX_FONT_W,
                    g_con_y + g_cur_row * GFX_FONT_H, c, g_fg, g_bg);
        g_cur_col++;
    }

    if (g_cur_col >= g_con_cols) {
        g_cur_col = 0;
        g_cur_row++;
    }
    if (g_cur_row >= g_con_rows)
        con_scroll();
}

/* ------------------------------------------------------------------ */
/* Mouse cursor                                                       */
/* ------------------------------------------------------------------ */

#define CURSOR_W 12
#define CURSOR_H 19

/* Classic arrow: 0 = transparent, 1 = black outline, 2 = white fill. */
static const UINT8 cursor_sprite[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

static UINT32 cur_saved[CURSOR_H][CURSOR_W];
static INT32 cur_x = -1, cur_y; /* last position; -1 until first placed */
static UINT32 cur_w, cur_h;     /* clipped extent actually saved */
static BOOLEAN cur_shown;

/* Restore the pixels the cursor is covering; leaves cur_x/cur_y intact so the
 * cursor can be shown again at the same spot. */
static void cursor_restore(void)
{
    if (!cur_shown)
        return;
    UINT32 sp = stride_px();
    for (UINT32 r = 0; r < cur_h; r++) {
        UINT32 *dst = GfxFramebuffer.Base + ((UINT32)cur_y + r) * sp + cur_x;
        for (UINT32 c = 0; c < cur_w; c++)
            dst[c] = cur_saved[r][c];
    }
    cur_shown = FALSE;
}

/* Save the background at (x,y) and blit the arrow over it. */
static void cursor_blit(INT32 x, INT32 y)
{
    cur_w = CURSOR_W;
    cur_h = CURSOR_H;
    if (x + (INT32)cur_w > (INT32)GfxFramebuffer.Width)
        cur_w = GfxFramebuffer.Width - x;
    if (y + (INT32)cur_h > (INT32)GfxFramebuffer.Height)
        cur_h = GfxFramebuffer.Height - y;

    UINT32 sp = stride_px();
    UINT32 black = GfxColor(0x00, 0x00, 0x00);
    UINT32 white = GfxColor(0xff, 0xff, 0xff);
    for (UINT32 r = 0; r < cur_h; r++) {
        UINT32 *dst = GfxFramebuffer.Base + ((UINT32)y + r) * sp + x;
        for (UINT32 c = 0; c < cur_w; c++) {
            cur_saved[r][c] = dst[c];
            UINT8 px = cursor_sprite[r][c];
            if (px == 1)      dst[c] = black;
            else if (px == 2) dst[c] = white;
        }
    }
    cur_x = x;
    cur_y = y;
    cur_shown = TRUE;
}

void GfxMoveCursor(INT32 x, INT32 y)
{
    if (!GfxAvailable())
        return;

    cursor_restore();

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (INT32)GfxFramebuffer.Width)  x = (INT32)GfxFramebuffer.Width - 1;
    if (y >= (INT32)GfxFramebuffer.Height) y = (INT32)GfxFramebuffer.Height - 1;

    cursor_blit(x, y);
}

/* Temporarily lift the cursor so drawing underneath it (e.g. the console
 * scrolling) doesn't smear its saved background. GfxShowCursor puts it back at
 * the same spot, re-sampling the now-current background. Callers pair these
 * around a drawing burst; both are cheap no-ops when no cursor is placed. */
void GfxHideCursor(void)
{
    if (GfxAvailable())
        cursor_restore();
}

void GfxShowCursor(void)
{
    if (GfxAvailable() && cur_x >= 0 && !cur_shown)
        cursor_blit(cur_x, cur_y);
}
