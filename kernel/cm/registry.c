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
#include <ntos/mm.h>
#include <ntos/rtl.h>
#include <nt/ntobject.h>

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

/* Resolve OBJECT_ATTRIBUTES (RootDirectory + captured name) to a target key.
 * Returns STATUS on failure; on success writes *out. */
static NTSTATUS cm_resolve_oa(POBJECT_ATTRIBUTES oa, BOOLEAN create,
                              CM_KEY **out)
{
    if (!MmProbeForRead((UINT64)oa, sizeof(OBJECT_ATTRIBUTES)))
        return STATUS_ACCESS_VIOLATION;

    /* RootDirectory (a key handle) anchors a relative name; NULL means the
     * name is absolute from the registry root. */
    CM_KEY *parent = cm_key_from_handle((UINT64)oa->RootDirectory);
    if (!parent)
        return STATUS_INVALID_HANDLE;

    char name[128];
    if (!MmCaptureUnicodeName((UINT64)oa->ObjectName, name, sizeof(name)))
        return STATUS_ACCESS_VIOLATION;

    CM_KEY *k = cm_walk(parent, name, create);
    if (!k) {
        KeLog("[cm]   key %s%s not found\n",
              create ? "" : "(open) ", name);
        return create ? STATUS_NO_MEMORY : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *out = k;
    return STATUS_SUCCESS;
}

/* NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, TitleIndex, Class,
 *             CreateOptions, PULONG Disposition). */
UINT64 NtCreateKey(UINT64 *a)
{
    PHANDLE out_handle = (PHANDLE)a[0];
    UINT32 *disp = (UINT32 *)a[6];
    if (!MmProbeForWrite((UINT64)out_handle, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    CM_KEY *k;
    NTSTATUS st = cm_resolve_oa((POBJECT_ATTRIBUTES)a[2], TRUE, &k);
    if (!NT_SUCCESS(st))
        return (UINT64)st;

    UINT64 h = cm_make_handle(k);
    *out_handle = (HANDLE)(ULONG_PTR)h;
    if (disp && MmProbeForWrite((UINT64)disp, sizeof(UINT32)))
        *disp = 1; /* REG_CREATED_NEW_KEY (we don't distinguish) */
    KeLog("[cm]   NtCreateKey -> handle %p\n", (void *)h);
    return (UINT64)STATUS_SUCCESS;
}

/* NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES). */
UINT64 NtOpenKey(UINT64 *a)
{
    PHANDLE out_handle = (PHANDLE)a[0];
    if (!MmProbeForWrite((UINT64)out_handle, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    CM_KEY *k;
    NTSTATUS st = cm_resolve_oa((POBJECT_ATTRIBUTES)a[2], FALSE, &k);
    if (!NT_SUCCESS(st))
        return (UINT64)st;

    UINT64 h = cm_make_handle(k);
    *out_handle = (HANDLE)(ULONG_PTR)h;
    KeLog("[cm]   NtOpenKey -> handle %p\n", (void *)h);
    return (UINT64)STATUS_SUCCESS;
}

/* NtSetValueKey(HANDLE, PUNICODE_STRING ValueName, TitleIndex, Type, Data,
 *               DataSize). */
UINT64 NtSetValueKey(UINT64 *a)
{
    CM_KEY *k = cm_key_from_handle(a[0]);
    if (!k)
        return (UINT64)STATUS_INVALID_HANDLE;

    char vname[64];
    if (!MmCaptureUnicodeName(a[1], vname, sizeof(vname)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT32 type = (UINT32)a[3];
    UINT64 data = a[4];
    UINT32 size = (UINT32)a[5];
    if (size && !MmProbeForRead(data, size))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    NTSTATUS st = cm_set_value(k, vname, type, (const void *)data, size);
    KeLog("[cm]   NtSetValueKey('%s', type=%u, %u bytes) -> 0x%08x\n",
          vname, (unsigned)type, (unsigned)size, (unsigned)st);
    return (UINT64)st;
}

/* NtQueryValueKey(HANDLE, PUNICODE_STRING ValueName, InfoClass, Info, Length,
 *                 PULONG ResultLength) - returns KEY_VALUE_PARTIAL_INFORMATION. */
UINT64 NtQueryValueKey(UINT64 *a)
{
    CM_KEY *k = cm_key_from_handle(a[0]);
    if (!k)
        return (UINT64)STATUS_INVALID_HANDLE;

    char vname[64];
    if (!MmCaptureUnicodeName(a[1], vname, sizeof(vname)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT64 info = a[3];
    UINT32 length = (UINT32)a[4];
    UINT32 *result_len = (UINT32 *)a[5];

    CM_VALUE *v = cm_find_value(k, vname);
    if (!v)
        return (UINT64)STATUS_OBJECT_NAME_NOT_FOUND;

    UINT32 needed = (UINT32)(sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1 + v->Size);
    if (result_len && MmProbeForWrite((UINT64)result_len, sizeof(UINT32)))
        *result_len = needed;

    if (info == 0 || length < needed) {
        if (!info || length < sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1)
            return (UINT64)STATUS_BUFFER_TOO_SMALL;
    }
    if (!MmProbeForWrite(info, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    KEY_VALUE_PARTIAL_INFORMATION *kvpi = (KEY_VALUE_PARTIAL_INFORMATION *)info;
    kvpi->TitleIndex = 0;
    kvpi->Type = v->Type;
    kvpi->DataLength = v->Size;
    UINT32 avail = length - (UINT32)(sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1);
    UINT32 copy = (avail < v->Size) ? avail : v->Size;
    memcpy(kvpi->Data, v->Data, copy);

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

    /* Winlogon's list of registered shell images.  Explorer refuses to run as
     * the shell unless its own image name appears under this key, so seed it
     * with the inbox shell.  The value data is UTF-16 (REG_SZ). */
    CM_KEY *shells = cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"
        "\\Winlogon\\AlternateShells\\AvailableShells", TRUE);
    if (shells) {
        static const UINT16 shell_image[] = {
            'e', 0, 'x', 0, 'p', 0, 'l', 0, 'o', 0, 'r', 0, 'e', 0,
            'r', 0, '.', 0, 'e', 0, 'x', 0, 'e', 0, 0, 0
        };
        cm_set_value(shells, "Shell", REG_SZ, shell_image,
                     sizeof(shell_image));
    }

    /* Explorer's shell startup reads the accent color chosen during OOBE and
     * treats a failed read as a fatal initialization error. */
    CM_KEY *accent = cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion"
        "\\Explorer\\Accent", TRUE);
    if (accent) {
        UINT32 oobe_color_set = 4;
        cm_set_value(accent, "OOBEColorSet", REG_DWORD, &oobe_color_set,
                     sizeof(oobe_color_set));
    }

    /* Keys the shell and its supporting DLLs probe during startup.  Missing
     * keys are legal on Windows, but seeding the ones with fixed semantics
     * keeps the boot path deterministic. */
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"
        "\\Winlogon", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows\\Dwm", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Direct2D", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\OLE", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Policies\\Microsoft\\Windows"
        "\\Personalization", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion"
        "\\Policies\\Explorer", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion"
        "\\Explorer\\FolderDescriptions", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion"
        "\\Explorer\\Shell Folders", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\System\\Setup", TRUE);
    cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion"
        "\\Explorer\\WCDEn", TRUE);

    CM_KEY *user_software = cm_walk(g_cm_root,
        "\\Registry\\User\\Software\\Microsoft\\Windows\\CurrentVersion",
        TRUE);
    if (user_software) {
        cm_walk(user_software, "Explorer", TRUE);
        cm_walk(user_software, "Search", TRUE);
        cm_walk(user_software, "Policies", TRUE);
        cm_walk(user_software, "Explorer\\User Shell Folders", TRUE);
        cm_walk(user_software, "Explorer\\Shell Folders", TRUE);
        cm_walk(user_software,
                "Explorer\\Desktop\\NameSpace", TRUE);

        /* Standard per-user shell folder redirections (REG_EXPAND_SZ). The
         * shell resolves known folders through these before falling back to
         * its internal defaults. */
        CM_KEY *usf = cm_walk(user_software,
                              "Explorer\\User Shell Folders", TRUE);
        if (usf) {
            static const UINT16 desktop[] = {
                '%',0,'U',0,'S',0,'E',0,'R',0,'P',0,'R',0,'O',0,'F',0,'I',0,
                'L',0,'E',0,'%',0,'\\',0,'D',0,'e',0,'s',0,'k',0,'t',0,'o',
                0,'p',0,0,0
            };
            static const UINT16 personal[] = {
                '%',0,'U',0,'S',0,'E',0,'R',0,'P',0,'R',0,'O',0,'F',0,'I',0,
                'L',0,'E',0,'%',0,'\\',0,'D',0,'o',0,'c',0,'u',0,'m',0,'e',
                0,'n',0,'t',0,'s',0,0,0
            };
            static const UINT16 appdata[] = {
                '%',0,'U',0,'S',0,'E',0,'R',0,'P',0,'R',0,'O',0,'F',0,'I',0,
                'L',0,'E',0,'%',0,'\\',0,'A',0,'p',0,'p',0,'D',0,'a',0,'t',
                0,'a',0,'\\',0,'R',0,'o',0,'a',0,'m',0,'i',0,'n',0,'g',0,0,0
            };
            static const UINT16 start_menu[] = {
                '%',0,'U',0,'S',0,'E',0,'R',0,'P',0,'R',0,'O',0,'F',0,'I',0,
                'L',0,'E',0,'%',0,'\\',0,'A',0,'p',0,'p',0,'D',0,'a',0,'t',
                0,'a',0,'\\',0,'R',0,'o',0,'a',0,'m',0,'i',0,'n',0,'g',0,
                '\\',0,'M',0,'i',0,'c',0,'r',0,'o',0,'s',0,'o',0,'f',0,'t',
                0,'\\',0,'W',0,'i',0,'n',0,'d',0,'o',0,'w',0,'s',0,'\\',0,
                'S',0,'t',0,'a',0,'r',0,'t',0,' ',0,'M',0,'e',0,'n',0,'u',0,0,0
            };
            cm_set_value(usf, "Desktop", REG_EXPAND_SZ, desktop,
                         sizeof(desktop));
            cm_set_value(usf, "Personal", REG_EXPAND_SZ, personal,
                         sizeof(personal));
            cm_set_value(usf, "AppData", REG_EXPAND_SZ, appdata,
                         sizeof(appdata));
            cm_set_value(usf, "Start Menu", REG_EXPAND_SZ, start_menu,
                         sizeof(start_menu));
        }
    }

    /* The profile list backs %USERPROFILE%-style expansion during known
     * folder resolution. */
    CM_KEY *profile_list = cm_walk(g_cm_root,
        "\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"
        "\\ProfileList", TRUE);
    if (profile_list) {
        static const UINT16 profiles_dir[] = {
            '%',0,'S',0,'y',0,'s',0,'t',0,'e',0,'m',0,'D',0,'r',0,'i',0,
            'v',0,'e',0,'%',0,'\\',0,'U',0,'s',0,'e',0,'r',0,'s',0,0,0
        };
        cm_set_value(profile_list, "ProfilesDirectory", REG_EXPAND_SZ,
                     profiles_dir, sizeof(profiles_dir));
    }

    KeLog("[cm]   registry ready: Key type + \\Registry hive\n");
}
