/*
 * cm/registry.c - the Configuration Manager (Cm): an in-memory registry.
 *
 * Keys form a tree rooted at an anonymous super-root whose single child is
 * "Registry" (so absolute paths read "\Registry\Machine\..."). Each key holds a
 * list of named, typed values. Keys are handed to ring 3 as Key objects (Ob
 * handles): NtCreateKey/NtOpenKey return a handle, NtSetValueKey/NtQueryValueKey
 * operate through it, and NtClose (Ob) releases it. The key tree itself persists
 * independently of handles.
 *
 * Simplifications vs. real NT: names are ASCII, there is no on-disk hive (the
 * store is built at boot and lives in memory), and user pointers are trusted.
 */
#include <ntos/cm.h>
#include <ntos/ob.h>
#include <ntos/ex.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>

typedef struct _CM_VALUE {
    LIST_ENTRY Link;
    char       Name[64];
    UINT32     Type;
    UINT32     Size;
    UINT8     *Data;
} CM_VALUE;

typedef struct _CM_KEY {
    char       Name[64];
    struct _CM_KEY *Parent;
    LIST_ENTRY SubkeyHead;  /* list of child CM_KEYs   */
    LIST_ENTRY SubkeyLink;  /* link in Parent->SubkeyHead */
    LIST_ENTRY ValueHead;   /* list of CM_VALUEs       */
} CM_KEY;

/* The Ob object handed to ring 3 just points at a persistent CM_KEY. */
typedef struct _CM_KEY_OBJECT {
    CM_KEY *Key;
} CM_KEY_OBJECT;

static POBJECT_TYPE g_key_type;
static CM_KEY      *g_cm_root; /* anonymous super-root */

/* ------------------------------------------------------------------ */
/* Tree primitives                                                    */
/* ------------------------------------------------------------------ */

static int ci_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
        if (!ca)
            return 1;
    }
}

static CM_KEY *cm_alloc_key(const char *name, CM_KEY *parent)
{
    CM_KEY *k = ExAllocatePoolWithTag(NonPagedPool, sizeof(CM_KEY), 'yeKC');
    if (!k)
        return NULL;
    memset(k, 0, sizeof(*k));
    SIZE_T i = 0;
    for (; name[i] && i < sizeof(k->Name) - 1; i++)
        k->Name[i] = name[i];
    k->Name[i] = 0;
    k->Parent = parent;
    InitializeListHead(&k->SubkeyHead);
    InitializeListHead(&k->ValueHead);
    if (parent)
        InsertTailList(&parent->SubkeyHead, &k->SubkeyLink);
    return k;
}

static CM_KEY *cm_find_subkey(CM_KEY *parent, const char *name)
{
    for (PLIST_ENTRY e = parent->SubkeyHead.Flink; e != &parent->SubkeyHead;
         e = e->Flink) {
        CM_KEY *k = CONTAINING_RECORD(e, CM_KEY, SubkeyLink);
        if (ci_eq(k->Name, name))
            return k;
    }
    return NULL;
}

/* Walk a (possibly multi-component, '\'-separated) path from `start`, creating
 * missing keys when `create` is set. Empty components are skipped, so a leading
 * '\' and the absolute "\Registry\..." form both work. */
static CM_KEY *cm_walk(CM_KEY *start, const char *path, BOOLEAN create)
{
    CM_KEY *cur = start;
    const char *p = path;

    while (*p) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;

        char comp[64];
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '\\' && *p != '/') /* skip an over-long component tail */
            p++;

        CM_KEY *child = cm_find_subkey(cur, comp);
        if (!child) {
            if (!create)
                return NULL;
            child = cm_alloc_key(comp, cur);
            if (!child)
                return NULL;
        }
        cur = child;
    }
    return cur;
}

static CM_VALUE *cm_find_value(CM_KEY *k, const char *name)
{
    for (PLIST_ENTRY e = k->ValueHead.Flink; e != &k->ValueHead; e = e->Flink) {
        CM_VALUE *v = CONTAINING_RECORD(e, CM_VALUE, Link);
        if (ci_eq(v->Name, name))
            return v;
    }
    return NULL;
}

static NTSTATUS cm_set_value(CM_KEY *k, const char *name, UINT32 type,
                             const void *data, UINT32 size)
{
    UINT8 *copy = NULL;
    if (size) {
        copy = ExAllocatePoolWithTag(NonPagedPool, size, 'laVC');
        if (!copy)
            return STATUS_NO_MEMORY;
        memcpy(copy, data, size);
    }

    CM_VALUE *v = cm_find_value(k, name);
    if (v) {
        if (v->Data)
            ExFreePool(v->Data);
    } else {
        v = ExAllocatePoolWithTag(NonPagedPool, sizeof(CM_VALUE), 'laVC');
        if (!v) {
            if (copy)
                ExFreePool(copy);
            return STATUS_NO_MEMORY;
        }
        memset(v, 0, sizeof(*v));
        SIZE_T i = 0;
        for (; name[i] && i < sizeof(v->Name) - 1; i++)
            v->Name[i] = name[i];
        v->Name[i] = 0;
        InsertTailList(&k->ValueHead, &v->Link);
    }
    v->Type = type;
    v->Size = size;
    v->Data = copy;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Key objects / handles                                              */
/* ------------------------------------------------------------------ */

static void CmpKeyDelete(POBJECT object)
{
    (void)object; /* the CM_KEY persists in the tree; nothing to free here */
}

static UINT64 cm_make_handle(CM_KEY *k)
{
    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_key_type, sizeof(CM_KEY_OBJECT), &obj)))
        return 0;
    ((CM_KEY_OBJECT *)obj)->Key = k;

    HANDLE h;
    NTSTATUS st = ObCreateHandle(obj, GENERIC_ALL, &h);
    ObDereferenceObject(obj); /* the handle keeps the object alive */
    if (!NT_SUCCESS(st))
        return 0;
    return (UINT64)(ULONG_PTR)h;
}

/* Resolve a key handle to its CM_KEY. Handle 0 means the root (so absolute
 * paths can be opened without first opening a root key). */
static CM_KEY *cm_key_from_handle(UINT64 handle)
{
    if (handle == 0)
        return g_cm_root;
    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0,
                                              g_key_type, &obj)))
        return NULL;
    CM_KEY *k = ((CM_KEY_OBJECT *)obj)->Key;
    ObDereferenceObject(obj);
    return k;
}

/* ------------------------------------------------------------------ */
/* Nt* services                                                       */
/* ------------------------------------------------------------------ */

UINT64 NtCreateKey(UINT64 parent_handle, UINT64 name_ptr, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    CM_KEY *parent = cm_key_from_handle(parent_handle);
    if (!parent || name_ptr == 0)
        return 0;
    CM_KEY *k = cm_walk(parent, (const char *)name_ptr, TRUE);
    if (!k)
        return 0;
    UINT64 h = cm_make_handle(k);
    KeLog("[cm]   NtCreateKey('%s') -> handle %p\n", (const char *)name_ptr,
          (void *)h);
    return h;
}

UINT64 NtOpenKey(UINT64 parent_handle, UINT64 name_ptr, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    CM_KEY *parent = cm_key_from_handle(parent_handle);
    if (!parent || name_ptr == 0)
        return 0;
    CM_KEY *k = cm_walk(parent, (const char *)name_ptr, FALSE);
    if (!k) {
        KeLog("[cm]   NtOpenKey('%s') -> not found\n", (const char *)name_ptr);
        return 0;
    }
    UINT64 h = cm_make_handle(k);
    KeLog("[cm]   NtOpenKey('%s') -> handle %p\n", (const char *)name_ptr,
          (void *)h);
    return h;
}

UINT64 NtSetValueKey(UINT64 handle, UINT64 param_ptr, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    CM_KEY *k = cm_key_from_handle(handle);
    if (!k || param_ptr == 0)
        return (UINT64)STATUS_INVALID_HANDLE;
    CM_SET_VALUE *p = (CM_SET_VALUE *)param_ptr;
    NTSTATUS st = cm_set_value(k, p->Name, p->Type, p->Data, p->Size);
    KeLog("[cm]   NtSetValueKey('%s', type=%u, %u bytes) -> 0x%08x\n",
          p->Name, (unsigned)p->Type, (unsigned)p->Size, (unsigned)st);
    return (UINT64)st;
}

UINT64 NtQueryValueKey(UINT64 handle, UINT64 param_ptr, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    CM_KEY *k = cm_key_from_handle(handle);
    if (!k || param_ptr == 0)
        return (UINT64)STATUS_INVALID_HANDLE;
    CM_QUERY_VALUE *p = (CM_QUERY_VALUE *)param_ptr;

    CM_VALUE *v = cm_find_value(k, p->Name);
    if (!v)
        return (UINT64)STATUS_OBJECT_NAME_NOT_FOUND;

    if (p->Type)
        *p->Type = v->Type;
    if (p->Data && p->Size) {
        UINT32 copy = (*p->Size < v->Size) ? *p->Size : v->Size;
        memcpy(p->Data, v->Data, copy);
    }
    if (p->Size)
        *p->Size = v->Size;
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

void CmInitialize(void)
{
    g_key_type = ObCreateObjectType("Key", CmpKeyDelete);

    /* Anonymous super-root whose child "Registry" anchors absolute paths. */
    g_cm_root = cm_alloc_key("", NULL);

    /* Build \Registry\Machine\Software\NTOS with a couple of preset values. */
    CM_KEY *ntos = cm_walk(g_cm_root, "\\Registry\\Machine\\Software\\NTOS", TRUE);
    if (ntos) {
        cm_set_value(ntos, "Version", REG_SZ, "0.6.0", 6);
        UINT32 build = 600;
        cm_set_value(ntos, "BuildNumber", REG_DWORD, &build, sizeof(build));
    }
    /* An empty \Registry\User for HKEY_CURRENT_USER to hang off of. */
    cm_walk(g_cm_root, "\\Registry\\User", TRUE);

    KeLog("[cm]   registry ready: Key type + \\Registry hive\n");
}
