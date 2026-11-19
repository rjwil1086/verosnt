/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v9/guest/ntfs_pagefile_probe.c
 * PURPOSE:     API-behavior probe: NtCreatePagingFile + ordinary CreateFile
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Probe session only. Formats a blank secondary disk NTFS, enables
 * SeCreatePagefilePrivilege, calls NtCreatePagingFile (name already in-tree),
 * then tries ordinary CreateFileW / WriteFile on that path. Logs to COM1.
 * Marker: NTFS-PAGE-PROBE-DONE. Do not edit ntfs.sys here.
 *
 * Sources:
 * - https://learn.microsoft.com/en-us/previous-versions/windows/drivers/ifs/irp-mj-create (SL_OPEN_PAGING_FILE)
 * - https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-pagefilesetting (SeCreatePagefilePrivilege)
 * - https://learn.microsoft.com/en-us/previous-versions/windows/hardware/wlk/ff563405(v=vs.85) (CreatePagingFileTest notes)
 * - in-tree sdk/include/ndk/mmfuncs.h NtCreatePagingFile
 * - in-tree ntoskrnl/mm/pagefile.c MINIMUM_PAGEFILE_SIZE (256*PAGE_SIZE) — probe sizes are probe reads
 * - tavera/probes/ntfs/probe-v7/guest/ntfs_log_iso_probe.c (disk/format helpers)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12

/* Probe sizes (bytes): above in-tree MINIMUM_PAGEFILE_SIZE (= 256*4096). Not a Windows field. */
#define PAGE_PROBE_MIN_BYTES (2ULL * 1024ULL * 1024ULL)
#define PAGE_PROBE_MAX_BYTES (4ULL * 1024ULL * 1024ULL)

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *PFN_NT_CREATE_PAGING_FILE)(
    PUNICODE_STRING FileName,
    PLARGE_INTEGER MinimumSize,
    PLARGE_INTEGER MaximumSize,
    ULONG Reserved);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_letter[8];
static ULONG g_fmt_bad;

static void Text(const char *s)
{
    DWORD wrote = 0;
    if (g_log == INVALID_HANDLE_VALUE || s == NULL)
        return;
    WriteFile(g_log, s, (DWORD)lstrlenA(s), &wrote, NULL);
}

static void Line(const char *tag, const char *detail)
{
    Text(tag);
    Text("\t");
    Text(detail ? detail : "");
    Text("\r\n");
}

static void U32Text(ULONG v, char *out)
{
    char rev[12];
    int n = 0;
    int i;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (v) {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        out[i] = rev[n - 1 - i];
    out[n] = 0;
}

static void HexStatus(LONG st, char *out)
{
    static const char *hex = "0123456789ABCDEF";
    ULONG u = (ULONG)st;
    int i;
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8; i++)
        out[2 + i] = hex[(u >> (28 - 4 * i)) & 0xF];
    out[10] = 0;
}

static void LineErr(const char *tag, const char *detail, DWORD err)
{
    char buf[16];
    U32Text(err, buf);
    Text(tag);
    Text("\t");
    Text(detail ? detail : "");
    Text("\te=");
    Text(buf);
    Text("\r\n");
}

static void LineNt(const char *tag, const char *detail, LONG st)
{
    char buf[16];
    HexStatus(st, buf);
    Text(tag);
    Text("\t");
    Text(detail ? detail : "");
    Text("\t");
    Text(buf);
    Text("\r\n");
}

static BOOLEAN NTAPI FormatCb(ULONG Command, ULONG SubAction, PVOID ActionInfo)
{
    char buf[16];
    (void)SubAction;
    (void)ActionInfo;
    if (Command != 15) {
        U32Text(Command, buf);
        Line("FmtCb", buf);
    }
    if (Command == 7 || Command == 8 || Command == 9 || Command == 17)
        g_fmt_bad = Command;
    return TRUE;
}

static int WEq(const WCHAR *a, const WCHAR *b)
{
    return lstrcmpiW(a, b) == 0;
}

static void WCopy(WCHAR *dst, const WCHAR *src)
{
    while (*src)
        *dst++ = *src++;
    *dst = 0;
}

static void WCat(WCHAR *dst, const WCHAR *src)
{
    while (*dst)
        dst++;
    while (*src)
        *dst++ = *src++;
    *dst = 0;
}

static int MemEq(const void *a, const void *b, DWORD n)
{
    const UCHAR *p = (const UCHAR *)a;
    const UCHAR *q = (const UCHAR *)b;
    DWORD i;
    for (i = 0; i < n; i++) {
        if (p[i] != q[i])
            return 0;
    }
    return 1;
}

static HANDLE OpenDisk(DWORD index, DWORD access)
{
    WCHAR path[64];
    WCHAR num[8];
    WCHAR rev[8];
    DWORD v = index;
    int n = 0;
    int i;

    WCopy(path, L"\\\\.\\PhysicalDrive");
    if (v == 0) {
        WCat(path, L"0");
    } else {
        while (v) {
            rev[n++] = (WCHAR)(L'0' + (v % 10));
            v /= 10;
        }
        for (i = 0; i < n; i++)
            num[i] = rev[n - 1 - i];
        num[n] = 0;
        WCat(path, num);
    }
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static int PickDisk(DWORD *index)
{
    DWORD i;
    for (i = 0; i < 8; i++) {
        HANDLE h = OpenDisk(i, GENERIC_READ);
        GET_LENGTH_INFORMATION len;
        DWORD got = 0;
        UCHAR sec[512];
        LARGE_INTEGER zero;

        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), &got, NULL)) {
            CloseHandle(h);
            continue;
        }
        if (len.Length.QuadPart < 48LL * 1024 * 1024 || len.Length.QuadPart > 96LL * 1024 * 1024) {
            CloseHandle(h);
            continue;
        }
        zero.QuadPart = 0;
        SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
        ReadFile(h, sec, 512, &got, NULL);
        CloseHandle(h);
        if (got >= 8 && (MemEq(sec + 3, "MSDOS", 5) || MemEq(sec + 3, "EXFAT", 5)))
            continue;
        *index = i;
        return 1;
    }
    return 0;
}

static int PartitionDisk(DWORD index)
{
    HANDLE h = OpenDisk(index, GENERIC_READ | GENERIC_WRITE);
    DISK_GEOMETRY geo;
    GET_LENGTH_INFORMATION len;
    CREATE_DISK create;
    DRIVE_LAYOUT_INFORMATION *layout;
    DWORD got = 0;
    DWORD bytes;
    UCHAR buf[sizeof(DRIVE_LAYOUT_INFORMATION) + 3 * sizeof(PARTITION_INFORMATION)];
    LARGE_INTEGER off;
    LARGE_INTEGER length;
    int i;

    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geo, sizeof(geo), &got, NULL)) {
        CloseHandle(h);
        return 0;
    }
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), &got, NULL)) {
        CloseHandle(h);
        return 0;
    }
    ZeroMemory(&create, sizeof(create));
    create.PartitionStyle = PARTITION_STYLE_MBR;
    create.Mbr.Signature = 0x50414745;
    DeviceIoControl(h, IOCTL_DISK_CREATE_DISK, &create, sizeof(create), NULL, 0, &got, NULL);
    off.QuadPart = 2048LL * geo.BytesPerSector;
    if (off.QuadPart >= len.Length.QuadPart)
        off.QuadPart = (LONGLONG)geo.BytesPerSector * geo.SectorsPerTrack;
    length.QuadPart = len.Length.QuadPart - off.QuadPart;
    length.QuadPart -= length.QuadPart % geo.BytesPerSector;
    ZeroMemory(buf, sizeof(buf));
    layout = (DRIVE_LAYOUT_INFORMATION *)buf;
    layout->PartitionCount = 4;
    layout->Signature = 0x50414745;
    layout->PartitionEntry[0].StartingOffset = off;
    layout->PartitionEntry[0].PartitionLength = length;
    layout->PartitionEntry[0].HiddenSectors = (DWORD)(off.QuadPart / geo.BytesPerSector);
    layout->PartitionEntry[0].PartitionNumber = 1;
    layout->PartitionEntry[0].PartitionType = PARTITION_IFS;
    layout->PartitionEntry[0].RecognizedPartition = TRUE;
    layout->PartitionEntry[0].RewritePartition = TRUE;
    for (i = 1; i < 4; i++)
        layout->PartitionEntry[i].RewritePartition = TRUE;
    bytes = sizeof(DRIVE_LAYOUT_INFORMATION) + 3 * sizeof(PARTITION_INFORMATION);
    if (!DeviceIoControl(h, IOCTL_DISK_SET_DRIVE_LAYOUT, layout, bytes, layout, bytes, &got, NULL)) {
        CloseHandle(h);
        return 0;
    }
    DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL);
    CloseHandle(h);
    return 1;
}

static int AssignLetter(DWORD index)
{
    WCHAR target[64];
    WCHAR num[8];
    WCHAR rev[8];
    DWORD v = index;
    int n = 0;
    int i;

    WCopy(target, L"\\Device\\Harddisk");
    if (v == 0) {
        WCat(target, L"0");
    } else {
        while (v) {
            rev[n++] = (WCHAR)(L'0' + (v % 10));
            v /= 10;
        }
        for (i = 0; i < n; i++)
            num[i] = rev[n - 1 - i];
        num[n] = 0;
        WCat(target, num);
    }
    WCat(target, L"\\Partition1");
    WCopy(g_letter, L"T:");
    DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION, g_letter, NULL);
    return DefineDosDeviceW(DDD_RAW_TARGET_PATH, g_letter, target) ? 1 : 0;
}

static int FormatVolume(DWORD disk)
{
    HMODULE fmifs;
    PFN_FORMATEX format_ex;
    WCHAR root[8];
    WCHAR fs[8];
    WCHAR label[16];
    HANDLE vol;
    int i;

    fmifs = LoadLibraryW(L"fmifs.dll");
    if (fmifs == NULL) {
        Line("Format", "fmifs missing");
        return 0;
    }
    format_ex = (PFN_FORMATEX)GetProcAddress(fmifs, "FormatEx");
    if (format_ex == NULL) {
        Line("Format", "FormatEx missing");
        return 0;
    }
    Sleep(2000);
    if (!AssignLetter(disk)) {
        Line("Format", "letter before format failed");
        return 0;
    }
    WCopy(root, L"\\\\.\\");
    WCat(root, g_letter);
    vol = CreateFileW(root, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) {
        Line("Format", "volume open before format failed");
        return 0;
    }
    CloseHandle(vol);
    g_fmt_bad = 0;
    WCopy(root, g_letter);
    WCat(root, L"\\");
    WCopy(fs, L"NTFS");
    WCopy(label, L"PAGEPF");
    format_ex(root, FMIFS_HARDDISK, fs, label, TRUE, 0, FormatCb);
    if (g_fmt_bad) {
        Line("Format", "FormatEx reported failure");
        return 0;
    }
    AssignLetter(disk);
    for (i = 0; i < 60; i++) {
        WCHAR name[32];
        DWORD flags = 0;
        WCopy(root, g_letter);
        WCat(root, L"\\");
        if (GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, name, 32) && WEq(name, L"NTFS")) {
            Line("Format", "ok");
            return 1;
        }
        Sleep(500);
    }
    Line("Format", "volume did not mount as NTFS");
    return 0;
}

static int EnablePagefilePriv(void)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    DWORD err;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LineErr("Priv", "OpenProcessToken", GetLastError());
        return 0;
    }
    if (!LookupPrivilegeValueW(NULL, L"SeCreatePagefilePrivilege", &luid)) {
        LineErr("Priv", "LookupPrivilegeValue", GetLastError());
        CloseHandle(tok);
        return 0;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        LineErr("Priv", "AdjustTokenPrivileges", GetLastError());
        CloseHandle(tok);
        return 0;
    }
    err = GetLastError();
    CloseHandle(tok);
    if (err == ERROR_NOT_ALL_ASSIGNED) {
        LineErr("Priv", "not assigned", err);
        return 0;
    }
    Line("Priv", "SeCreatePagefilePrivilege enabled");
    return 1;
}

static int RunPagingFileProbe(void)
{
    HMODULE ntdll;
    PFN_NT_CREATE_PAGING_FILE create_pf;
    WCHAR dos_path[64];
    WCHAR nt_path[96];
    UNICODE_STRING us;
    LARGE_INTEGER min_sz;
    LARGE_INTEGER max_sz;
    NTSTATUS st;
    HANDLE h;
    DWORD wrote = 0;
    char one = 'A';

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL)
        ntdll = LoadLibraryW(L"ntdll.dll");
    if (ntdll == NULL) {
        LineErr("Page", "ntdll", GetLastError());
        return 0;
    }
    create_pf = (PFN_NT_CREATE_PAGING_FILE)GetProcAddress(ntdll, "NtCreatePagingFile");
    if (create_pf == NULL) {
        Line("Page", "NtCreatePagingFile MISSING");
        return 0;
    }
    Line("Page", "NtCreatePagingFile resolved");

    WCopy(dos_path, g_letter);
    WCat(dos_path, L"\\pagefile.sys");
    WCopy(nt_path, L"\\??\\");
    WCat(nt_path, dos_path);
    us.Buffer = nt_path;
    us.Length = (USHORT)(lstrlenW(nt_path) * sizeof(WCHAR));
    us.MaximumLength = (USHORT)(us.Length + sizeof(WCHAR));
    min_sz.QuadPart = (LONGLONG)PAGE_PROBE_MIN_BYTES;
    max_sz.QuadPart = (LONGLONG)PAGE_PROBE_MAX_BYTES;
    Line("Page", "call NtCreatePagingFile min=2MiB max=4MiB");
    st = create_pf(&us, &min_sz, &max_sz, 0);
    LineNt("Page", "NtCreatePagingFile", st);
    if (st < 0)
        return 0;

    h = CreateFileW(dos_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LineErr("Page", "CreateFile after create FAIL_expected", GetLastError());
    } else {
        Line("Page", "CreateFile after create UNEXPECTED_ok");
        if (WriteFile(h, &one, 1, &wrote, NULL))
            Line("Page", "WriteFile after create UNEXPECTED_ok");
        else
            LineErr("Page", "WriteFile after create", GetLastError());
        CloseHandle(h);
    }

    h = CreateFileW(dos_path, SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LineErr("Page", "CreateFile SYNCHRONIZE-only FAIL_expected", GetLastError());
    } else {
        Line("Page", "CreateFile SYNCHRONIZE-only UNEXPECTED_ok");
        CloseHandle(h);
    }
    return 1;
}

static void OpenLog(void)
{
    g_log = CreateFileW(L"\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        g_log = CreateFileW(L"COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

static int RunCmd(const WCHAR *cmd, DWORD timeout_ms)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    WCHAR buf[512];

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    WCopy(buf, cmd);
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        LineErr("RunCmd", "CreateProcess", GetLastError());
        return 0;
    }
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Line("RunCmd", "timeout");
        return 0;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static void ShutdownGuest(void)
{
    WCHAR cmd[64];
    WCopy(cmd, L"shutdown.exe /s /t 0 /f");
    RunCmd(cmd, 120000);
}

int main(void)
{
    int ok = 1;
    DWORD disk = 0;

    OpenLog();
    if (g_log == INVALID_HANDLE_VALUE)
        return 2;
    Line("Probe", "ntfs-pagefile");
    Line("Build", "19045-guest");
    if (!PickDisk(&disk)) {
        Line("Disk", "no blank disk");
        ok = 0;
    } else if (!PartitionDisk(disk)) {
        Line("Disk", "partition failed");
        ok = 0;
    } else if (!FormatVolume(disk)) {
        ok = 0;
    } else if (!EnablePagefilePriv()) {
        ok = 0;
    } else if (!RunPagingFileProbe()) {
        ok = 0;
    }
    Text(ok ? "NTFS-PAGE-PROBE-DONE\tok\r\n" : "NTFS-PAGE-PROBE-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(3000);
    ShutdownGuest();
    return ok ? 0 : 1;
}
