/*
 * user/advapi32.c - the registry slice of advapi32.dll.
 *
 * Implements the classic Win32 Reg* API on top of the native Nt*Key services,
 * building the real OBJECT_ATTRIBUTES / UNICODE_STRING / KEY_VALUE_PARTIAL_-
 * INFORMATION structures exactly as advapi32 does on Windows. Predefined roots
 * (HKEY_LOCAL_MACHINE, ...) map to absolute registry paths opened from the root;
 * a real key handle passes through as the OBJECT_ATTRIBUTES RootDirectory.
 */

typedef void              *HANDLE;
typedef void              *HKEY;
typedef unsigned char      BYTE;
typedef unsigned short     WCHAR;
typedef unsigned long      DWORD;
typedef long               LONG;
typedef long               NTSTATUS;

#define ERROR_SUCCESS         0L
#define ERROR_FILE_NOT_FOUND  2L
#define KEY_READ              0x20019
#define NT_SUCCESS(s)         ((NTSTATUS)(s) >= 0)
#define KeyValuePartialInformation 2

#define HKEY_CURRENT_USER  ((HKEY)(unsigned long long)0x80000001ULL)
#define HKEY_LOCAL_MACHINE ((HKEY)(unsigned long long)0x80000002ULL)
#define HKEY_USERS         ((HKEY)(unsigned long long)0x80000003ULL)

typedef struct _UNICODE_STRING { WCHAR Length, MaximumLength; WCHAR *Buffer; }
    UNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    DWORD           Length;
    HANDLE          RootDirectory;
    UNICODE_STRING *ObjectName;
    DWORD           Attributes;
    void           *SecurityDescriptor;
    void           *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    DWORD TitleIndex;
    DWORD Type;
    DWORD DataLength;
    BYTE  Data[1];
} KEY_VALUE_PARTIAL_INFORMATION;

/* Native registry services (real signatures). */
extern NTSTATUS NtOpenKey(HANDLE *KeyHandle, DWORD DesiredAccess,
                          OBJECT_ATTRIBUTES *ObjectAttributes);
extern NTSTATUS NtCreateKey(HANDLE *KeyHandle, DWORD DesiredAccess,
                            OBJECT_ATTRIBUTES *ObjectAttributes, DWORD TitleIndex,
                            void *Class, DWORD CreateOptions, DWORD *Disposition);
extern NTSTATUS NtSetValueKey(HANDLE KeyHandle, UNICODE_STRING *ValueName,
                              DWORD TitleIndex, DWORD Type, const void *Data,
                              DWORD DataSize);
extern NTSTATUS NtQueryValueKey(HANDLE KeyHandle, UNICODE_STRING *ValueName,
                                DWORD InfoClass, void *Info, DWORD Length,
                                DWORD *ResultLength);
extern long     NtClose(HANDLE h);

static int is_predefined(HKEY h)
{
    return ((unsigned long long)h & 0x80000000ULL) != 0;
}

static const char *root_prefix(HKEY h)
{
    switch ((unsigned long long)h) {
    case 0x80000002ULL: return "\\Registry\\Machine";
    case 0x80000001ULL: return "\\Registry\\User";
    case 0x80000003ULL: return "\\Registry\\User";
    default:            return "\\Registry";
    }
}

/* Widen an ASCII string into `w`, returning its length in characters. */
static int widen(WCHAR *w, const char *s, int cap)
{
    int n = 0;
    while (*s && n < cap - 1)
        w[n++] = (WCHAR)(unsigned char)*s++;
    w[n] = 0;
    return n;
}

static void init_ustr(UNICODE_STRING *u, WCHAR *buf, int chars)
{
    u->Length = (WCHAR)(chars * 2);
    u->MaximumLength = (WCHAR)(chars * 2 + 2);
    u->Buffer = buf;
}

static void init_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *name, HANDLE root)
{
    oa->Length = sizeof(*oa);
    oa->RootDirectory = root;
    oa->ObjectName = name;
    oa->Attributes = 0;
    oa->SecurityDescriptor = 0;
    oa->SecurityQualityOfService = 0;
}

/* Build the (name, RootDirectory) pair for a Reg* call: a predefined root
 * becomes an absolute path opened from the registry root; a real handle passes
 * through with the relative subkey. */
static void build_key_name(HKEY hKey, const char *subKey, WCHAR *wbuf,
                           UNICODE_STRING *name, HANDLE *root)
{
    if (is_predefined(hKey)) {
        char path[256];
        int i = 0;
        const char *pre = root_prefix(hKey);
        while (*pre)
            path[i++] = *pre++;
        if (subKey && *subKey) {
            path[i++] = '\\';
            while (*subKey)
                path[i++] = *subKey++;
        }
        path[i] = 0;
        init_ustr(name, wbuf, widen(wbuf, path, 256));
        *root = 0;
    } else {
        init_ustr(name, wbuf, widen(wbuf, subKey ? subKey : "", 256));
        *root = (HANDLE)hKey;
    }
}

__declspec(dllexport) LONG RegOpenKeyExA(HKEY hKey, const char *subKey,
                                         DWORD options, DWORD sam, HKEY *result)
{
    (void)options; (void)sam;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtOpenKey(&out, KEY_READ, &oa);
    *result = (HKEY)out;
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegCreateKeyExA(HKEY hKey, const char *subKey,
                                           DWORD res, char *cls, DWORD options,
                                           DWORD sam, void *sa, HKEY *result,
                                           DWORD *disposition)
{
    (void)res; (void)cls; (void)options; (void)sam; (void)sa;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name(hKey, subKey, wbuf, &name, &root);
    init_oa(&oa, &name, root);
    NTSTATUS st = NtCreateKey(&out, KEY_READ, &oa, 0, 0, 0, disposition);
    *result = (HKEY)out;
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegSetValueExA(HKEY hKey, const char *valueName,
                                          DWORD res, DWORD type,
                                          const BYTE *data, DWORD cbData)
{
    (void)res;
    WCHAR wbuf[128];
    UNICODE_STRING name;
    init_ustr(&name, wbuf, widen(wbuf, valueName ? valueName : "", 128));
    NTSTATUS st = NtSetValueKey((HANDLE)hKey, &name, 0, type, data, cbData);
    return NT_SUCCESS(st) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegQueryValueExA(HKEY hKey, const char *valueName,
                                            DWORD *res, DWORD *type,
                                            BYTE *data, DWORD *cbData)
{
    (void)res;
    WCHAR wbuf[128];
    UNICODE_STRING name;
    init_ustr(&name, wbuf, widen(wbuf, valueName ? valueName : "", 128));

    /* KEY_VALUE_PARTIAL_INFORMATION header + up to a page of data. */
    BYTE buf[512];
    DWORD resultLen = 0;
    NTSTATUS st = NtQueryValueKey((HANDLE)hKey, &name, KeyValuePartialInformation,
                                  buf, sizeof(buf), &resultLen);
    if (!NT_SUCCESS(st))
        return ERROR_FILE_NOT_FOUND;

    KEY_VALUE_PARTIAL_INFORMATION *kvpi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    if (type)
        *type = kvpi->Type;
    if (data && cbData) {
        DWORD copy = *cbData < kvpi->DataLength ? *cbData : kvpi->DataLength;
        for (DWORD i = 0; i < copy; i++)
            data[i] = kvpi->Data[i];
    }
    if (cbData)
        *cbData = kvpi->DataLength;
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegCloseKey(HKEY hKey)
{
    if (is_predefined(hKey))
        return ERROR_SUCCESS;
    NtClose((HANDLE)hKey);
    return ERROR_SUCCESS;
}
