/*
 * user/extra.c - a standalone DLL loaded at runtime (extra.dll).
 *
 * Not linked into the app's import chain: testapp loads it with LoadLibraryA
 * and resolves its exports with GetProcAddress, exactly the way a Windows
 * program loads a plugin. Self-contained (no imports of its own).
 */

typedef unsigned long DWORD;

__declspec(dllexport) DWORD ExtraAddNumbers(DWORD a, DWORD b)
{
    return a + b;
}

__declspec(dllexport) const char *ExtraGreeting(void)
{
    return "  extra.dll says: hello from a DLL loaded at runtime!\n";
}
