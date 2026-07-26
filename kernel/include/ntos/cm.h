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
 * KEY_VALUE_PARTIAL_INFORMATION - what NtQueryValueKey returns for info class
 * KeyValuePartialInformation (2): the value's type, its size, and its bytes.
 */
#define KeyValuePartialInformation 2

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    UINT32 TitleIndex;
    UINT32 Type;
    UINT32 DataLength;
    UINT8  Data[1];
} KEY_VALUE_PARTIAL_INFORMATION;

/* Bring up the registry: register the Key object type and build the initial
 * hive (\Registry\Machine\Software\NTOS with a couple of values). */
void CmInitialize(void);

/* Nt* registry services (take the syscall argument array). */
UINT64 NtCreateKey(UINT64 *args);
UINT64 NtOpenKey(UINT64 *args);
UINT64 NtSetValueKey(UINT64 *args);
UINT64 NtQueryValueKey(UINT64 *args);

#endif /* _NTOS_CM_H_ */
