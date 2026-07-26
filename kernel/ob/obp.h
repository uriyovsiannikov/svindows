/*
 * ob/obp.h - Object Manager private definitions shared across the ob/ sources.
 */
#ifndef _OB_OBP_H_
#define _OB_OBP_H_

#include <ntos/ob.h>

/* Body of a Directory object: a list of the OBJECT_HEADERs it contains,
 * threaded through their NamespaceEntry field. */
typedef struct _OBJECT_DIRECTORY {
    LIST_ENTRY Entries;
} OBJECT_DIRECTORY;

/* The Directory object type, created during ObInitialize. */
extern POBJECT_TYPE ObpDirectoryType;

/* Create the Directory type and the root directory. */
void ObpInitializeNamespace(void);

#endif /* _OB_OBP_H_ */
