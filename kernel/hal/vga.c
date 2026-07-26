/*
 * hal/vga.c - VGA text-mode console (80x25, colour, at physical 0xB8000).
 *
 * Each cell is a 16-bit word: low byte character, high byte attribute. The
 * framebuffer is reached through MmPhysToVirt so this keeps working once the
 * identity map is replaced by a real direct map.
 */
#include <ntos/hal.h>
#include <ntos/mm.h>

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_PHYS    0xB8000

/* CRT controller registers, used to move the hardware cursor. */
#define CRTC_INDEX  0x3D4
#define CRTC_DATA   0x3D5

static volatile UINT16 *g_vga;
static UINT8            g_attr = 0x07; /* light grey on black */
static int             g_row = 0;
static int             g_col = 0;

static ALWAYS_INLINE UINT16 vga_cell(char c, UINT8 attr)
{
    return (UINT16)((UINT8)c | ((UINT16)attr << 8));
}

static void vga_update_cursor(void)
{
    UINT16 pos = (UINT16)(g_row * VGA_WIDTH + g_col);
    __outbyte(CRTC_INDEX, 0x0F);
    __outbyte(CRTC_DATA, (UINT8)(pos & 0xFF));
    __outbyte(CRTC_INDEX, 0x0E);
    __outbyte(CRTC_DATA, (UINT8)((pos >> 8) & 0xFF));
}

void HalVgaSetColor(UINT8 attribute)
{
    g_attr = attribute;
}

void HalVgaClear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        g_vga[i] = vga_cell(' ', g_attr);
    g_row = 0;
    g_col = 0;
    vga_update_cursor();
}

void HalInitializeVga(void)
{
    g_vga = (volatile UINT16 *)MmPhysToVirt(VGA_PHYS);
    g_attr = VGA_COLOR(VGA_LGRAY, VGA_BLACK);
    HalVgaClear();
}

static void vga_scroll(void)
{
    /* Move rows 1..H-1 up by one and clear the last row. */
    for (int r = 1; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            g_vga[(r - 1) * VGA_WIDTH + c] = g_vga[r * VGA_WIDTH + c];

    for (int c = 0; c < VGA_WIDTH; c++)
        g_vga[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = vga_cell(' ', g_attr);

    g_row = VGA_HEIGHT - 1;
}

void HalVgaPutChar(char c)
{
    if (!g_vga)
        return;

    switch (c) {
    case '\n':
        g_col = 0;
        g_row++;
        break;
    case '\r':
        g_col = 0;
        break;
    case '\t':
        g_col = (g_col + 8) & ~7;
        break;
    case '\b':
        if (g_col > 0)
            g_col--;
        break;
    default:
        g_vga[g_row * VGA_WIDTH + g_col] = vga_cell(c, g_attr);
        g_col++;
        break;
    }

    if (g_col >= VGA_WIDTH) {
        g_col = 0;
        g_row++;
    }
    if (g_row >= VGA_HEIGHT)
        vga_scroll();

    vga_update_cursor();
}
