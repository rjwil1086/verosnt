/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/cmdutils/fsutil/quota.c
 * PURPOSE:     fsutil quota track/modify via FileFsControlInformation and FSCTL_SET_QUOTA
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Sources: fsutil quota —
 *   https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-quota
 *          FileFsControlInformation / FILE_VC_QUOTA_TRACK —
 *   https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/e5a70738-7ee4-46d9-a5f7-6644daa49a51
 *          FILE_FS_CONTROL_INFORMATION —
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_fs_control_information
 *          modify via NtSetQuotaInformationFile (not local FSCTL 0x900) —
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetquotainformationfile
 *   MS-FSCC 2.4.41; ntfs-quota-on-disk.md Rev.4 / NTFS-QUOTA-005.
 *          SeIncreaseQuotaPrivilege for NtSetQuotaInformationFile —
 *   https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants
 */

#include "fsutil.h"
#include <winioctl.h>

#ifndef FileFsControlInformation
#define FileFsControlInformation 6
#endif

#ifndef FILE_VC_QUOTA_TRACK
#define FILE_VC_QUOTA_NONE     0x00000000
#define FILE_VC_QUOTA_TRACK    0x00000001
#define FILE_VC_QUOTA_ENFORCE  0x00000002
#define FILE_VC_QUOTA_MASK     0x00000003
#endif

#ifndef SE_INCREASE_QUOTA_PRIVILEGE
#define SE_INCREASE_QUOTA_PRIVILEGE 5
#endif

typedef struct _FSUTIL_FS_CONTROL_INFORMATION {
    LARGE_INTEGER FreeSpaceStartFiltering;
    LARGE_INTEGER FreeSpaceThreshold;
    LARGE_INTEGER FreeSpaceStopFiltering;
    LARGE_INTEGER DefaultQuotaThreshold;
    LARGE_INTEGER DefaultQuotaLimit;
    ULONG FileSystemControlFlags;
} FSUTIL_FS_CONTROL_INFORMATION;

typedef struct _FSUTIL_IO_STATUS_BLOCK {
    union {
        LONG Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} FSUTIL_IO_STATUS_BLOCK;

typedef LONG (NTAPI *PFN_NT_QUERY_VOLUME)(HANDLE, FSUTIL_IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
typedef LONG (NTAPI *PFN_NT_SET_VOLUME)(HANDLE, FSUTIL_IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
typedef LONG (NTAPI *PFN_NT_SET_QUOTA)(HANDLE, FSUTIL_IO_STATUS_BLOCK *, PVOID, ULONG);
typedef LONG (NTAPI *PFN_RTL_ADJUST_PRIVILEGE)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);

typedef struct _FSUTIL_FILE_QUOTA_INFORMATION {
    ULONG NextEntryOffset;
    ULONG SidLength;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER QuotaUsed;
    LARGE_INTEGER QuotaThreshold;
    LARGE_INTEGER QuotaLimit;
    /* SID follows; allocate extra for a typical SID. */
    UCHAR SidBuffer[68];
} FSUTIL_FILE_QUOTA_INFORMATION;

static HandlerProc TrackMain;
static HandlerProc DisableMain;
static HandlerProc EnforceMain;
static HandlerProc ModifyMain;
static HandlerProc QueryMain;
static HandlerItem HandlersList[] =
{
    { DisableMain, _T("disable"), _T("Disable quota tracking") },
    { EnforceMain, _T("enforce"), _T("Enforce quotas") },
    { ModifyMain, _T("modify"), _T("Set a user quota threshold and limit") },
    { QueryMain, _T("query"), _T("Query volume quota control flags") },
    { TrackMain, _T("track"), _T("Track disk usage without enforcing") },
};

static BOOLEAN
LoadNtVolume(PFN_NT_QUERY_VOLUME *QueryFn, PFN_NT_SET_VOLUME *SetFn)
{
    HMODULE Ntdll;

    Ntdll = GetModuleHandleW(L"ntdll.dll");
    if (Ntdll == NULL)
        return FALSE;
    *QueryFn = (PFN_NT_QUERY_VOLUME)GetProcAddress(Ntdll, "NtQueryVolumeInformationFile");
    *SetFn = (PFN_NT_SET_VOLUME)GetProcAddress(Ntdll, "NtSetVolumeInformationFile");
    return (*QueryFn != NULL && *SetFn != NULL);
}

static int
SetQuotaFlags(const TCHAR *VolumeArg, ULONG WantedBits)
{
    HANDLE Volume;
    FSUTIL_FS_CONTROL_INFORMATION Control;
    FSUTIL_IO_STATUS_BLOCK IoStatus;
    PFN_NT_QUERY_VOLUME QueryFn;
    PFN_NT_SET_VOLUME SetFn;
    LONG Status;

    if (!LoadNtVolume(&QueryFn, &SetFn))
    {
        _ftprintf(stderr, _T("NtSetVolumeInformationFile was not found\n"));
        return 1;
    }

    Volume = OpenVolumeEx(VolumeArg, FALSE, TRUE, TRUE);
    if (Volume == INVALID_HANDLE_VALUE)
        return 1;

    ZeroMemory(&Control, sizeof(Control));
    ZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = QueryFn(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    if (Status < 0)
    {
        /* No prior control info: start from defaults and apply WantedBits. */
        ZeroMemory(&Control, sizeof(Control));
        Control.DefaultQuotaThreshold.QuadPart = -1LL;
        Control.DefaultQuotaLimit.QuadPart = -1LL;
    }

    Control.FileSystemControlFlags =
        (Control.FileSystemControlFlags & ~FILE_VC_QUOTA_MASK) | (WantedBits & FILE_VC_QUOTA_MASK);

    ZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = SetFn(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    CloseHandle(Volume);
    if (Status < 0)
    {
        _ftprintf(stderr, _T("NtSetVolumeInformationFile failed: 0x%08lx\n"), Status);
        return 1;
    }
    return 0;
}

static int
TrackMain(int argc, const TCHAR *argv[])
{
    if (argc < 2)
    {
        _ftprintf(stderr, _T("Usage: fsutil quota track <volume>\n"));
        return 1;
    }
    return SetQuotaFlags(argv[1], FILE_VC_QUOTA_TRACK);
}

static int
DisableMain(int argc, const TCHAR *argv[])
{
    if (argc < 2)
    {
        _ftprintf(stderr, _T("Usage: fsutil quota disable <volume>\n"));
        return 1;
    }
    return SetQuotaFlags(argv[1], FILE_VC_QUOTA_NONE);
}

static int
EnforceMain(int argc, const TCHAR *argv[])
{
    if (argc < 2)
    {
        _ftprintf(stderr, _T("Usage: fsutil quota enforce <volume>\n"));
        return 1;
    }
    return SetQuotaFlags(argv[1], FILE_VC_QUOTA_ENFORCE);
}

static int
QueryMain(int argc, const TCHAR *argv[])
{
    HANDLE Volume;
    FSUTIL_FS_CONTROL_INFORMATION Control;
    FSUTIL_IO_STATUS_BLOCK IoStatus;
    PFN_NT_QUERY_VOLUME QueryFn;
    PFN_NT_SET_VOLUME SetFn;
    LONG Status;

    if (argc < 2)
    {
        _ftprintf(stderr, _T("Usage: fsutil quota query <volume>\n"));
        return 1;
    }
    if (!LoadNtVolume(&QueryFn, &SetFn))
        return 1;

    Volume = OpenVolumeEx(argv[1], FALSE, TRUE, FALSE);
    if (Volume == INVALID_HANDLE_VALUE)
        return 1;

    ZeroMemory(&Control, sizeof(Control));
    ZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = QueryFn(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    CloseHandle(Volume);
    if (Status < 0)
    {
        _ftprintf(stderr, _T("NtQueryVolumeInformationFile failed: 0x%08lx\n"), Status);
        return 1;
    }

    _ftprintf(stdout, _T("FileSystemControlFlags = 0x%08lx\n"), Control.FileSystemControlFlags);
    _ftprintf(stdout, _T("DefaultQuotaThreshold = %I64d\n"), Control.DefaultQuotaThreshold.QuadPart);
    _ftprintf(stdout, _T("DefaultQuotaLimit = %I64d\n"), Control.DefaultQuotaLimit.QuadPart);
    return 0;
}

static int
ModifyMain(int argc, const TCHAR *argv[])
{
    HANDLE Volume;
    FSUTIL_FILE_QUOTA_INFORMATION Entry;
    FSUTIL_IO_STATUS_BLOCK IoStatus;
    PFN_NT_SET_QUOTA SetQuota;
    PFN_RTL_ADJUST_PRIVILEGE AdjustPrivilege;
    HMODULE Ntdll;
    DWORD SidBytes = sizeof(Entry.SidBuffer);
    DWORD DomainBytes = 0;
    TCHAR Domain[256];
    SID_NAME_USE Use;
    LONGLONG Threshold;
    LONGLONG Limit;
    DWORD Needed;
    LONG Status;
    BOOLEAN WasEnabled = FALSE;

    /* fsutil quota modify <volume> <threshold> <limit> <username> */
    if (argc < 5)
    {
        _ftprintf(stderr,
                  _T("Usage: fsutil quota modify <volume> <threshold> <limit> <username>\n"));
        return 1;
    }

    Threshold = _ttoi64(argv[2]);
    Limit = _ttoi64(argv[3]);

    ZeroMemory(&Entry, sizeof(Entry));
    Domain[0] = 0;
    DomainBytes = sizeof(Domain);
    if (!LookupAccountName(NULL, argv[4], Entry.SidBuffer, &SidBytes,
                           Domain, &DomainBytes, &Use))
    {
        PrintErrorMessage(GetLastError());
        return 1;
    }

    Entry.NextEntryOffset = 0;
    Entry.SidLength = SidBytes;
    Entry.QuotaThreshold.QuadPart = Threshold;
    Entry.QuotaLimit.QuadPart = Limit;
    Needed = FIELD_OFFSET(FSUTIL_FILE_QUOTA_INFORMATION, SidBuffer) + SidBytes;

    Ntdll = GetModuleHandleW(L"ntdll.dll");
    if (Ntdll == NULL)
    {
        _ftprintf(stderr, _T("ntdll.dll was not found\n"));
        return 1;
    }
    SetQuota = (PFN_NT_SET_QUOTA)GetProcAddress(Ntdll, "NtSetQuotaInformationFile");
    AdjustPrivilege = (PFN_RTL_ADJUST_PRIVILEGE)GetProcAddress(Ntdll, "RtlAdjustPrivilege");
    if (SetQuota == NULL)
    {
        _ftprintf(stderr, _T("NtSetQuotaInformationFile was not found\n"));
        return 1;
    }

    /* Same privilege the suite agent enables for QuotaDelete (Learn privilege
     * constants; ntfs-quota-on-disk.md Rev.4 NtSet path). */
    if (AdjustPrivilege != NULL)
        AdjustPrivilege(SE_INCREASE_QUOTA_PRIVILEGE, TRUE, FALSE, &WasEnabled);

    Volume = OpenVolumeEx(argv[1], FALSE, TRUE, TRUE);
    if (Volume == INVALID_HANDLE_VALUE)
        return 1;

    ZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = SetQuota(Volume, &IoStatus, &Entry, Needed);
    CloseHandle(Volume);
    if (Status < 0)
    {
        _ftprintf(stderr, _T("NtSetQuotaInformationFile failed: 0x%08lx\n"), Status);
        return 1;
    }
    return 0;
}

static void
PrintUsage(const TCHAR * Command)
{
    PrintDefaultUsage(_T(" QUOTA "), Command, (HandlerItem *)&HandlersList,
                      (sizeof(HandlersList) / sizeof(HandlersList[0])));
}

int
QuotaMain(int argc, const TCHAR *argv[])
{
    return FindHandler(argc, argv, (HandlerItem *)&HandlersList,
                       (sizeof(HandlersList) / sizeof(HandlersList[0])),
                       PrintUsage);
}
