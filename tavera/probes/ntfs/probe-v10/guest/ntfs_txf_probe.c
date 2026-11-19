/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v10/guest/ntfs_txf_probe.c
 * PURPOSE:     API-behavior probe: CreateTransaction / CreateFileTransacted
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Probe session only. Formats a blank secondary disk NTFS, then:
 *   C = CreateTransaction + CreateFileTransacted + write + CommitTransaction
 *   R = CreateTransaction + CreateFileTransacted + write + RollbackTransaction
 *   V = after uncommitted write, non-transacted CreateFile/ReadFile visibility
 * Logs Win32 errors and commit/rollback outcomes to COM1.
 * Marker: NTFS-TXF-PROBE-DONE. Do not edit ntfs.sys here.
 *
 * Sources:
 * - https://learn.microsoft.com/en-us/windows/win32/fileio/how-to-use-transactional-ntfs
 * - https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiletransactedw
 * - https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-createtransaction
 * - https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-committransaction
 * - https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-rollbacktransaction
 * - tavera/probes/ntfs/probe-v7/guest/ntfs_log_iso_probe.c (disk/format helpers)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);

typedef HANDLE (WINAPI *PFN_CREATE_TRANSACTION)(
    LPSECURITY_ATTRIBUTES, LPGUID, DWORD, DWORD, DWORD, DWORD, LPWSTR);
typedef BOOL (WINAPI *PFN_COMMIT_TRANSACTION)(HANDLE);
typedef BOOL (WINAPI *PFN_ROLLBACK_TRANSACTION)(HANDLE);
typedef HANDLE (WINAPI *PFN_CREATE_FILE_TRANSACTED)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE,
    HANDLE, PUSHORT, PVOID);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_letter[8];
static ULONG g_fmt_bad;
static PFN_CREATE_TRANSACTION g_create_tx;
static PFN_COMMIT_TRANSACTION g_commit_tx;
static PFN_ROLLBACK_TRANSACTION g_rollback_tx;
static PFN_CREATE_FILE_TRANSACTED g_create_ft;

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
    create.Mbr.Signature = 0x54584631;
    DeviceIoControl(h, IOCTL_DISK_CREATE_DISK, &create, sizeof(create), NULL, 0, &got, NULL);
    off.QuadPart = 2048LL * geo.BytesPerSector;
    if (off.QuadPart >= len.Length.QuadPart)
        off.QuadPart = (LONGLONG)geo.BytesPerSector * geo.SectorsPerTrack;
    length.QuadPart = len.Length.QuadPart - off.QuadPart;
    length.QuadPart -= length.QuadPart % geo.BytesPerSector;
    ZeroMemory(buf, sizeof(buf));
    layout = (DRIVE_LAYOUT_INFORMATION *)buf;
    layout->PartitionCount = 4;
    layout->Signature = 0x54584631;
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
    WCopy(label, L"TXFPF");
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

static void PathOnVol(WCHAR *out, const WCHAR *name)
{
    WCopy(out, g_letter);
    WCat(out, L"\\");
    WCat(out, name);
}

static int LoadTxfApis(void)
{
    HMODULE ktm;
    HMODULE k32;

    ktm = LoadLibraryW(L"ktmw32.dll");
    if (ktm == NULL) {
        LineErr("TxF", "ktmw32 missing", GetLastError());
        return 0;
    }
    g_create_tx = (PFN_CREATE_TRANSACTION)GetProcAddress(ktm, "CreateTransaction");
    g_commit_tx = (PFN_COMMIT_TRANSACTION)GetProcAddress(ktm, "CommitTransaction");
    g_rollback_tx = (PFN_ROLLBACK_TRANSACTION)GetProcAddress(ktm, "RollbackTransaction");
    if (!g_create_tx || !g_commit_tx || !g_rollback_tx) {
        Line("TxF", "KTM entry MISSING");
        return 0;
    }
    k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == NULL)
        k32 = LoadLibraryW(L"kernel32.dll");
    g_create_ft = (PFN_CREATE_FILE_TRANSACTED)GetProcAddress(k32, "CreateFileTransactedW");
    if (g_create_ft == NULL) {
        Line("TxF", "CreateFileTransactedW MISSING");
        return 0;
    }
    Line("TxF", "APIs resolved");
    return 1;
}

static int CaseCommit(void)
{
    HANDLE tx;
    HANDLE file;
    WCHAR path[260];
    const char payload[] = "TXF-COMMIT-PAYLOAD-v10";
    DWORD wrote = 0;
    HANDLE plain;
    char back[64];
    DWORD got = 0;

    PathOnVol(path, L"txf_commit.txt");
    tx = g_create_tx(NULL, NULL, 0, 0, 0, 0, L"ntfs-txf-commit");
    if (tx == INVALID_HANDLE_VALUE) {
        LineErr("Commit", "CreateTransaction", GetLastError());
        return 0;
    }
    Line("Commit", "CreateTransaction ok");
    file = g_create_ft(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL, tx, NULL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LineErr("Commit", "CreateFileTransacted", GetLastError());
        CloseHandle(tx);
        return 0;
    }
    Line("Commit", "CreateFileTransacted ok");
    if (!WriteFile(file, payload, (DWORD)sizeof(payload) - 1, &wrote, NULL) ||
        wrote != sizeof(payload) - 1) {
        LineErr("Commit", "WriteFile", GetLastError());
        CloseHandle(file);
        CloseHandle(tx);
        return 0;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    Line("Commit", "write+close ok");
    if (!g_commit_tx(tx)) {
        LineErr("Commit", "CommitTransaction", GetLastError());
        CloseHandle(tx);
        return 0;
    }
    CloseHandle(tx);
    Line("Commit", "CommitTransaction ok");

    plain = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (plain == INVALID_HANDLE_VALUE) {
        LineErr("Commit", "plain open after commit", GetLastError());
        return 0;
    }
    ZeroMemory(back, sizeof(back));
    if (!ReadFile(plain, back, sizeof(back) - 1, &got, NULL) ||
        got != sizeof(payload) - 1 || !MemEq(back, payload, got)) {
        Line("Commit", "plain read mismatch");
        CloseHandle(plain);
        return 0;
    }
    CloseHandle(plain);
    Line("Commit", "plain read matches");
    return 1;
}

static int CaseRollback(void)
{
    HANDLE tx;
    HANDLE file;
    WCHAR path[260];
    const char payload[] = "TXF-ROLLBACK-PAYLOAD-v10";
    DWORD wrote = 0;
    HANDLE plain;

    PathOnVol(path, L"txf_rollback.txt");
    tx = g_create_tx(NULL, NULL, 0, 0, 0, 0, L"ntfs-txf-rollback");
    if (tx == INVALID_HANDLE_VALUE) {
        LineErr("Rollback", "CreateTransaction", GetLastError());
        return 0;
    }
    Line("Rollback", "CreateTransaction ok");
    file = g_create_ft(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL, tx, NULL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LineErr("Rollback", "CreateFileTransacted", GetLastError());
        CloseHandle(tx);
        return 0;
    }
    if (!WriteFile(file, payload, (DWORD)sizeof(payload) - 1, &wrote, NULL)) {
        LineErr("Rollback", "WriteFile", GetLastError());
        CloseHandle(file);
        CloseHandle(tx);
        return 0;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    Line("Rollback", "write+close ok");
    if (!g_rollback_tx(tx)) {
        LineErr("Rollback", "RollbackTransaction", GetLastError());
        CloseHandle(tx);
        return 0;
    }
    CloseHandle(tx);
    Line("Rollback", "RollbackTransaction ok");

    plain = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (plain == INVALID_HANDLE_VALUE) {
        LineErr("Rollback", "plain open absent_expected", GetLastError());
        return 1;
    }
    Line("Rollback", "plain open UNEXPECTED_ok");
    CloseHandle(plain);
    return 0;
}

static int CaseVisibility(void)
{
    HANDLE tx;
    HANDLE file;
    WCHAR path[260];
    const char payload[] = "TXF-VIS-PAYLOAD-v10";
    DWORD wrote = 0;
    HANDLE plain;
    int ok = 1;

    PathOnVol(path, L"txf_vis.txt");
    tx = g_create_tx(NULL, NULL, 0, 0, 0, 0, L"ntfs-txf-vis");
    if (tx == INVALID_HANDLE_VALUE) {
        LineErr("Vis", "CreateTransaction", GetLastError());
        return 0;
    }
    file = g_create_ft(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL, tx, NULL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LineErr("Vis", "CreateFileTransacted", GetLastError());
        CloseHandle(tx);
        return 0;
    }
    if (!WriteFile(file, payload, (DWORD)sizeof(payload) - 1, &wrote, NULL)) {
        LineErr("Vis", "WriteFile", GetLastError());
        CloseHandle(file);
        CloseHandle(tx);
        return 0;
    }
    FlushFileBuffers(file);
    Line("Vis", "uncommitted write ok");

    plain = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (plain == INVALID_HANDLE_VALUE) {
        LineErr("Vis", "plain open while uncommitted", GetLastError());
    } else {
        char back[64];
        DWORD got = 0;
        ZeroMemory(back, sizeof(back));
        if (ReadFile(plain, back, sizeof(back) - 1, &got, NULL) && got > 0)
            Line("Vis", "plain read while uncommitted SAW_BYTES");
        else
            LineErr("Vis", "plain read while uncommitted", GetLastError());
        CloseHandle(plain);
    }

    CloseHandle(file);
    if (!g_rollback_tx(tx)) {
        LineErr("Vis", "RollbackTransaction", GetLastError());
        ok = 0;
    } else {
        Line("Vis", "RollbackTransaction ok");
    }
    CloseHandle(tx);
    return ok;
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
    Line("Probe", "ntfs-txf");
    Line("Build", "19045-guest");
    if (!LoadTxfApis()) {
        ok = 0;
    } else if (!PickDisk(&disk)) {
        Line("Disk", "no blank disk");
        ok = 0;
    } else if (!PartitionDisk(disk)) {
        Line("Disk", "partition failed");
        ok = 0;
    } else if (!FormatVolume(disk)) {
        ok = 0;
    } else {
        if (!CaseCommit())
            ok = 0;
        if (!CaseRollback())
            ok = 0;
        if (!CaseVisibility())
            ok = 0;
    }
    Text(ok ? "NTFS-TXF-PROBE-DONE\tok\r\n" : "NTFS-TXF-PROBE-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(3000);
    ShutdownGuest();
    return ok ? 0 : 1;
}
