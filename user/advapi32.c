/*
 * user/advapi32.c - the registry slice of advapi32.dll.
 *
 * Implements the classic Win32 Reg* API on top of the native Nt*Key services
 * exported by ntdll. Predefined roots (HKEY_LOCAL_MACHINE, ...) map to absolute
 * registry paths; real key handles returned by RegOpenKeyEx/RegCreateKeyEx are
 * passed straight through as the parent for relative opens.
 */

typedef void              *HANDLE;
typedef void              *HKEY;
typedef unsigned char      BYTE;
typedef unsigned long      DWORD;
typedef long               LONG;

#define ERROR_SUCCESS         0L
#define ERROR_FILE_NOT_FOUND  2L
#define ERROR_MORE_DATA       234L

#define HKEY_CLASSES_ROOT  ((HKEY)(unsigned long long)0x80000000ULL)
#define HKEY_CURRENT_USER  ((HKEY)(unsigned long long)0x80000001ULL)
#define HKEY_LOCAL_MACHINE ((HKEY)(unsigned long long)0x80000002ULL)
#define HKEY_USERS         ((HKEY)(unsigned long long)0x80000003ULL)

/* Parameter blocks passed to the set/query services (mirror ntos/cm.h). */
typedef struct { const char *Name; DWORD Type; DWORD Size; const void *Data; }
    CM_SET_VALUE;
typedef struct { const char *Name; DWORD *Type; DWORD *Size; void *Data; }
    CM_QUERY_VALUE;

/* Native registry services from ntdll. */
extern HANDLE NtCreateKey(HANDLE parent, const char *name);
extern HANDLE NtOpenKey(HANDLE parent, const char *name);
extern long   NtSetValueKey(HANDLE key, void *param);
extern long   NtQueryValueKey(HANDLE key, void *param);
extern long   NtClose(HANDLE h);

static int is_predefined(HKEY h)
{
    return ((unsigned long long)h & 0x80000000ULL) != 0;
}

static const char *root_prefix(HKEY h)
{
    switch ((unsigned long long)h) {
    case 0x80000002ULL: return "\\Registry\\Machine"; /* HKEY_LOCAL_MACHINE */
    case 0x80000001ULL: return "\\Registry\\User";    /* HKEY_CURRENT_USER  */
    case 0x80000003ULL: return "\\Registry\\User";    /* HKEY_USERS         */
    default:            return "\\Registry";
    }
}

/* dst = prefix + ("\\" + sub, if sub non-empty). */
static void path_join(char *dst, const char *prefix, const char *sub)
{
    int i = 0;
    while (*prefix)
        dst[i++] = *prefix++;
    if (sub && *sub) {
        dst[i++] = '\\';
        while (*sub)
            dst[i++] = *sub++;
    }
    dst[i] = 0;
}

/* Resolve (hKey, subKey) to a (parent handle, name) pair for the Nt services:
 * predefined roots become an absolute path opened from the root (parent 0),
 * real handles pass through with the relative name. */
static HANDLE reg_do(HKEY hKey, const char *subKey, int create, HKEY *out)
{
    HANDLE result;
    if (is_predefined(hKey)) {
        char path[256];
        path_join(path, root_prefix(hKey), subKey);
        result = create ? NtCreateKey((HANDLE)0, path) : NtOpenKey((HANDLE)0, path);
    } else {
        result = create ? NtCreateKey(hKey, subKey) : NtOpenKey(hKey, subKey);
    }
    *out = (HKEY)result;
    return result;
}

__declspec(dllexport) LONG RegOpenKeyExA(HKEY hKey, const char *subKey,
                                         DWORD options, DWORD sam, HKEY *result)
{
    (void)options; (void)sam;
    return reg_do(hKey, subKey, 0, result) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegCreateKeyExA(HKEY hKey, const char *subKey,
                                           DWORD res, char *cls, DWORD options,
                                           DWORD sam, void *sa, HKEY *result,
                                           DWORD *disposition)
{
    (void)res; (void)cls; (void)options; (void)sam; (void)sa;
    if (disposition)
        *disposition = 1; /* REG_CREATED_NEW_KEY (we don't distinguish) */
    return reg_do(hKey, subKey, 1, result) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegSetValueExA(HKEY hKey, const char *valueName,
                                          DWORD res, DWORD type,
                                          const BYTE *data, DWORD cbData)
{
    (void)res;
    CM_SET_VALUE p = { valueName, type, cbData, data };
    return NtSetValueKey((HANDLE)hKey, &p) == 0 ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegQueryValueExA(HKEY hKey, const char *valueName,
                                            DWORD *res, DWORD *type,
                                            BYTE *data, DWORD *cbData)
{
    (void)res;
    CM_QUERY_VALUE p = { valueName, type, cbData, data };
    return NtQueryValueKey((HANDLE)hKey, &p) == 0 ? ERROR_SUCCESS
                                                  : ERROR_FILE_NOT_FOUND;
}

__declspec(dllexport) LONG RegCloseKey(HKEY hKey)
{
    /* Predefined roots aren't real handles; closing them is a no-op. */
    if (is_predefined(hKey))
        return ERROR_SUCCESS;
    NtClose((HANDLE)hKey);
    return ERROR_SUCCESS;
}
