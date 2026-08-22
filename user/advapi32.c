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
    /* Callers pass sign-extended handle-sized values (0xFFFFFFFF8xxxxxxx);
     * normalize to the 32-bit predefined-root code before matching. */
    unsigned long long v = (unsigned long long)h;
    if (v & 0xFFFFFFFF00000000ULL)
        v &= 0xFFFFFFFFULL;
    switch (v) {
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

static int wide_len(const WCHAR *s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void build_key_name_w(HKEY hKey, const WCHAR *subKey, WCHAR *wbuf,
                             UNICODE_STRING *name, HANDLE *root)
{
    int n = 0;
    if (is_predefined(hKey)) {
        const char *pre = root_prefix(hKey);
        while (*pre && n < 255)
            wbuf[n++] = (WCHAR)(unsigned char)*pre++;
        if (subKey && *subKey && n < 255)
            wbuf[n++] = '\\';
        while (subKey && *subKey && n < 255)
            wbuf[n++] = *subKey++;
        *root = 0;
    } else {
        while (subKey && *subKey && n < 255)
            wbuf[n++] = *subKey++;
        *root = (HANDLE)hKey;
    }
    wbuf[n] = 0;
    init_ustr(name, wbuf, n);
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

__declspec(dllexport) LONG RegOpenKeyExW(HKEY hKey, const WCHAR *subKey,
                                         DWORD options, DWORD sam, HKEY *result)
{
    (void)options; (void)sam;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name_w(hKey, subKey, wbuf, &name, &root);
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

__declspec(dllexport) LONG RegCreateKeyExW(HKEY hKey, const WCHAR *subKey,
                                           DWORD res, WCHAR *cls, DWORD options,
                                           DWORD sam, void *sa, HKEY *result,
                                           DWORD *disposition)
{
    (void)res; (void)cls; (void)options; (void)sam; (void)sa;
    WCHAR wbuf[256];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE root, out = 0;
    build_key_name_w(hKey, subKey, wbuf, &name, &root);
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

__declspec(dllexport) LONG RegSetValueExW(HKEY hKey, const WCHAR *valueName,
                                          DWORD res, DWORD type,
                                          const BYTE *data, DWORD cbData)
{
    (void)res;
    UNICODE_STRING name;
    init_ustr(&name, (WCHAR *)(valueName ? valueName : L""),
              wide_len(valueName));
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

__declspec(dllexport) LONG RegQueryValueExW(HKEY hKey, const WCHAR *valueName,
                                            DWORD *res, DWORD *type,
                                            BYTE *data, DWORD *cbData)
{
    (void)res;
    UNICODE_STRING name;
    init_ustr(&name, (WCHAR *)(valueName ? valueName : L""),
              wide_len(valueName));

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

/* ------------------------------------------------------------------ */
/* Tokens and SIDs                                                     */
/* ------------------------------------------------------------------ */

typedef int BOOL_K;
#define TRUE_K  1
#define FALSE_K 0

extern NTSTATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress,
                                        unsigned long long ZeroBits,
                                        unsigned long long *RegionSize,
                                        DWORD AllocationType, DWORD Protect);

static void *sid_alloc(DWORD bytes)
{
    void *base = 0;
    unsigned long long size = bytes;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)-1ULL, &base, 0, &size,
                                          0x3000 /* MEM_COMMIT|MEM_RESERVE */,
                                          0x04 /* PAGE_READWRITE */);
    return NT_SUCCESS(st) ? base : 0;
}

__declspec(dllexport) BOOL_K AllocateAndInitializeSid(
    const BYTE *identifier_authority, BYTE sub_authority_count,
    DWORD s0, DWORD s1, DWORD s2, DWORD s3, DWORD s4, DWORD s5, DWORD s6,
    DWORD s7, void **sid)
{
    if (!identifier_authority || !sub_authority_count ||
        sub_authority_count > 8 || !sid)
        return FALSE_K;
    BYTE *raw = sid_alloc(8 + 4ull * sub_authority_count);
    if (!raw)
        return FALSE_K;
    raw[0] = 1; /* revision */
    raw[1] = sub_authority_count;
    for (int i = 0; i < 6; i++)
        raw[2 + i] = identifier_authority[i];
    DWORD *authorities = (DWORD *)(raw + 8);
    authorities[0] = s0;
    if (sub_authority_count > 1) authorities[1] = s1;
    if (sub_authority_count > 2) authorities[2] = s2;
    if (sub_authority_count > 3) authorities[3] = s3;
    if (sub_authority_count > 4) authorities[4] = s4;
    if (sub_authority_count > 5) authorities[5] = s5;
    if (sub_authority_count > 6) authorities[6] = s6;
    if (sub_authority_count > 7) authorities[7] = s7;
    *sid = raw;
    return TRUE_K;
}

__declspec(dllexport) void *FreeSid(void *sid)
{
    (void)sid;
    return 0;
}

__declspec(dllexport) BOOL_K CheckTokenMembership(void *token, void *sid,
                                                  BOOL_K *is_member)
{
    (void)token;
    (void)sid;
    if (!is_member)
        return FALSE_K;
    *is_member = TRUE_K;
    return TRUE_K;
}

__declspec(dllexport) BOOL_K OpenProcessToken(HANDLE process,
                                              DWORD desired_access,
                                              HANDLE *token_handle)
{
    (void)process;
    (void)desired_access;
    if (!token_handle)
        return FALSE_K;
    *token_handle = (HANDLE)0x0000000100000002ULL;
    return TRUE_K;
}

__declspec(dllexport) BOOL_K OpenThreadToken(HANDLE thread,
                                             DWORD desired_access,
                                             BOOL_K open_as_self,
                                             HANDLE *token_handle)
{
    (void)thread;
    (void)desired_access;
    (void)open_as_self;
    if (!token_handle)
        return FALSE_K;
    *token_handle = (HANDLE)0x0000000100000003ULL;
    return TRUE_K;
}

__declspec(dllexport) LONG RegOpenCurrentUser(DWORD desired_access, HKEY *key)
{
    (void)desired_access;
    if (!key)
        return ERROR_FILE_NOT_FOUND;
    *key = HKEY_CURRENT_USER;
    return ERROR_SUCCESS;
}

__declspec(dllexport) LONG RegCreateKeyW(HKEY hKey, const WCHAR *subKey,
                                         HKEY *result)
{
    return RegCreateKeyExW(hKey, subKey, 0, 0, 0, 0, 0, result, 0);
}

__declspec(dllexport) LONG RegQueryValueW(HKEY hKey, const WCHAR *subKey,
                                          void *data, LONG *data_size)
{
    DWORD type = 0;
    DWORD size = data_size ? (DWORD)*data_size : 0;
    LONG status = RegQueryValueExW(hKey, subKey, 0, &type, data,
                                   data_size ? &size : 0);
    if (data_size)
        *data_size = (LONG)size;
    return status;
}

__declspec(dllexport) LONG RegGetValueW(HKEY hKey, const WCHAR *subKey,
                                        const WCHAR *value, DWORD flags,
                                        DWORD *type, void *data,
                                        DWORD *data_size)
{
    (void)flags;
    HKEY opened = hKey;
    LONG status = ERROR_SUCCESS;
    if (subKey && subKey[0])
        status = RegOpenKeyExW(hKey, subKey, 0, KEY_READ, &opened);
    if (status != ERROR_SUCCESS)
        return status;
    status = RegQueryValueExW(opened, value, 0, type, data, data_size);
    if (opened != hKey)
        NtClose(opened);
    return status;
}
