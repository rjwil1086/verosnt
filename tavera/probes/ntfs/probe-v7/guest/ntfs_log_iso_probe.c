/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v7/guest/ntfs_log_iso_probe.c
 * PURPOSE:     Single-op staged $LogFile isolation probe
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Raw-format probe session only. Stage letter in C:\temp\ntfs_log_iso_stage.txt:
 *   F = format only
 *   C = format + create empty logiso.bin + flush
 *   W = C + write 512 known bytes + flush
 *   N = W + rename to logiso2.bin + flush
 *   X = N + delete + flush
 *   T = W + fsutil dirty set + dismount/remount + readback
 * Host dumps under output/ntfs/probe-v7/<stage>/. Do not edit ntfs.sys here.
 *
 * Sources:
 * - https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty
 * - https://learn.microsoft.com/en-us/windows-server/storage/file-server/ntfs-overview
 * - https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)
 * - in-tree fmifs FormatEx pattern (tavera/test-suites/ntfs/agent/ntfs_agent.c)
 * - tavera/probes/ntfs/probe-v5/guest/ntfs_log_probe.c (disk/format helpers)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12
#ifndef FSCTL_DISMOUNT_VOLUME
#define FSCTL_DISMOUNT_VOLUME CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 9, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_letter[8];
static ULONG g_fmt_bad;
static char g_stage = 'F';
static UCHAR g_known[512];

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
    create.Mbr.Signature = 0x4E544653;
    DeviceIoControl(h, IOCTL_DISK_CREATE_DISK, &create, sizeof(create), NULL, 0, &got, NULL);
    off.QuadPart = 2048LL * geo.BytesPerSector;
    if (off.QuadPart >= len.Length.QuadPart)
        off.QuadPart = (LONGLONG)geo.BytesPerSector * geo.SectorsPerTrack;
    length.QuadPart = len.Length.QuadPart - off.QuadPart;
    length.QuadPart -= length.QuadPart % geo.BytesPerSector;
    ZeroMemory(buf, sizeof(buf));
    layout = (DRIVE_LAYOUT_INFORMATION *)buf;
    layout->PartitionCount = 4;
    layout->Signature = 0x4E544653;
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
    WCopy(label, L"LOGISO");
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

static HANDLE OpenVol(void)
{
    WCHAR path[16];
    WCopy(path, L"\\\\.\\");
    WCat(path, g_letter);
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static void FlushVol(void)
{
    HANDLE vol = OpenVol();
    if (vol != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(vol);
        CloseHandle(vol);
    }
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

static void PathOnVol(WCHAR *out, const WCHAR *name)
{
    WCopy(out, g_letter);
    WCat(out, L"\\");
    WCat(out, name);
}

static void FillKnown(void)
{
    DWORD i;
    for (i = 0; i < sizeof(g_known); i++)
        g_known[i] = (UCHAR)(i * 3 + 7);
}

static int CreateEmpty(void)
{
    WCHAR path[260];
    HANDLE h;

    PathOnVol(path, L"logiso.bin");
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LineErr("Log", "create empty", GetLastError());
        return 0;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    FlushVol();
    Line("Log", "create empty ok");
    return 1;
}

static int WriteKnown(void)
{
    WCHAR path[260];
    HANDLE h;
    DWORD wrote = 0;

    FillKnown();
    PathOnVol(path, L"logiso.bin");
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, g_known, sizeof(g_known), &wrote, NULL) ||
        wrote != sizeof(g_known)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        LineErr("Log", "write known", GetLastError());
        return 0;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    FlushVol();
    Line("Log", "write known ok");
    return 1;
}

static int RenameKnown(void)
{
    WCHAR path[260];
    WCHAR other[260];

    PathOnVol(path, L"logiso.bin");
    PathOnVol(other, L"logiso2.bin");
    if (!MoveFileW(path, other)) {
        LineErr("Log", "rename", GetLastError());
        return 0;
    }
    FlushVol();
    Line("Log", "rename ok");
    return 1;
}

static int DeleteRenamed(void)
{
    WCHAR other[260];

    PathOnVol(other, L"logiso2.bin");
    if (!DeleteFileW(other)) {
        LineErr("Log", "delete", GetLastError());
        return 0;
    }
    FlushVol();
    Line("Log", "delete ok");
    return 1;
}

static int DirtySet(void)
{
    WCHAR cmd[96];
    WCopy(cmd, L"fsutil.exe dirty set ");
    WCat(cmd, g_letter);
    Line("Log", "fsutil dirty set");
    if (!RunCmd(cmd, 120000)) {
        LineErr("Log", "fsutil dirty set failed", GetLastError());
        return 0;
    }
    Line("Log", "fsutil dirty set ok");
    return 1;
}

static int DismountRemountReadback(void)
{
    HANDLE vol;
    HANDLE h;
    WCHAR path[260];
    UCHAR back[512];
    DWORD got = 0;
    DWORD i;
    WCHAR root[8];
    WCHAR fs[32];

    SetCurrentDirectoryW(L"C:\\");
    vol = OpenVol();
    if (vol == INVALID_HANDLE_VALUE) {
        LineErr("Log", "vol before dismount", GetLastError());
        return 0;
    }
    for (i = 0; i < 20; i++) {
        if (DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL))
            break;
        Sleep(200);
    }
    if (!DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
        LineErr("Log", "dismount", GetLastError());
        CloseHandle(vol);
        return 0;
    }
    CloseHandle(vol);
    Sleep(500);
    WCopy(root, g_letter);
    WCat(root, L"\\");
    fs[0] = 0;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32) || !WEq(fs, L"NTFS")) {
        Line("Log", "remount not NTFS");
        return 0;
    }
    Line("Log", "remount NTFS ok");
    FillKnown();
    PathOnVol(path, L"logiso.bin");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, sizeof(back), &got, NULL) || got != sizeof(g_known) ||
        !MemEq(back, g_known, sizeof(g_known))) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Line("Log", "readback mismatch");
        return 0;
    }
    CloseHandle(h);
    Line("Log", "readback ok");
    FlushVol();
    return 1;
}

static void OpenLog(void)
{
    g_log = CreateFileW(L"\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        g_log = CreateFileW(L"COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

static void ShutdownGuest(void)
{
    WCHAR cmd[64];
    WCopy(cmd, L"shutdown.exe /s /t 0 /f");
    RunCmd(cmd, 120000);
}

static void LoadStage(void)
{
    HANDLE h;
    char buf[8];
    DWORD got = 0;

    h = CreateFileW(L"C:\\temp\\ntfs_log_iso_stage.txt", GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        char c = buf[0];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == 'F' || c == 'C' || c == 'W' || c == 'N' || c == 'X' || c == 'T')
            g_stage = c;
    }
    CloseHandle(h);
}

int main(void)
{
    int ok = 1;
    DWORD disk = 0;
    char stag[2];

    OpenLog();
    if (g_log == INVALID_HANDLE_VALUE)
        return 2;
    LoadStage();
    stag[0] = g_stage;
    stag[1] = 0;
    Line("Probe", "ntfs-log-iso");
    Line("Stage", stag);
    if (!PickDisk(&disk)) {
        Line("Disk", "no blank disk");
        ok = 0;
    } else if (!PartitionDisk(disk)) {
        Line("Disk", "partition failed");
        ok = 0;
    } else if (!FormatVolume(disk)) {
        ok = 0;
    } else {
        FlushVol();
        if (g_stage == 'F') {
            Line("Stage", "stop after format");
        } else if (!CreateEmpty()) {
            ok = 0;
        } else if (g_stage == 'C') {
            Line("Stage", "stop after create");
        } else if (!WriteKnown()) {
            ok = 0;
        } else if (g_stage == 'W') {
            Line("Stage", "stop after write");
        } else if (g_stage == 'N') {
            if (!RenameKnown())
                ok = 0;
            else
                Line("Stage", "stop after rename");
        } else if (g_stage == 'X') {
            if (!RenameKnown() || !DeleteRenamed())
                ok = 0;
            else
                Line("Stage", "stop after delete");
        } else if (g_stage == 'T') {
            if (!DirtySet() || !DismountRemountReadback())
                ok = 0;
            else
                Line("Stage", "stop after dirty remount");
        } else {
            Line("Stage", "unknown");
            ok = 0;
        }
    }
    FlushVol();
    Text(ok ? "NTFS-LOG-ISO-DONE\tok\r\n" : "NTFS-LOG-ISO-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(3000);
    ShutdownGuest();
    return ok ? 0 : 1;
}
