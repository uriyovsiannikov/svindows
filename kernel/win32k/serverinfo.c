/*
 * win32k/serverinfo.c - the SERVERINFO shared page (USER32's `gpsi`).
 *
 * USER32 receives a pointer to this page during DLL_PROCESS_ATTACH, through
 * SHAREDINFO.psi in the connection block, and from then on answers
 * GetSystemMetrics, GetSysColor and GetSysColorBrush straight out of it with no
 * syscall at all. That is why a shell running on an empty (zeroed) SERVERINFO
 * believes the screen is 0x0 pixels: it never asks.
 *
 * Windows keeps the page writable only by win32k and read-only for the client,
 * and the supplied user32 confirms it -- it writes the `gpsi` pointer itself but
 * never writes through it. Same split here: filled while writable, then made
 * read-only for ring 3 and the kernel alike.
 *
 * SERVERINFO is not a stable structure across Windows versions, so the offsets
 * below were read out of the user32 build in win/ rather than assumed:
 *
 *   GetSystemMetrics    mov gpsi,%rdx; add $0x758,%rdx; mov (%rdx,%rbx,4),%eax
 *   GetSysColor         mov gpsi,%rax; mov 0x1360(%rax,%rdx,4),%eax
 *   GetSysColorBrush    mov gpsi,%rax; mov 0x13e0(%rax,%rcx,8),%rax
 *   HMValidateHandle    mov gpsi,%rax; ... 0x8(%rax)          (cHandleEntries)
 */
#include <ntos/win32k.h>
#include <ntos/ps.h>
#include <ntos/mm.h>
#include <ntos/gfx.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>

#define SI_CHANDLEENTRIES 0x0008
#define SI_AISYSMET       0x0758
#define SI_ARGBSYSTEM     0x1360
#define SI_AHBRSYSTEM     0x13E0

/* GetSystemMetrics rejects indices above 0x60 in this build. */
#define SM_COUNT 0x61
#define SYSCOLOR_COUNT 31

/* SM_* indices, in the order the array is laid out. */
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYVTHUMB 9
#define SM_CXHTHUMB 10
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_CYMENU 15
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_MOUSEPRESENT 19
#define SM_CYVSCROLL 20
#define SM_CXHSCROLL 21
#define SM_CXMIN 28
#define SM_CYMIN 29
#define SM_CXSIZE 30
#define SM_CYSIZE 31
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXMINTRACK 34
#define SM_CYMINTRACK 35
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SM_CXICONSPACING 38
#define SM_CYICONSPACING 39
#define SM_CMOUSEBUTTONS 43
#define SM_CXEDGE 45
#define SM_CYEDGE 46
#define SM_CXMINSPACING 47
#define SM_CYMINSPACING 48
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_CYSMCAPTION 51
#define SM_CXSMSIZE 52
#define SM_CYSMSIZE 53
#define SM_CXMENUSIZE 54
#define SM_CYMENUSIZE 55
#define SM_ARRANGE 56
#define SM_CXMINIMIZED 57
#define SM_CYMINIMIZED 58
#define SM_CXMAXTRACK 59
#define SM_CYMAXTRACK 60
#define SM_CXMAXIMIZED 61
#define SM_CYMAXIMIZED 62
#define SM_NETWORK 63
#define SM_CXDRAG 68
#define SM_CYDRAG 69
#define SM_CXMENUCHECK 71
#define SM_CYMENUCHECK 72
#define SM_MOUSEWHEELPRESENT 75
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80
#define SM_SAMEDISPLAYFORMAT 81
#define SM_CXFOCUSBORDER 83
#define SM_CYFOCUSBORDER 84

/* The stock Windows colour scheme, as COLORREF (0x00BBGGRR). Index order is
 * COLOR_SCROLLBAR(0) .. COLOR_MENUBAR(30). */
static const UINT32 g_sys_colors[SYSCOLOR_COUNT] = {
    0x00C8C8C8, /*  0 SCROLLBAR              */
    0x00000000, /*  1 BACKGROUND (desktop)   */
    0x00D1B499, /*  2 ACTIVECAPTION          */
    0x00DBCDBF, /*  3 INACTIVECAPTION        */
    0x00F0F0F0, /*  4 MENU                   */
    0x00FFFFFF, /*  5 WINDOW                 */
    0x00646464, /*  6 WINDOWFRAME            */
    0x00000000, /*  7 MENUTEXT               */
    0x00000000, /*  8 WINDOWTEXT             */
    0x00000000, /*  9 CAPTIONTEXT            */
    0x00B4B4B4, /* 10 ACTIVEBORDER           */
    0x00FCF7F4, /* 11 INACTIVEBORDER         */
    0x00ABABAB, /* 12 APPWORKSPACE           */
    0x00D77800, /* 13 HIGHLIGHT              */
    0x00FFFFFF, /* 14 HIGHLIGHTTEXT          */
    0x00F0F0F0, /* 15 BTNFACE                */
    0x00A0A0A0, /* 16 BTNSHADOW              */
    0x006D6D6D, /* 17 GRAYTEXT               */
    0x00000000, /* 18 BTNTEXT                */
    0x00000000, /* 19 INACTIVECAPTIONTEXT    */
    0x00FFFFFF, /* 20 BTNHIGHLIGHT           */
    0x00696969, /* 21 3DDKSHADOW             */
    0x00E3E3E3, /* 22 3DLIGHT                */
    0x00000000, /* 23 INFOTEXT               */
    0x00FFFFE1, /* 24 INFOBK                 */
    0x00000000, /* 25 (unused)               */
    0x00CC6600, /* 26 HOTLIGHT               */
    0x00EAD1B9, /* 27 GRADIENTACTIVECAPTION  */
    0x00F2E4D7, /* 28 GRADIENTINACTIVECAPTION*/
    0x00FF9933, /* 29 MENUHILIGHT            */
    0x00F0F0F0, /* 30 MENUBAR                */
};

void W32kInitializeServerInfo(void)
{
    for (UINT64 off = 0; off < PROCESS_SERVER_INFO_SIZE; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS ||
            !MmMapPage(PROCESS_SERVER_INFO_VA + off, pa,
                       PTE_USER | PTE_WRITE)) {
            KeLog("[w32k] no memory for the SERVERINFO page\n");
            return;
        }
        memset((void *)(PROCESS_SERVER_INFO_VA + off), 0, PAGE_SIZE);
    }

    UINT8 *si = (UINT8 *)PROCESS_SERVER_INFO_VA;
    *(UINT64 *)(si + SI_CHANDLEENTRIES) = 0x10000; /* the mapped table size */

    INT32 *met = (INT32 *)(si + SI_AISYSMET);
    INT32 width = GfxAvailable() ? (INT32)GfxFramebuffer.Width : 1024;
    INT32 height = GfxAvailable() ? (INT32)GfxFramebuffer.Height : 768;

    met[SM_CXSCREEN] = width;
    met[SM_CYSCREEN] = height;
    met[SM_CXVSCROLL] = 17;
    met[SM_CYHSCROLL] = 17;
    met[SM_CYCAPTION] = 23;
    met[SM_CXBORDER] = 1;
    met[SM_CYBORDER] = 1;
    met[SM_CXDLGFRAME] = 3;
    met[SM_CYDLGFRAME] = 3;
    met[SM_CYVTHUMB] = 17;
    met[SM_CXHTHUMB] = 17;
    met[SM_CXICON] = 32;
    met[SM_CYICON] = 32;
    met[SM_CXCURSOR] = 32;
    met[SM_CYCURSOR] = 32;
    met[SM_CYMENU] = 19;
    met[SM_CXFULLSCREEN] = width;
    met[SM_CYFULLSCREEN] = height - 23;
    met[SM_MOUSEPRESENT] = 1;
    met[SM_CYVSCROLL] = 17;
    met[SM_CXHSCROLL] = 17;
    met[SM_CXMIN] = 132;
    met[SM_CYMIN] = 39;
    met[SM_CXSIZE] = 18;
    met[SM_CYSIZE] = 22;
    met[SM_CXFRAME] = 4;
    met[SM_CYFRAME] = 4;
    met[SM_CXMINTRACK] = 132;
    met[SM_CYMINTRACK] = 39;
    met[SM_CXDOUBLECLK] = 4;
    met[SM_CYDOUBLECLK] = 4;
    met[SM_CXICONSPACING] = 75;
    met[SM_CYICONSPACING] = 75;
    met[SM_CMOUSEBUTTONS] = 2;
    met[SM_CXEDGE] = 2;
    met[SM_CYEDGE] = 2;
    met[SM_CXMINSPACING] = 160;
    met[SM_CYMINSPACING] = 24;
    met[SM_CXSMICON] = 16;
    met[SM_CYSMICON] = 16;
    met[SM_CYSMCAPTION] = 17;
    met[SM_CXSMSIZE] = 16;
    met[SM_CYSMSIZE] = 15;
    met[SM_CXMENUSIZE] = 19;
    met[SM_CYMENUSIZE] = 19;
    met[SM_ARRANGE] = 8;
    met[SM_CXMINIMIZED] = 160;
    met[SM_CYMINIMIZED] = 24;
    met[SM_CXMAXTRACK] = width + 12;
    met[SM_CYMAXTRACK] = height + 12;
    met[SM_CXMAXIMIZED] = width + 8;
    met[SM_CYMAXIMIZED] = height + 8;
    met[SM_NETWORK] = 3;
    met[SM_CXDRAG] = 4;
    met[SM_CYDRAG] = 4;
    met[SM_CXMENUCHECK] = 19;
    met[SM_CYMENUCHECK] = 19;
    met[SM_MOUSEWHEELPRESENT] = 1;
    met[SM_XVIRTUALSCREEN] = 0;
    met[SM_YVIRTUALSCREEN] = 0;
    met[SM_CXVIRTUALSCREEN] = width;
    met[SM_CYVIRTUALSCREEN] = height;
    met[SM_CMONITORS] = 1;
    met[SM_SAMEDISPLAYFORMAT] = 1;
    met[SM_CXFOCUSBORDER] = 1;
    met[SM_CYFOCUSBORDER] = 1;

    UINT32 *colors = (UINT32 *)(si + SI_ARGBSYSTEM);
    for (UINT32 i = 0; i < SYSCOLOR_COUNT; i++)
        colors[i] = g_sys_colors[i];
    /* ahbrSystem stays NULL: the system brushes are GDI objects and there is
     * no GDI handle table content yet. GetSysColorBrush answering NULL is a
     * documented failure; a fabricated handle would be dereferenced. */

    MmProtectRange(PROCESS_SERVER_INFO_VA, PROCESS_SERVER_INFO_SIZE, FALSE);
    KeLog("[w32k] SERVERINFO at 0x%lx: screen %ldx%ld, %u system colours\n",
          (unsigned long)PROCESS_SERVER_INFO_VA, (long)width, (long)height,
          SYSCOLOR_COUNT);
}
