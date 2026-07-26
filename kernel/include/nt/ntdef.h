/*
 * nt/ntdef.h - Fundamental NT data types.
 *
 * These follow the public Windows NT type conventions so that the native API
 * and PE loader we build later see a familiar ABI. Everything here is
 * freestanding: no host headers are pulled in.
 */
#ifndef _NT_NTDEF_H_
#define _NT_NTDEF_H_

/* ------------------------------------------------------------------ */
/* Fixed-width integer base types                                     */
/* ------------------------------------------------------------------ */

typedef signed char        INT8, *PINT8;
typedef signed short       INT16, *PINT16;
typedef signed int         INT32, *PINT32;
typedef signed long long   INT64, *PINT64;

typedef unsigned char      UINT8, *PUINT8;
typedef unsigned short     UINT16, *PUINT16;
typedef unsigned int       UINT32, *PUINT32;
typedef unsigned long long UINT64, *PUINT64;

/* Classic Windows spellings ---------------------------------------- */

typedef void               VOID;
typedef void              *PVOID;
typedef void              *PVOID64;

typedef char               CHAR, *PCHAR, *PSTR;
typedef const char        *PCSTR, *LPCSTR;
typedef unsigned char      UCHAR, *PUCHAR;
typedef unsigned char      BYTE, *PBYTE;

typedef short              SHORT, *PSHORT;
typedef unsigned short     USHORT, *PUSHORT;
typedef unsigned short     WORD, *PWORD;

typedef int                INT, *PINT;
typedef unsigned int       UINT, *PUINT;
typedef int                LONG, *PLONG;        /* NT LONG is 32-bit */
typedef unsigned int       ULONG, *PULONG;      /* NT ULONG is 32-bit */
typedef unsigned int       DWORD, *PDWORD;

typedef long long          LONGLONG, *PLONGLONG;
typedef unsigned long long ULONGLONG, *PULONGLONG;
typedef unsigned long long QWORD, *PQWORD;

/* NOTE: Win32/NT define LONG/ULONG/DWORD as 32-bit even on 64-bit targets.
 * The host toolchain is LP64 (`long` is 64-bit), so these are typedef'd to
 * `int`/`unsigned int` to preserve the NT width of exactly 32 bits. Anything
 * that must be pointer-sized uses the *_PTR forms below. */
_Static_assert(sizeof(ULONG) == 4, "NT ULONG must be 32-bit");
_Static_assert(sizeof(LONG) == 4, "NT LONG must be 32-bit");

typedef __UINTPTR_TYPE__   ULONG_PTR, *PULONG_PTR;
typedef __INTPTR_TYPE__    LONG_PTR, *PLONG_PTR;
typedef ULONG_PTR          SIZE_T, *PSIZE_T;
typedef LONG_PTR           SSIZE_T, *PSSIZE_T;
typedef ULONG_PTR          DWORD_PTR, *PDWORD_PTR;

typedef __UINTPTR_TYPE__   UINT_PTR;
typedef __INTPTR_TYPE__    INT_PTR;

/* Booleans --------------------------------------------------------- */

typedef UCHAR              BOOLEAN, *PBOOLEAN;
typedef int                BOOL, *PBOOL;

/* Handles and access ----------------------------------------------- */

typedef void              *HANDLE, **PHANDLE;
typedef ULONG              ACCESS_MASK, *PACCESS_MASK;

/* Generic access rights (subset). */
#define DELETE                 0x00010000u
#define READ_CONTROL           0x00020000u
#define SYNCHRONIZE            0x00100000u
#define STANDARD_RIGHTS_ALL    0x001F0000u
#define GENERIC_READ           0x80000000u
#define GENERIC_WRITE          0x40000000u
#define GENERIC_EXECUTE        0x20000000u
#define GENERIC_ALL            0x10000000u

/* ------------------------------------------------------------------ */
/* Status                                                             */
/* ------------------------------------------------------------------ */

typedef LONG               NTSTATUS, *PNTSTATUS;

/* ------------------------------------------------------------------ */
/* Constants and helper macros                                        */
/* ------------------------------------------------------------------ */

#ifndef NULL
#define NULL ((void *)0)
#endif

#define TRUE   1
#define FALSE  0

/* SAL-lite annotations: no-ops, present only for documentation. */
#define IN
#define OUT
#define OPTIONAL
#define UNREFERENCED_PARAMETER(P) ((void)(P))

/* Microsoft x64 calling convention for the native ABI boundary.
 * Internal kernel code uses the toolchain-default SysV ABI. */
#if defined(__GNUC__) || defined(__clang__)
#define NTAPI      __attribute__((ms_abi))
#define NORETURN   __attribute__((noreturn))
#define PACKED     __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define NTAPI
#define NORETURN
#define PACKED
#define ALIGNED(n)
#define ALWAYS_INLINE inline
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Field/containing-record helpers used pervasively by NT. */
#define FIELD_OFFSET(type, field) ((LONG)(ULONG_PTR) & (((type *)0)->field))
#define CONTAINING_RECORD(address, type, field) \
    ((type *)((PCHAR)(address) - (ULONG_PTR)(&((type *)0)->field)))

/* Alignment helpers. */
#define ALIGN_DOWN_BY(x, a) ((ULONG_PTR)(x) & ~((ULONG_PTR)(a) - 1))
#define ALIGN_UP_BY(x, a)   ALIGN_DOWN_BY((ULONG_PTR)(x) + ((ULONG_PTR)(a) - 1), a)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ------------------------------------------------------------------ */
/* Common NT structures                                               */
/* ------------------------------------------------------------------ */

/*
 * LIST_ENTRY - the ubiquitous NT circular doubly linked list node. The list is
 * anchored by a sentinel LIST_ENTRY whose Flink/Blink point at the first/last
 * real entries; an empty list has both pointing back at the sentinel.
 */
typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

/* Single linked list node. */
typedef struct _SINGLE_LIST_ENTRY {
    struct _SINGLE_LIST_ENTRY *Next;
} SINGLE_LIST_ENTRY, *PSINGLE_LIST_ENTRY;

/* Counted UTF-16 string (NT strings are counted, not NUL-terminated). */
typedef struct _UNICODE_STRING {
    USHORT Length;         /* bytes, not counting a terminator */
    USHORT MaximumLength;  /* capacity of Buffer in bytes */
    UINT16 *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

/* Counted ANSI string. */
typedef struct _ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} ANSI_STRING, *PANSI_STRING, STRING, *PSTRING;

/* 64-bit value that can also be viewed as two 32-bit halves. */
typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        ULONG LowPart;
        ULONG HighPart;
    } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

#endif /* _NT_NTDEF_H_ */
