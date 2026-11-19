/*
 * PROJECT:     Tavera OS System Libraries
 * LICENSE:     GNU Lesser General Public License, version 2.1 or later (LGPL-2.1-or-later)
 * FILE:        dll/win32/kernel32/kernel32_vista/CreateFileTransacted.c
 * PURPOSE:     CreateFileTransacted with process-local staging (TxF API)
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 *
 * Sources:
 *   https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiletransactedw
 *   https://learn.microsoft.com/en-us/windows/win32/fileio/how-to-use-transactional-ntfs
 *   tavera/specs/ntfs/ntfs-txf.md (probe-v10 NTFS-TXF-PROBE-DONE ok).
 * Staging via ktmw32 KtmRegisterTransactedFile — no invented $RmMetadata layouts.
 */

#include <k32.h>

typedef BOOL (WINAPI *PFN_KTM_REGISTER)(HANDLE, LPCWSTR, LPCWSTR);

static PFN_KTM_REGISTER
TxfGetRegister(void)
{
    static PFN_KTM_REGISTER Register;
    static BOOL Tried;

    if (!Tried)
    {
        HMODULE Mod = LoadLibraryW(L"ktmw32.dll");
        Tried = TRUE;
        if (Mod != NULL)
            Register = (PFN_KTM_REGISTER)GetProcAddress(Mod, "KtmRegisterTransactedFile");
    }
    return Register;
}

static void
TxfBuildStagingPath(LPCWSTR FinalPath, LPWSTR StagingPath, SIZE_T Cap)
{
    WCHAR Dir[MAX_PATH];
    WCHAR Name[MAX_PATH];
    LPCWSTR Slash;
    SIZE_T DirLen;
    DWORD Tick;

    Slash = wcsrchr(FinalPath, L'\\');
    if (Slash == NULL)
        Slash = wcsrchr(FinalPath, L'/');
    if (Slash != NULL)
    {
        DirLen = (SIZE_T)(Slash - FinalPath + 1);
        if (DirLen >= Cap)
            DirLen = Cap - 1;
        RtlCopyMemory(Dir, FinalPath, DirLen * sizeof(WCHAR));
        Dir[DirLen] = 0;
        wcsncpy(Name, Slash + 1, MAX_PATH - 1);
        Name[MAX_PATH - 1] = 0;
    }
    else
    {
        Dir[0] = 0;
        wcsncpy(Name, FinalPath, MAX_PATH - 1);
        Name[MAX_PATH - 1] = 0;
    }

    Tick = GetTickCount();
    _snwprintf(StagingPath, (unsigned)Cap, L"%s.txf_%08lx_%s", Dir, (unsigned long)Tick, Name);
    StagingPath[Cap - 1] = 0;
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreateFileTransactedW(LPCWSTR lpFileName,
                      DWORD dwDesiredAccess,
                      DWORD dwShareMode,
                      LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                      DWORD dwCreationDisposition,
                      DWORD dwFlagsAndAttributes,
                      HANDLE hTemplateFile,
                      HANDLE hTransaction,
                      PUSHORT pusMiniVersion,
                      PVOID lpExtendedParameter)
{
    WCHAR Staging[MAX_PATH];
    HANDLE File;
    PFN_KTM_REGISTER Register;

    UNREFERENCED_PARAMETER(pusMiniVersion);
    UNREFERENCED_PARAMETER(lpExtendedParameter);

    if (lpFileName == NULL || hTransaction == NULL || hTransaction == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    Register = TxfGetRegister();
    if (Register == NULL)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE_VALUE;
    }

    TxfBuildStagingPath(lpFileName, Staging, MAX_PATH);
    File = CreateFileW(Staging,
                       dwDesiredAccess,
                       dwShareMode,
                       lpSecurityAttributes,
                       dwCreationDisposition,
                       dwFlagsAndAttributes | FILE_ATTRIBUTE_HIDDEN,
                       hTemplateFile);
    if (File == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    if (!Register(hTransaction, lpFileName, Staging))
    {
        DWORD Err = GetLastError();
        CloseHandle(File);
        DeleteFileW(Staging);
        SetLastError(Err);
        return INVALID_HANDLE_VALUE;
    }

    return File;
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreateFileTransactedA(LPCSTR lpFileName,
                      DWORD dwDesiredAccess,
                      DWORD dwShareMode,
                      LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                      DWORD dwCreationDisposition,
                      DWORD dwFlagsAndAttributes,
                      HANDLE hTemplateFile,
                      HANDLE hTransaction,
                      PUSHORT pusMiniVersion,
                      PVOID lpExtendedParameter)
{
    WCHAR Wide[MAX_PATH];
    int n;

    if (lpFileName == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    n = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, Wide, MAX_PATH);
    if (n <= 0)
        return INVALID_HANDLE_VALUE;
    return CreateFileTransactedW(Wide,
                                 dwDesiredAccess,
                                 dwShareMode,
                                 lpSecurityAttributes,
                                 dwCreationDisposition,
                                 dwFlagsAndAttributes,
                                 hTemplateFile,
                                 hTransaction,
                                 pusMiniVersion,
                                 lpExtendedParameter);
}
