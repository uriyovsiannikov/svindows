/*
 * win32k/gdiobj.c - GDI objects, surfaces, and the drawing services.
 *
 * The genuine gdi32 is a thin client over a kernel that owns every GDI object.
 * A handle is (type << 16) | index; the index selects a 24-byte GDICELL64 in
 * the table at PEB->GdiSharedHandleTable, and gdi32 checks the cell's type,
 * uniqueness and owning process before it will touch the object. The layout and
 * the checks below are not guessed: they were read out of the supplied
 * gdi32/gdi32full binaries.
 *
 *   gdi32!GdiGetEntry(handle, out)
 *       r9 = pGdiSharedHandleTable
 *       index = handle & 0xffff; cell = r9 + index*24; copy 24 bytes
 *       ... and for a handle whose upper bits do not match the cell's byte at
 *       +0x0D it tail-jumps to the NtGdiGetEntry syscall instead, which is the
 *       path every real handle takes. So the kernel is asked for the cell.
 *
 *   gdi32full!GetDCBrushColor(hdc)
 *       index >= *gMaxGdiHandleCount        -> fail
 *       GdiGetEntry(hdc, &cell)             -> negative NTSTATUS -> fail
 *       cell+0x0E (nType) != 1              -> fail        (1 == DC)
 *       cell+0x0C (nUpper) != handle >> 16  -> fail
 *       (cell+0x08 (ProcessId) & ~1) != pid -> fail
 *       cell+0x10 (pUserInfo) == NULL       -> fail
 *       return *(DWORD *)(pUserInfo + 0x34)                (DC_ATTR.ulBrushClr)
 *
 * That last offset pins DC_ATTR to the layout the ReactOS headers describe, so
 * the colour fields below follow it. It matters because SetTextColor and
 * SetBkColor are pure client-side writes into DC_ATTR -- they never enter the
 * kernel -- so the drawing services have to read the current colours out of the
 * block rather than keeping their own copy.
 */
#include <ntos/win32k.h>
#include <ntos/ps.h>
#include <ntos/mm.h>
#include <ntos/gfx.h>
#include <ntos/ex.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include <nt/ntstatus.h>

extern const UINT8 Font8x16[95][16];

/* ------------------------------------------------------------------ */
/* The shared handle table                                             */
/* ------------------------------------------------------------------ */

typedef struct _GDICELL64 {
    UINT64 pKernelInfo;
    UINT32 ProcessId;
    UINT16 nUpper;
    UINT16 nType;
    UINT64 pUserInfo;
} GDICELL64;

_Static_assert(sizeof(GDICELL64) == 24, "GDICELL64 must be 24 bytes");

/* nType values (the GDI handle type nibble, LO_*_TYPE >> 16). */
#define GDI_TYPE_DC     0x01
#define GDI_TYPE_REGION 0x04
#define GDI_TYPE_BITMAP 0x05
#define GDI_TYPE_FONT   0x0A
#define GDI_TYPE_BRUSH  0x10
#define GDI_TYPE_PEN    0x30

/* DC_ATTR offsets (see the header comment). */
#define DCATTR_PVLDC       0x00
#define DCATTR_ULDIRTY     0x08
#define DCATTR_HBRUSH      0x10
#define DCATTR_HPEN        0x18
#define DCATTR_CRBACKGND   0x20
#define DCATTR_ULBACKGND   0x24
#define DCATTR_CRFOREGND   0x28
#define DCATTR_ULFOREGND   0x2C
#define DCATTR_CRBRUSH     0x30
#define DCATTR_ULBRUSH     0x34
#define DCATTR_CRPEN       0x38
#define DCATTR_ULPEN       0x3C
#define DCATTR_SIZE        0x200

#define GDI_MAX_OBJECTS 512   /* handle indices 1..511 */
#define GDI_TABLE_ENTRIES 0x10000

/* ------------------------------------------------------------------ */
/* Kernel-side objects                                                 */
/* ------------------------------------------------------------------ */

/* A drawing target: either the linear framebuffer or a pool-backed bitmap.
 * Both are 32 bits per pixel in the framebuffer's channel order, so a blit
 * between them is a straight copy. */
typedef struct _W32K_SURFACE {
    UINT32 *Bits;
    UINT32  Width;
    UINT32  Height;
    UINT32  Stride;   /* pixels per scanline */
    BOOLEAN IsScreen;
} W32K_SURFACE;

typedef struct _W32K_DC {
    W32K_SURFACE *Surface;
    W32K_SURFACE  ScreenSurface; /* used when the DC draws to the screen */
    UINT64 Hwnd;
    INT32  OrgX, OrgY;           /* where (0,0) of the DC lands on the surface */
    INT32  ClipX, ClipY, ClipW, ClipH; /* in surface coordinates */
    INT32  CurX, CurY;           /* current position for MoveTo/LineTo */
    UINT64 SelectedBrush;
    UINT64 SelectedPen;
    UINT64 SelectedFont;
    UINT64 SelectedBitmap;
} W32K_DC;

typedef struct _W32K_BRUSH {
    UINT32 Color;   /* COLORREF */
    BOOLEAN Hollow;
} W32K_BRUSH;

typedef struct _W32K_BITMAP {
    W32K_SURFACE Surface;
    void *Storage;
} W32K_BITMAP;

typedef struct _W32K_OBJECT {
    UINT16 Type;      /* 0 when the slot is free */
    UINT16 Upper;     /* the handle's high 16 bits */
    union {
        W32K_DC     Dc;
        W32K_BRUSH  Brush;
        W32K_BITMAP Bitmap;
    } u;
} W32K_OBJECT;

static W32K_OBJECT g_gdi_objects[GDI_MAX_OBJECTS];
static UINT16 g_gdi_next_index = 1;
static UINT64 g_gdi_attr_next = PROCESS_GDI_ATTR_VA;

static GDICELL64 *gdi_table(void)
{
    return (GDICELL64 *)PROCESS_GDI_SHARED_TABLE_VA;
}

static W32K_OBJECT *gdi_object(UINT64 handle, UINT16 type)
{
    UINT16 index = (UINT16)(handle & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS)
        return NULL;
    W32K_OBJECT *object = &g_gdi_objects[index];
    if (!object->Type || (type && object->Type != type))
        return NULL;
    if (object->Upper != (UINT16)(handle >> 16))
        return NULL;
    return object;
}

/* Allocate a handle of `type`, publish its cell, and return the handle. The
 * handle's upper half is the type, which is what gdi32 compares the cell's
 * nUpper against. */
static UINT64 gdi_allocate(UINT16 type, UINT64 user_info, W32K_OBJECT **out)
{
    for (UINT16 tries = 0; tries < GDI_MAX_OBJECTS; tries++) {
        UINT16 index = g_gdi_next_index;
        g_gdi_next_index = (UINT16)(index + 1);
        if (g_gdi_next_index >= GDI_MAX_OBJECTS)
            g_gdi_next_index = 1;
        if (!index || g_gdi_objects[index].Type)
            continue;

        W32K_OBJECT *object = &g_gdi_objects[index];
        memset(object, 0, sizeof(*object));
        object->Type = type;
        object->Upper = type;

        GDICELL64 *cell = &gdi_table()[index];
        cell->pKernelInfo = (UINT64)object;
        cell->ProcessId = PROCESS_CLIENT_ID; /* must be even; see ps.h */
        cell->nUpper = type;
        cell->nType = type;
        cell->pUserInfo = user_info;

        if (out)
            *out = object;
        return ((UINT64)type << 16) | index;
    }
    KeLog("[w32k] GDI handle table full\n");
    return 0;
}

static void gdi_free(UINT64 handle)
{
    UINT16 index = (UINT16)(handle & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS || !g_gdi_objects[index].Type)
        return;
    g_gdi_objects[index].Type = 0;
    memset(&gdi_table()[index], 0, sizeof(GDICELL64));
}

/* Hand out a zeroed DC_ATTR block from the user-writable arena. */
static UINT64 gdi_allocate_attr(void)
{
    if (g_gdi_attr_next + DCATTR_SIZE >
        PROCESS_GDI_ATTR_VA + PROCESS_GDI_ATTR_SIZE)
        return 0;
    UINT64 block = g_gdi_attr_next;
    g_gdi_attr_next += DCATTR_SIZE;
    memset((void *)block, 0, DCATTR_SIZE);
    return block;
}

void W32kInitializeGdi(void)
{
    memset(g_gdi_objects, 0, sizeof(g_gdi_objects));
    g_gdi_next_index = 1;
    g_gdi_attr_next = PROCESS_GDI_ATTR_VA;
    memset(gdi_table(), 0, GDI_TABLE_ENTRIES * sizeof(GDICELL64));
    KeLog("[w32k] GDI object table ready (%u handles, cells at 0x%lx, "
          "attributes at 0x%lx)\n", (unsigned)GDI_MAX_OBJECTS - 1,
          (unsigned long)PROCESS_GDI_SHARED_TABLE_VA,
          (unsigned long)PROCESS_GDI_ATTR_VA);
}

/* ------------------------------------------------------------------ */
/* Surfaces and clipped drawing                                        */
/* ------------------------------------------------------------------ */

/* COLORREF is 0x00BBGGRR; the framebuffer's channel order comes from the
 * Multiboot2 framebuffer tag, so go through GfxColor rather than assuming. */
static UINT32 gdi_pixel(UINT32 colorref)
{
    return GfxColor((UINT8)(colorref & 0xFF), (UINT8)((colorref >> 8) & 0xFF),
                    (UINT8)((colorref >> 16) & 0xFF));
}

static void surface_fill(W32K_SURFACE *surface, INT32 x, INT32 y, INT32 w,
                         INT32 h, UINT32 pixel)
{
    if (!surface || !surface->Bits || w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (INT32)surface->Width || y >= (INT32)surface->Height)
        return;
    if (x + w > (INT32)surface->Width)
        w = (INT32)surface->Width - x;
    if (y + h > (INT32)surface->Height)
        h = (INT32)surface->Height - y;
    if (w <= 0 || h <= 0)
        return;

    for (INT32 row = 0; row < h; row++) {
        UINT32 *line = surface->Bits + (UINT32)(y + row) * surface->Stride + x;
        for (INT32 col = 0; col < w; col++)
            line[col] = pixel;
    }
}

static void surface_pixel(W32K_SURFACE *surface, INT32 x, INT32 y, UINT32 pixel)
{
    if (!surface || !surface->Bits || x < 0 || y < 0 ||
        x >= (INT32)surface->Width || y >= (INT32)surface->Height)
        return;
    surface->Bits[(UINT32)y * surface->Stride + (UINT32)x] = pixel;
}

/* Intersect a DC-relative rectangle with the DC's clip box and translate it
 * into surface coordinates. Returns FALSE when nothing is left. */
static BOOLEAN dc_clip(W32K_DC *dc, INT32 *x, INT32 *y, INT32 *w, INT32 *h)
{
    INT32 left = dc->OrgX + *x, top = dc->OrgY + *y;
    INT32 right = left + *w, bottom = top + *h;
    INT32 clip_right = dc->ClipX + dc->ClipW, clip_bottom = dc->ClipY + dc->ClipH;

    if (left < dc->ClipX) left = dc->ClipX;
    if (top < dc->ClipY) top = dc->ClipY;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (right <= left || bottom <= top)
        return FALSE;
    *x = left;
    *y = top;
    *w = right - left;
    *h = bottom - top;
    return TRUE;
}

static void dc_fill(W32K_DC *dc, INT32 x, INT32 y, INT32 w, INT32 h,
                    UINT32 colorref)
{
    if (!dc_clip(dc, &x, &y, &w, &h))
        return;
    surface_fill(dc->Surface, x, y, w, h, gdi_pixel(colorref));
}

/* Draw one 8x16 glyph at a DC-relative position, clipped, with an opaque or
 * transparent background. */
static void dc_glyph(W32K_DC *dc, INT32 x, INT32 y, char c, UINT32 fg,
                     UINT32 bg, BOOLEAN opaque)
{
    UINT8 uc = (UINT8)c;
    const UINT8 *glyph = (uc >= 32 && uc < 127) ? Font8x16[uc - 32] : Font8x16[0];
    UINT32 fg_pixel = gdi_pixel(fg), bg_pixel = gdi_pixel(bg);

    for (INT32 row = 0; row < 16; row++) {
        UINT8 bits = glyph[row];
        for (INT32 col = 0; col < 8; col++) {
            BOOLEAN set = (bits & (1u << col)) != 0;
            if (!set && !opaque)
                continue;
            INT32 px = x + col, py = y + row, pw = 1, ph = 1;
            if (!dc_clip(dc, &px, &py, &pw, &ph))
                continue;
            surface_pixel(dc->Surface, px, py, set ? fg_pixel : bg_pixel);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Device contexts                                                     */
/* ------------------------------------------------------------------ */

static void screen_surface(W32K_SURFACE *surface)
{
    surface->Bits = GfxFramebuffer.Base;
    surface->Width = GfxFramebuffer.Width;
    surface->Height = GfxFramebuffer.Height;
    surface->Stride = GfxFramebuffer.Pitch / 4;
    surface->IsScreen = TRUE;
}

UINT64 W32kCreateWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                          INT32 height)
{
    if (!GfxAvailable())
        return 0;

    UINT64 attr = gdi_allocate_attr();
    if (!attr) {
        KeLog("[w32k] DC_ATTR arena exhausted\n");
        return 0;
    }

    W32K_OBJECT *object = NULL;
    UINT64 hdc = gdi_allocate(GDI_TYPE_DC, attr, &object);
    if (!hdc)
        return 0;

    W32K_DC *dc = &object->u.Dc;
    screen_surface(&dc->ScreenSurface);
    dc->Surface = &dc->ScreenSurface;
    dc->Hwnd = hwnd;

    /* A window with no size yet still gets a DC; it simply clips everything
     * away, which is what a zero-sized client area means. */
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    dc->OrgX = x;
    dc->OrgY = y;
    dc->ClipX = x;
    dc->ClipY = y;
    dc->ClipW = width;
    dc->ClipH = height;

    /* Defaults a fresh DC starts with: black text on white, white brush. */
    *(UINT32 *)(attr + DCATTR_CRFOREGND) = 0x00000000;
    *(UINT32 *)(attr + DCATTR_ULFOREGND) = 0x00000000;
    *(UINT32 *)(attr + DCATTR_CRBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_CRBRUSH) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBRUSH) = 0x00FFFFFF;

    KeLog("[w32k] DC %p for HWND %p at %ld,%ld %ldx%ld (attr 0x%lx)\n",
          (void *)hdc, (void *)hwnd, (long)x, (long)y, (long)width,
          (long)height, (unsigned long)attr);
    return hdc;
}

void W32kReleaseDc(UINT64 hdc)
{
    if (gdi_object(hdc, GDI_TYPE_DC))
        gdi_free(hdc);
}

static W32K_DC *dc_from_handle(UINT64 hdc)
{
    W32K_OBJECT *object = gdi_object(hdc, GDI_TYPE_DC);
    return object ? &object->u.Dc : NULL;
}

/* The DC's user-mode attribute block, or 0. */
static UINT64 dc_attr(UINT64 hdc)
{
    UINT16 index = (UINT16)(hdc & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS)
        return 0;
    return gdi_table()[index].pUserInfo;
}

static UINT32 dc_text_color(UINT64 hdc)
{
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRFOREGND) : 0;
}

static UINT32 dc_bk_color(UINT64 hdc)
{
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRBACKGND) : 0x00FFFFFF;
}

/* ------------------------------------------------------------------ */
/* Services: object lifetime                                           */
/* ------------------------------------------------------------------ */

/* NTSTATUS NtGdiGetEntry(ULONG handle, GDICELL64 *out) -- gdi32!GdiGetEntry
 * tail-jumps here for every handle whose upper bits it cannot match against the
 * table itself, which is every real handle. */
UINT64 NtGdiGetEntry(UINT64 *a)
{
    UINT64 handle = a[0];
    GDICELL64 *out = (GDICELL64 *)a[1];
    if (!out || !MmProbeForWrite((UINT64)out, sizeof(*out)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT16 index = (UINT16)(handle & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS || !g_gdi_objects[index].Type) {
        memset(out, 0, sizeof(*out));
        return (UINT64)STATUS_INVALID_HANDLE;
    }
    *out = gdi_table()[index];
    return (UINT64)STATUS_SUCCESS;
}

/* HBRUSH NtGdiCreateSolidBrush(COLORREF colour, HBRUSH reuse) */
UINT64 NtGdiCreateSolidBrush(UINT64 *a)
{
    W32K_OBJECT *object = NULL;
    UINT64 handle = gdi_allocate(GDI_TYPE_BRUSH, 0, &object);
    if (!handle)
        return 0;
    object->u.Brush.Color = (UINT32)a[0];
    object->u.Brush.Hollow = FALSE;
    return handle;
}

/* HPEN NtGdiCreatePen(int style, int width, COLORREF colour, HBRUSH) */
UINT64 NtGdiCreatePen(UINT64 *a)
{
    W32K_OBJECT *object = NULL;
    UINT64 handle = gdi_allocate(GDI_TYPE_PEN, 0, &object);
    if (!handle)
        return 0;
    object->u.Brush.Color = (UINT32)a[2];
    object->u.Brush.Hollow = ((INT32)a[0] == 5); /* PS_NULL */
    return handle;
}

/* HFONT NtGdiHfontCreate(...): one bitmap font exists, so every logical font
 * maps to it. The handle still has to be a real GDI object, because gdi32
 * validates it before every text call. */
UINT64 NtGdiHfontCreate(UINT64 *a)
{
    return gdi_allocate(GDI_TYPE_FONT, 0, NULL);
}

UINT64 NtGdiCreateRectRgn(UINT64 *a)
{
    return gdi_allocate(GDI_TYPE_REGION, 0, NULL);
}

UINT64 NtGdiDeleteObjectApp(UINT64 *a)
{
    UINT16 index = (UINT16)(a[0] & 0xFFFF);
    if (!index || index >= GDI_MAX_OBJECTS)
        return 0;
    W32K_OBJECT *object = &g_gdi_objects[index];
    if (!object->Type)
        return 0;
    if (object->Type == GDI_TYPE_BITMAP && object->u.Bitmap.Storage)
        ExFreePool(object->u.Bitmap.Storage);
    gdi_free(a[0]);
    return 1;
}

UINT64 NtGdiFlush(UINT64 *a)
{
    return 1;
}

/* ------------------------------------------------------------------ */
/* Services: memory DCs and bitmaps                                    */
/* ------------------------------------------------------------------ */

/* HDC NtGdiCreateCompatibleDC(HDC reference): a memory DC starts out bound to
 * nothing drawable, exactly as on Windows -- a 1x1 monochrome default. It
 * becomes useful once a bitmap is selected into it. */
UINT64 NtGdiCreateCompatibleDC(UINT64 *a)
{
    UINT64 attr = gdi_allocate_attr();
    if (!attr)
        return 0;
    W32K_OBJECT *object = NULL;
    UINT64 hdc = gdi_allocate(GDI_TYPE_DC, attr, &object);
    if (!hdc)
        return 0;
    W32K_DC *dc = &object->u.Dc;
    dc->Surface = NULL;
    *(UINT32 *)(attr + DCATTR_CRBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBACKGND) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_CRBRUSH) = 0x00FFFFFF;
    *(UINT32 *)(attr + DCATTR_ULBRUSH) = 0x00FFFFFF;
    return hdc;
}

static UINT64 gdi_create_bitmap(INT32 width, INT32 height)
{
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return 0;
    SIZE_T bytes = (SIZE_T)width * (SIZE_T)height * 4;
    void *bits = ExAllocatePoolWithTag(NonPagedPool, bytes, 'mtiB');
    if (!bits)
        return 0;
    memset(bits, 0, bytes);

    W32K_OBJECT *object = NULL;
    UINT64 handle = gdi_allocate(GDI_TYPE_BITMAP, 0, &object);
    if (!handle) {
        ExFreePool(bits);
        return 0;
    }
    object->u.Bitmap.Storage = bits;
    object->u.Bitmap.Surface.Bits = bits;
    object->u.Bitmap.Surface.Width = (UINT32)width;
    object->u.Bitmap.Surface.Height = (UINT32)height;
    object->u.Bitmap.Surface.Stride = (UINT32)width;
    object->u.Bitmap.Surface.IsScreen = FALSE;
    return handle;
}

/* HBITMAP NtGdiCreateCompatibleBitmap(HDC, int cx, int cy) */
UINT64 NtGdiCreateCompatibleBitmap(UINT64 *a)
{
    return gdi_create_bitmap((INT32)a[1], (INT32)a[2]);
}

/* HBITMAP NtGdiCreateBitmap(int cx, int cy, UINT planes, UINT bpp, void *bits) */
UINT64 NtGdiCreateBitmap(UINT64 *a)
{
    return gdi_create_bitmap((INT32)a[0], (INT32)a[1]);
}

/* HBITMAP NtGdiSelectBitmap(HDC, HBITMAP): binds the memory DC to the bitmap's
 * pixels, which is what makes a memory DC drawable. */
UINT64 NtGdiSelectBitmap(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    W32K_OBJECT *bitmap = gdi_object(a[1], GDI_TYPE_BITMAP);
    if (!dc || !bitmap)
        return 0;
    UINT64 previous = dc->SelectedBitmap;
    dc->SelectedBitmap = a[1];
    dc->Surface = &bitmap->u.Bitmap.Surface;
    dc->OrgX = dc->OrgY = 0;
    dc->ClipX = dc->ClipY = 0;
    dc->ClipW = (INT32)bitmap->u.Bitmap.Surface.Width;
    dc->ClipH = (INT32)bitmap->u.Bitmap.Surface.Height;
    return previous;
}

UINT64 NtGdiSelectBrush(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !gdi_object(a[1], GDI_TYPE_BRUSH))
        return 0;
    UINT64 previous = dc->SelectedBrush;
    dc->SelectedBrush = a[1];
    return previous;
}

UINT64 NtGdiSelectPen(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !gdi_object(a[1], GDI_TYPE_PEN))
        return 0;
    UINT64 previous = dc->SelectedPen;
    dc->SelectedPen = a[1];
    return previous;
}

UINT64 NtGdiSelectFont(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    UINT64 previous = dc->SelectedFont;
    dc->SelectedFont = a[1];
    return previous;
}

/* HANDLE NtGdiGetDCObject(HDC, int type): GDI_OBJECT_TYPE_BRUSH == 0x10,
 * _PEN == 0x30, _FONT == 0x0A, _BITMAP == 0x05. */
UINT64 NtGdiGetDCObject(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    switch ((UINT32)a[1]) {
    case GDI_TYPE_BRUSH:  return dc->SelectedBrush;
    case GDI_TYPE_PEN:    return dc->SelectedPen;
    case GDI_TYPE_FONT:   return dc->SelectedFont;
    case GDI_TYPE_BITMAP: return dc->SelectedBitmap;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Services: drawing                                                   */
/* ------------------------------------------------------------------ */

/* The colour a fill uses: the selected brush, or the DC brush colour cached in
 * DC_ATTR when nothing has been selected. */
static UINT32 dc_brush_color(UINT64 hdc, W32K_DC *dc, BOOLEAN *hollow)
{
    W32K_OBJECT *brush = gdi_object(dc->SelectedBrush, GDI_TYPE_BRUSH);
    if (hollow)
        *hollow = brush ? brush->u.Brush.Hollow : FALSE;
    if (brush)
        return brush->u.Brush.Color;
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRBRUSH) : 0x00FFFFFF;
}

static UINT32 dc_pen_color(UINT64 hdc, W32K_DC *dc, BOOLEAN *hollow)
{
    W32K_OBJECT *pen = gdi_object(dc->SelectedPen, GDI_TYPE_PEN);
    if (hollow)
        *hollow = pen ? pen->u.Brush.Hollow : FALSE;
    if (pen)
        return pen->u.Brush.Color;
    UINT64 attr = dc_attr(hdc);
    return attr ? *(volatile UINT32 *)(attr + DCATTR_CRPEN) : 0;
}

/* BOOL NtGdiPatBlt(HDC, int x, int y, int w, int h, DWORD rop) */
UINT64 NtGdiPatBlt(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;
    UINT32 rop = (UINT32)a[5];
    BOOLEAN hollow = FALSE;
    UINT32 color;
    switch (rop) {
    case 0x00000042: color = 0x00000000; break;             /* BLACKNESS */
    case 0x00FF0062: color = 0x00FFFFFF; break;             /* WHITENESS */
    default:         color = dc_brush_color(a[0], dc, &hollow); break;
    }
    if (hollow)
        return 1;
    dc_fill(dc, (INT32)a[1], (INT32)a[2], (INT32)a[3], (INT32)a[4], color);
    return 1;
}

/* BOOL NtGdiBitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx,
 *                  int sy, DWORD rop, DWORD backColour, FLONG flags) */
UINT64 NtGdiBitBlt(UINT64 *a)
{
    W32K_DC *dst = dc_from_handle(a[0]);
    W32K_DC *src = dc_from_handle(a[5]);
    if (!dst || !dst->Surface)
        return 0;
    INT32 x = (INT32)a[1], y = (INT32)a[2];
    INT32 w = (INT32)a[3], h = (INT32)a[4];
    UINT32 rop = (UINT32)a[8];

    /* A blit with no source is a pattern fill; that is how a lot of client code
     * clears a rectangle. */
    if (!src || !src->Surface) {
        if (rop == 0x00000042 || rop == 0x00FF0062) {
            dc_fill(dst, x, y, w, h,
                    rop == 0x00000042 ? 0x00000000 : 0x00FFFFFF);
            return 1;
        }
        return 0;
    }

    INT32 sx = (INT32)a[6], sy = (INT32)a[7];
    for (INT32 row = 0; row < h; row++) {
        for (INT32 col = 0; col < w; col++) {
            INT32 source_x = src->OrgX + sx + col;
            INT32 source_y = src->OrgY + sy + row;
            if (source_x < 0 || source_y < 0 ||
                source_x >= (INT32)src->Surface->Width ||
                source_y >= (INT32)src->Surface->Height)
                continue;
            UINT32 pixel = src->Surface->Bits[(UINT32)source_y *
                                              src->Surface->Stride +
                                              (UINT32)source_x];
            INT32 dx = x + col, dy = y + row, dw = 1, dh = 1;
            if (!dc_clip(dst, &dx, &dy, &dw, &dh))
                continue;
            surface_pixel(dst->Surface, dx, dy, pixel);
        }
    }
    return 1;
}

UINT64 NtGdiSetPixel(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return (UINT64)-1;
    INT32 x = (INT32)a[1], y = (INT32)a[2], w = 1, h = 1;
    if (dc_clip(dc, &x, &y, &w, &h))
        surface_pixel(dc->Surface, x, y, gdi_pixel((UINT32)a[3]));
    return a[3];
}

UINT64 NtGdiMoveTo(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    INT32 *old = (INT32 *)a[3];
    if (old && MmProbeForWrite((UINT64)old, 8)) {
        old[0] = dc->CurX;
        old[1] = dc->CurY;
    }
    dc->CurX = (INT32)a[1];
    dc->CurY = (INT32)a[2];
    return 1;
}

/* Straight Bresenham in DC coordinates; each pixel is clipped individually. */
UINT64 NtGdiLineTo(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;
    BOOLEAN hollow = FALSE;
    UINT32 pixel = gdi_pixel(dc_pen_color(a[0], dc, &hollow));
    INT32 x0 = dc->CurX, y0 = dc->CurY;
    INT32 x1 = (INT32)a[1], y1 = (INT32)a[2];
    dc->CurX = x1;
    dc->CurY = y1;
    if (hollow)
        return 1;

    INT32 dx = x1 > x0 ? x1 - x0 : x0 - x1;
    INT32 dy = y1 > y0 ? y1 - y0 : y0 - y1;
    INT32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    INT32 err = dx - dy;
    for (;;) {
        INT32 px = x0, py = y0, pw = 1, ph = 1;
        if (dc_clip(dc, &px, &py, &pw, &ph))
            surface_pixel(dc->Surface, px, py, pixel);
        if (x0 == x1 && y0 == y1)
            break;
        INT32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
    return 1;
}

/* BOOL NtGdiRectangle(HDC, int left, int top, int right, int bottom):
 * brush-filled interior with a one-pixel pen border. */
UINT64 NtGdiRectangle(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;
    INT32 left = (INT32)a[1], top = (INT32)a[2];
    INT32 right = (INT32)a[3], bottom = (INT32)a[4];
    if (right < left || bottom < top)
        return 0;

    BOOLEAN brush_hollow = FALSE, pen_hollow = FALSE;
    UINT32 fill = dc_brush_color(a[0], dc, &brush_hollow);
    UINT32 edge = dc_pen_color(a[0], dc, &pen_hollow);
    if (!brush_hollow)
        dc_fill(dc, left, top, right - left, bottom - top, fill);
    if (!pen_hollow) {
        dc_fill(dc, left, top, right - left, 1, edge);
        dc_fill(dc, left, bottom - 1, right - left, 1, edge);
        dc_fill(dc, left, top, 1, bottom - top, edge);
        dc_fill(dc, right - 1, top, 1, bottom - top, edge);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Services: text                                                      */
/* ------------------------------------------------------------------ */

#define GDI_FONT_WIDTH  8
#define GDI_FONT_HEIGHT 16

/* BOOL NtGdiExtTextOutW(HDC, int x, int y, UINT flags, const RECT *clip,
 *                       const WCHAR *text, int count, const INT *dx,
 *                       DWORD codePage)
 *
 * ETO_OPAQUE == 0x0002 fills the background rectangle first; otherwise the
 * background mode in DC_ATTR decides, and TRANSPARENT is the common case. The
 * font is the kernel's 8x16 bitmap, so every glyph is a fixed cell. */
UINT64 NtGdiExtTextOutW(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc || !dc->Surface)
        return 0;

    INT32 x = (INT32)a[1], y = (INT32)a[2];
    UINT32 flags = (UINT32)a[3];
    const UINT16 *text = (const UINT16 *)a[5];
    INT32 count = (INT32)a[6];
    if (count < 0 || count > 4096)
        return 0;
    if (count && (!text || !MmProbeForRead((UINT64)text,
                                           (SIZE_T)count * sizeof(UINT16))))
        return 0;

    UINT32 fg = dc_text_color(a[0]);
    UINT32 bg = dc_bk_color(a[0]);
    BOOLEAN opaque = (flags & 0x0002) != 0;

    if (opaque)
        dc_fill(dc, x, y, count * GDI_FONT_WIDTH, GDI_FONT_HEIGHT, bg);

    for (INT32 i = 0; i < count; i++) {
        UINT16 wide = text[i];
        /* One bitmap font, ASCII cells: anything outside becomes a visible
         * placeholder rather than silently disappearing. */
        char c = (wide >= 32 && wide < 127) ? (char)wide : '?';
        dc_glyph(dc, x + i * GDI_FONT_WIDTH, y, c, fg, bg, opaque);
    }
    return 1;
}

/* BOOL NtGdiGetTextMetricsW(HDC, TMW_INTERNAL *out, ULONG size) */
UINT64 NtGdiGetTextMetricsW(UINT64 *a)
{
    if (!dc_from_handle(a[0]))
        return 0;
    UINT8 *out = (UINT8 *)a[1];
    UINT32 size = (UINT32)a[2];
    if (size > 0x100)
        size = 0x100;
    if (!out || size < 0x3C || !MmProbeForWrite((UINT64)out, size))
        return 0;
    memset(out, 0, size);

    INT32 *metrics = (INT32 *)out;
    metrics[0] = GDI_FONT_HEIGHT;      /* tmHeight            */
    metrics[1] = GDI_FONT_HEIGHT - 3;  /* tmAscent            */
    metrics[2] = 3;                    /* tmDescent           */
    metrics[3] = 0;                    /* tmInternalLeading   */
    metrics[4] = 0;                    /* tmExternalLeading   */
    metrics[5] = GDI_FONT_WIDTH;       /* tmAveCharWidth      */
    metrics[6] = GDI_FONT_WIDTH;       /* tmMaxCharWidth      */
    metrics[7] = 400;                  /* tmWeight (normal)   */
    metrics[8] = 0;                    /* tmOverhang          */
    metrics[9] = 96;                   /* tmDigitizedAspectX  */
    metrics[10] = 96;                  /* tmDigitizedAspectY  */
    *(UINT16 *)(out + 0x2C) = 32;      /* tmFirstChar         */
    *(UINT16 *)(out + 0x2E) = 126;     /* tmLastChar          */
    *(UINT16 *)(out + 0x30) = '?';     /* tmDefaultChar       */
    *(UINT16 *)(out + 0x32) = ' ';     /* tmBreakChar         */
    out[0x37] = 0x00;                  /* tmPitchAndFamily: fixed pitch */
    out[0x38] = 0x00;                  /* tmCharSet: ANSI_CHARSET */
    return 1;
}

static void gdi_text_size(INT32 count, INT32 *width, INT32 *height)
{
    *width = count * GDI_FONT_WIDTH;
    *height = GDI_FONT_HEIGHT;
}

/* BOOL NtGdiGetTextExtent(HDC, const WCHAR *, int count, SIZE *out, UINT) */
UINT64 NtGdiGetTextExtent(UINT64 *a)
{
    if (!dc_from_handle(a[0]))
        return 0;
    INT32 *out = (INT32 *)a[3];
    if (!out || !MmProbeForWrite((UINT64)out, 8))
        return 0;
    gdi_text_size((INT32)a[2], &out[0], &out[1]);
    return 1;
}

/* BOOL NtGdiGetTextExtentExW(HDC, const WCHAR *, ULONG count, ULONG maxExtent,
 *                           ULONG *fit, ULONG *dx, SIZE *size, FLONG) */
UINT64 NtGdiGetTextExtentExW(UINT64 *a)
{
    if (!dc_from_handle(a[0]))
        return 0;
    UINT32 count = (UINT32)a[2];
    if (count > 4096)
        return 0;
    UINT32 max_extent = (UINT32)a[3];
    UINT32 *fit = (UINT32 *)a[4];
    UINT32 *dx = (UINT32 *)a[5];
    INT32 *size = (INT32 *)a[6];

    if (dx && MmProbeForWrite((UINT64)dx, (SIZE_T)count * sizeof(UINT32)))
        for (UINT32 i = 0; i < count; i++)
            dx[i] = (i + 1) * GDI_FONT_WIDTH;
    if (fit && MmProbeForWrite((UINT64)fit, sizeof(UINT32))) {
        UINT32 possible = max_extent / GDI_FONT_WIDTH;
        *fit = possible < count ? possible : count;
    }
    if (size && MmProbeForWrite((UINT64)size, 8))
        gdi_text_size((INT32)count, &size[0], &size[1]);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Window device contexts                                              */
/* ------------------------------------------------------------------ */

/*
 * Windows keeps a small cache of device contexts and hands the same one back
 * for repeated GetDC calls on a window; ReleaseDC returns it to the cache
 * rather than destroying it. The same model here, for a concrete reason: our
 * ReleaseDC path (a NtUserCallOneParam routine we have not identified yet) does
 * not reach the kernel, so creating a fresh DC per GetDC would burn through the
 * handle table in a few hundred repaints.
 */
#define W32K_DC_CACHE 64

static UINT64 g_window_dc[W32K_DC_CACHE];

UINT64 W32kAcquireWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                           INT32 height)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (slot >= W32K_DC_CACHE)
        slot = 0;

    UINT64 cached = g_window_dc[slot];
    W32K_DC *dc = cached ? dc_from_handle(cached) : NULL;
    if (dc && dc->Hwnd == hwnd) {
        /* Refresh the geometry: the window may have been moved or resized
         * since the DC was first handed out. */
        dc->OrgX = x;
        dc->OrgY = y;
        dc->ClipX = x;
        dc->ClipY = y;
        dc->ClipW = width < 0 ? 0 : width;
        dc->ClipH = height < 0 ? 0 : height;
        return cached;
    }

    UINT64 hdc = W32kCreateWindowDc(hwnd, x, y, width, height);
    if (hdc)
        g_window_dc[slot] = hdc;
    return hdc;
}

void W32kDestroyWindowDc(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (slot >= W32K_DC_CACHE)
        return;
    if (g_window_dc[slot]) {
        W32kReleaseDc(g_window_dc[slot]);
        g_window_dc[slot] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Services: device capabilities                                       */
/* ------------------------------------------------------------------ */

/* GetDeviceCaps indices. */
#define DEVCAP_DRIVERVERSION 0
#define DEVCAP_TECHNOLOGY    2
#define DEVCAP_HORZSIZE      4
#define DEVCAP_VERTSIZE      6
#define DEVCAP_HORZRES       8
#define DEVCAP_VERTRES       10
#define DEVCAP_BITSPIXEL     12
#define DEVCAP_PLANES        14
#define DEVCAP_NUMBRUSHES    16
#define DEVCAP_NUMPENS       18
#define DEVCAP_NUMFONTS      22
#define DEVCAP_NUMCOLORS     24
#define DEVCAP_RASTERCAPS    38
#define DEVCAP_LOGPIXELSX    88
#define DEVCAP_LOGPIXELSY    90
#define DEVCAP_SIZEPALETTE   104
#define DEVCAP_COLORRES      108

/*
 * int NtGdiGetDeviceCaps(HDC, int index)
 *
 * gdi32!GetDeviceCaps only reaches this syscall when the DC's
 * DC_ATTR.ulDirty_ bit 0x10000 is clear; with it set it reads a cached
 * pGdiDevCaps blob instead. A freshly zeroed DC_ATTR takes the syscall path,
 * which is the one we can answer honestly from the framebuffer.
 *
 * LOGPIXELSY matters more than it looks: a program that asks for it to size a
 * font and gets 0 computes a zero-height font, fails to create it, and gives up
 * before showing a window.
 */
UINT64 NtGdiGetDeviceCaps(UINT64 *a)
{
    W32K_DC *dc = dc_from_handle(a[0]);
    if (!dc)
        return 0;
    W32K_SURFACE *surface = dc->Surface;
    INT32 width = surface ? (INT32)surface->Width : (INT32)GfxFramebuffer.Width;
    INT32 height = surface ? (INT32)surface->Height
                           : (INT32)GfxFramebuffer.Height;

    switch ((UINT32)a[1]) {
    case DEVCAP_DRIVERVERSION: return 0x4000;
    case DEVCAP_TECHNOLOGY:    return 1;      /* DT_RASDISPLAY */
    /* Physical size in millimetres at 96 DPI: 25.4 mm per inch. */
    case DEVCAP_HORZSIZE:      return (UINT64)(width * 254 / 960);
    case DEVCAP_VERTSIZE:      return (UINT64)(height * 254 / 960);
    case DEVCAP_HORZRES:       return (UINT64)width;
    case DEVCAP_VERTRES:       return (UINT64)height;
    case DEVCAP_BITSPIXEL:     return 32;
    case DEVCAP_PLANES:        return 1;
    case DEVCAP_NUMBRUSHES:    return (UINT64)-1;
    case DEVCAP_NUMPENS:       return (UINT64)-1;
    case DEVCAP_NUMFONTS:      return 1;
    case DEVCAP_NUMCOLORS:     return (UINT64)-1; /* more than 8 bpp */
    /* RC_BITBLT | RC_BITMAP64 | RC_GDI20_OUTPUT | RC_DI_BITMAP | RC_PALETTE
     * is what a plain 32-bpp raster display reports; leave out palette and
     * stretching we do not implement. */
    case DEVCAP_RASTERCAPS:    return 0x0001 | 0x0002 | 0x0010 | 0x0080;
    case DEVCAP_LOGPIXELSX:    return 96;
    case DEVCAP_LOGPIXELSY:    return 96;
    case DEVCAP_SIZEPALETTE:   return 0;
    case DEVCAP_COLORRES:      return 24;
    }
    return 0;
}

/* int NtGdiGetCurrentDpiInfo(HDC, DPI_INFO *out): the shape of DPI_INFO is not
 * public. Report failure rather than filling a structure we would be guessing
 * at; callers fall back to GetDeviceCaps(LOGPIXELS*). */
UINT64 NtGdiGetCurrentDpiInfo(UINT64 *a)
{
    return 0;
}
