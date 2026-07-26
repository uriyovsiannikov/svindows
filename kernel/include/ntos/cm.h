/*
 * ntos/cm.h - the Configuration Manager (Cm): the registry.
 *
 * A hierarchical key/value store held in memory. Keys form a tree rooted at
 * \Registry (with \Registry\Machine and \Registry\User beneath it); each key
 * carries named, typed values. Keys are exposed to ring 3 as Key objects (Ob
 * handles), so NtClose closes a key just like any other handle.
 */
#ifndef _NTOS_CM_H_
#define _NTOS_CM_H_

#include <nt/ntdef.h>
#include <nt/ntstatus.h>

/* Value data types (a subset of the Windows REG_* set). */
#define REG_NONE   0
#define REG_SZ     1
#define REG_BINARY 3
#define REG_DWORD  4

/*
 * Parameter blocks for the set/query services, passed by pointer (the syscall
 * path marshals only four arguments). advapi32 mirrors these layouts.
 */
typedef struct _CM_SET_VALUE {
    const char *Name;  /* value name (ASCII)          */
    UINT32      Type;  /* REG_*                       */
    UINT32      Size;  /* data size in bytes          */
    const void *Data;  /* data to store               */
} CM_SET_VALUE;

typedef struct _CM_QUERY_VALUE {
    const char *Name;  /* value name to look up       */
    UINT32     *Type;  /* out: REG_* (may be NULL)    */
    UINT32     *Size;  /* in: buffer size; out: data size (may be NULL for size query) */
    void       *Data;  /* out: buffer to fill (may be NULL) */
} CM_QUERY_VALUE;

/* Bring up the registry: register the Key object type and build the initial
 * hive (\Registry\Machine\Software\NTOS with a couple of values). */
void CmInitialize(void);

/* Nt* registry services (signatures match the syscall dispatcher). */
UINT64 NtCreateKey(UINT64 parent_handle, UINT64 name_ptr, UINT64 a3, UINT64 a4);
UINT64 NtOpenKey(UINT64 parent_handle, UINT64 name_ptr, UINT64 a3, UINT64 a4);
UINT64 NtSetValueKey(UINT64 handle, UINT64 param_ptr, UINT64 a3, UINT64 a4);
UINT64 NtQueryValueKey(UINT64 handle, UINT64 param_ptr, UINT64 a3, UINT64 a4);

#endif /* _NTOS_CM_H_ */
