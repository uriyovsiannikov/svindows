/*
 * nt/peb.h - the process and thread environment blocks.
 *
 * On Windows x64 the GS segment base in user mode points at the current
 * thread's TEB; `gs:[0x30]` is the TEB self-pointer and `gs:[0x60]` is the PEB.
 * We reproduce the same layout so native code can find its environment exactly
 * where it expects it. Only the leading, widely-relied-upon fields are defined.
 */
#ifndef _NT_PEB_H_
#define _NT_PEB_H_

#include <nt/ntdef.h>

/* Thread Information Block - the head of the TEB (offsets 0x00..0x38). */
typedef struct _NT_TIB {
    PVOID ExceptionList;         /* 0x00 */
    PVOID StackBase;             /* 0x08 */
    PVOID StackLimit;            /* 0x10 */
    PVOID SubSystemTib;          /* 0x18 */
    PVOID FiberData;             /* 0x20 */
    PVOID ArbitraryUserPointer;  /* 0x28 */
    struct _NT_TIB *Self;        /* 0x30 */
} NT_TIB;

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID;

/* Thread Environment Block (leading fields; reached via GS in user mode). */
typedef struct _TEB {
    NT_TIB    NtTib;                      /* 0x00 */
    PVOID     EnvironmentPointer;         /* 0x38 */
    CLIENT_ID ClientId;                   /* 0x40 */
    PVOID     ActiveRpcHandle;            /* 0x50 */
    PVOID     ThreadLocalStoragePointer;  /* 0x58 */
    PVOID     ProcessEnvironmentBlock;    /* 0x60 -> PEB */
    ULONG     LastErrorValue;             /* 0x68 */
    ULONG     CountOfOwnedCriticalSections;/* 0x6C */
} TEB, *PTEB;

/* Loader data (module list), referenced by PEB.Ldr. */
typedef struct _PEB_LDR_DATA {
    ULONG      Length;
    UINT8      Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

/*
 * One loaded module. Windows' loader threads these onto the three PEB_LDR_DATA
 * lists; kernel32's GetModuleHandle/GetProcAddress walk the load-order list.
 * Field offsets match the Windows x64 layout so native code can rely on them.
 */
typedef struct _LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY     InLoadOrderLinks;           /* 0x00 */
    LIST_ENTRY     InMemoryOrderLinks;         /* 0x10 */
    LIST_ENTRY     InInitializationOrderLinks; /* 0x20 */
    PVOID          DllBase;                    /* 0x30 */
    PVOID          EntryPoint;                 /* 0x38 */
    ULONG          SizeOfImage;                /* 0x40 */
    UNICODE_STRING FullDllName;                /* 0x48 */
    UNICODE_STRING BaseDllName;                /* 0x58 */
    ULONG          Flags;                      /* 0x68 */
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

/* Process Environment Block (leading fields; reached via TEB[0x60]). */
typedef struct PACKED _PEB {
    UINT8  InheritedAddressSpace;      /* 0x00 */
    UINT8  ReadImageFileExecOptions;   /* 0x01 */
    UINT8  BeingDebugged;              /* 0x02 */
    UINT8  BitField;                   /* 0x03 */
    UINT8  Padding0[4];                /* 0x04 */
    PVOID  Mutant;                     /* 0x08 */
    PVOID  ImageBaseAddress;           /* 0x10 */
    PVOID  Ldr;                        /* 0x18 -> PEB_LDR_DATA */
    PVOID  ProcessParameters;          /* 0x20 */
    PVOID  SubSystemData;              /* 0x28 */
    PVOID  ProcessHeap;                /* 0x30 */
} PEB, *PPEB;

#endif /* _NT_PEB_H_ */
