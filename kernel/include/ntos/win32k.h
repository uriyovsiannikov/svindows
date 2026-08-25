/*
 * ntos/win32k.h - the win32k side of the USER/GDI subsystem.
 *
 * Two pieces of shared memory and a GDI object table are what make the genuine
 * user32/gdi32 work against this kernel:
 *
 *  - SERVERINFO ("gpsi"): read-only, reached through SHAREDINFO.psi. USER32
 *    answers GetSystemMetrics and GetSysColor out of it with no syscall.
 *  - The GDI handle table at PEB->GdiSharedHandleTable: one 24-byte GDICELL64
 *    per object. GDI32 validates every handle against it before use.
 *  - Per-object attribute blocks (DC_ATTR): user-writable, pointed at by a
 *    cell's pUserInfo. GDI32 caches DC state there, so SetTextColor and
 *    SetBkColor never enter the kernel and win32k has to read the current
 *    colours back out of the block when it draws.
 */
#ifndef _NTOS_WIN32K_H_
#define _NTOS_WIN32K_H_

#include <nt/ntdef.h>

/*
 * Build and publish the SERVERINFO page at PROCESS_SERVER_INFO_VA. Requires Mm
 * and the framebuffer (screen metrics come from it), and must run before the
 * user process starts, because USER32 reads the page during its process
 * attach.
 */
void W32kInitializeServerInfo(void);

/* Bring up the GDI object table. Requires the GDI shared handle table and the
 * attribute arena to be mapped (PsCreateUserProcess does that). */
void W32kInitializeGdi(void);

/*
 * Create a device context that draws to the screen inside a window's
 * rectangle, and return its GDI handle (0 on failure). Drawing is clipped to
 * the rectangle and coordinates are relative to it, which is what a client
 * DC's coordinate space means.
 */
UINT64 W32kCreateWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                          INT32 height);

/* Release a device context created for a window. */
void W32kReleaseDc(UINT64 hdc);

/* Hand out the cached device context for a window, creating it on first use and
 * refreshing its geometry on every call. */
UINT64 W32kAcquireWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                           INT32 height);

/* Drop a window's cached device context (on window destruction). */
void W32kDestroyWindowDc(UINT64 hwnd);

/* The win32k system services, dispatched from KiServiceTable. */
UINT64 NtGdiGetEntry(UINT64 *args);
UINT64 NtGdiCreateSolidBrush(UINT64 *args);
UINT64 NtGdiSelectBrush(UINT64 *args);
UINT64 NtGdiSelectFont(UINT64 *args);
UINT64 NtGdiSelectPen(UINT64 *args);
UINT64 NtGdiSelectBitmap(UINT64 *args);
UINT64 NtGdiDeleteObjectApp(UINT64 *args);
UINT64 NtGdiPatBlt(UINT64 *args);
UINT64 NtGdiBitBlt(UINT64 *args);
UINT64 NtGdiExtTextOutW(UINT64 *args);
UINT64 NtGdiGetTextMetricsW(UINT64 *args);
UINT64 NtGdiGetTextExtent(UINT64 *args);
UINT64 NtGdiGetTextExtentExW(UINT64 *args);
UINT64 NtGdiSetPixel(UINT64 *args);
UINT64 NtGdiRectangle(UINT64 *args);
UINT64 NtGdiMoveTo(UINT64 *args);
UINT64 NtGdiLineTo(UINT64 *args);
UINT64 NtGdiCreateCompatibleDC(UINT64 *args);
UINT64 NtGdiCreateCompatibleBitmap(UINT64 *args);
UINT64 NtGdiCreateBitmap(UINT64 *args);
UINT64 NtGdiGetDCObject(UINT64 *args);
UINT64 NtGdiFlush(UINT64 *args);
UINT64 NtGdiHfontCreate(UINT64 *args);
UINT64 NtGdiCreateRectRgn(UINT64 *args);
UINT64 NtGdiCreatePen(UINT64 *args);
UINT64 NtGdiGetDeviceCaps(UINT64 *args);
UINT64 NtGdiGetCurrentDpiInfo(UINT64 *args);

#endif /* _NTOS_WIN32K_H_ */
