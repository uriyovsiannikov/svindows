/* Minimal ws2_32.dll surface for small Windows console utilities. */
typedef unsigned short WCHAR;
typedef unsigned long  DWORD;

/* Hostname.exe imports WSAStartup by its legacy ordinal 115.  A .def file
 * supplies that ordinal while preserving the normal named export. */
int WSAStartup(unsigned short version, void *data)
{
    (void)version;
    /* The caller only checks the return status; zero means success. */
    if (data) {
        unsigned char *p = (unsigned char *)data;
        for (unsigned i = 0; i < 400; i++) p[i] = 0;
    }
    return 0;
}

__declspec(dllexport) int GetHostNameW(WCHAR *name, int count)
{
    static const WCHAR host[] = { 'N','T','O','S',0 };
    int i = 0;
    if (!name || count <= 0)
        return -1;
    while (host[i] && i + 1 < count) {
        name[i] = host[i];
        i++;
    }
    name[i] = 0;
    return host[i] ? -1 : 0;
}
