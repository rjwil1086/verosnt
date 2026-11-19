/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v6/guest/ntfs_efs_probe.c
 * PURPOSE:     Staged EncryptFileW / $EFS raw-format probe
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Raw-format probe session only. Stage letter in C:\temp\ntfs_efs_stage.txt:
 *   Z = empty secretA.txt + EncryptFileW
 *   S = 512-byte pattern (i*3+1) + EncryptFileW
 *   L = 2048-byte pattern + EncryptFileW
 *   P = same-boot secretA (i*3+1) + secretB (i*5+7)
 *   R = S then DecryptFileW, overwrite, EncryptFileW again
 * Host dumps under output/ntfs/probe-v6/<stage>/. Do not edit ntfs.sys here.
 *
 * Sources:
 * - https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-encryptfilew
 * - https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-decryptfilew
 * - https://learn.microsoft.com/en-us/windows/win32/fileio/handling-encrypted-files-and-directories
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702
 * - in-tree fmifs FormatEx pattern (tavera/test-suites/ntfs/agent/ntfs_agent.c)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12
#ifndef FILE_SUPPORTS_ENCRYPTION
#define FILE_SUPPORTS_ENCRYPTION 0x00020000
#endif
#ifndef FILE_ATTRIBUTE_ENCRYPTED
#define FILE_ATTRIBUTE_ENCRYPTED 0x00004000
#endif

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);
typedef BOOL (WINAPI *PFN_ENCRYPT)(LPCWSTR);
typedef BOOL (WINAPI *PFN_DECRYPT)(LPCWSTR, DWORD);
typedef DWORD (WINAPI *PFN_FINALPATH)(HANDLE, LPWSTR, DWORD, DWORD);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_letter[8];
static ULONG g_fmt_bad;
static char g_stage = 'S';

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
    WCopy(label, L"EFSPRB");
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

static int EnsureEfsDir(void)
{
    WCHAR dir[260];
    WCHAR root[8];
    DWORD flags = 0;

    WCopy(root, g_letter);
    WCat(root, L"\\");
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, NULL, 0)) {
        LineErr("Efs", "GetVolumeInformation", GetLastError());
        return 0;
    }
    if ((flags & FILE_SUPPORTS_ENCRYPTION) == 0) {
        Line("Efs", "FILE_SUPPORTS_ENCRYPTION clear");
        return 0;
    }
    Line("Efs", "FILE_SUPPORTS_ENCRYPTION set");
    PathOnVol(dir, L"efs");
    CreateDirectoryW(dir, NULL);
    return 1;
}

static int EncryptPath(const WCHAR *rel)
{
    HMODULE k32;
    HMODULE adv;
    PFN_ENCRYPT enc;
    PFN_FINALPATH final_path;
    WCHAR path[260];
    WCHAR full[520];
    HANDLE h;
    DWORD n;
    DWORD attr;

    PathOnVol(path, rel);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LineErr("Efs", "open for encrypt", GetLastError());
        return 0;
    }
    k32 = GetModuleHandleW(L"kernel32.dll");
    final_path = k32 ? (PFN_FINALPATH)GetProcAddress(k32, "GetFinalPathNameByHandleW") : NULL;
    full[0] = 0;
    if (final_path != NULL) {
        n = final_path(h, full, (DWORD)(sizeof(full) / sizeof(full[0])), 0);
        if (n == 0 || n >= sizeof(full) / sizeof(full[0]))
            full[0] = 0;
    }
    CloseHandle(h);
    adv = LoadLibraryW(L"advapi32.dll");
    enc = adv ? (PFN_ENCRYPT)GetProcAddress(adv, "EncryptFileW") : NULL;
    if (enc == NULL) {
        Line("Efs", "EncryptFileW missing");
        return 0;
    }
    if (full[0] != 0 && enc(full)) {
        Line("Efs", "EncryptFileW finalpath ok");
    } else if (enc(path)) {
        Line("Efs", "EncryptFileW path ok");
    } else {
        LineErr("Efs", "EncryptFileW", GetLastError());
        return 0;
    }
    attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_ENCRYPTED) == 0) {
        Line("Efs", "FILE_ATTRIBUTE_ENCRYPTED clear");
        return 0;
    }
    Line("Efs", "FILE_ATTRIBUTE_ENCRYPTED set");
    return 1;
}

static int DecryptPath(const WCHAR *rel)
{
    HMODULE adv;
    HMODULE k32;
    PFN_DECRYPT dec;
    PFN_FINALPATH final_path;
    WCHAR path[260];
    WCHAR full[520];
    HANDLE h;
    DWORD n;

    PathOnVol(path, rel);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* Encrypted files still open without the reparse flag. */
        h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    full[0] = 0;
    if (h != INVALID_HANDLE_VALUE) {
        k32 = GetModuleHandleW(L"kernel32.dll");
        final_path = k32 ? (PFN_FINALPATH)GetProcAddress(k32, "GetFinalPathNameByHandleW") : NULL;
        if (final_path != NULL) {
            n = final_path(h, full, (DWORD)(sizeof(full) / sizeof(full[0])), 0);
            if (n == 0 || n >= sizeof(full) / sizeof(full[0]))
                full[0] = 0;
        }
        CloseHandle(h);
    }
    adv = LoadLibraryW(L"advapi32.dll");
    dec = adv ? (PFN_DECRYPT)GetProcAddress(adv, "DecryptFileW") : NULL;
    if (dec == NULL) {
        Line("Efs", "DecryptFileW missing");
        return 0;
    }
    if (full[0] != 0 && dec(full, 0)) {
        Line("Efs", "DecryptFileW finalpath ok");
        return 1;
    }
    if (dec(path, 0)) {
        Line("Efs", "DecryptFileW path ok");
        return 1;
    }
    LineErr("Efs", "DecryptFileW", GetLastError());
    return 0;
}

/* len==0 → empty file. Pattern plain[i] = i*mul+add. */
static int WriteSecret(const WCHAR *rel, DWORD len, UCHAR mul, UCHAR add)
{
    WCHAR path[260];
    HANDLE h;
    DWORD wrote = 0;
    DWORD i;
    UCHAR *buf;

    PathOnVol(path, rel);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LineErr("Efs", "create", GetLastError());
        return 0;
    }
    if (len == 0) {
        FlushFileBuffers(h);
        CloseHandle(h);
        Line("Efs", "write empty ok");
        return 1;
    }
    buf = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, len);
    if (buf == NULL) {
        CloseHandle(h);
        Line("Efs", "oom");
        return 0;
    }
    for (i = 0; i < len; i++)
        buf[i] = (UCHAR)(i * mul + add);
    if (!WriteFile(h, buf, len, &wrote, NULL) || wrote != len) {
        HeapFree(GetProcessHeap(), 0, buf);
        CloseHandle(h);
        LineErr("Efs", "write", GetLastError());
        return 0;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    FlushFileBuffers(h);
    CloseHandle(h);
    Line("Efs", "write ok");
    return 1;
}

static int StageZ(void)
{
    if (!EnsureEfsDir() || !WriteSecret(L"efs\\secretA.txt", 0, 0, 0) || !EncryptPath(L"efs\\secretA.txt"))
        return 0;
    Line("Stage", "stop after empty encrypt");
    return 1;
}

static int StageS(void)
{
    if (!EnsureEfsDir() || !WriteSecret(L"efs\\secretA.txt", 512, 3, 1) || !EncryptPath(L"efs\\secretA.txt"))
        return 0;
    Line("Stage", "stop after 512 encrypt");
    return 1;
}

static int StageL(void)
{
    if (!EnsureEfsDir() || !WriteSecret(L"efs\\secretA.txt", 2048, 3, 1) || !EncryptPath(L"efs\\secretA.txt"))
        return 0;
    Line("Stage", "stop after 2048 encrypt");
    return 1;
}

static int StageP(void)
{
    if (!EnsureEfsDir())
        return 0;
    if (!WriteSecret(L"efs\\secretA.txt", 512, 3, 1) || !EncryptPath(L"efs\\secretA.txt"))
        return 0;
    Line("Efs", "secretA ok");
    if (!WriteSecret(L"efs\\secretB.txt", 512, 5, 7) || !EncryptPath(L"efs\\secretB.txt"))
        return 0;
    Line("Efs", "secretB ok");
    Line("Stage", "stop after same-boot pair");
    return 1;
}

static int StageR(void)
{
    if (!EnsureEfsDir())
        return 0;
    if (!WriteSecret(L"efs\\secretA.txt", 512, 3, 1) || !EncryptPath(L"efs\\secretA.txt"))
        return 0;
    Line("Efs", "first encrypt ok");
    if (!DecryptPath(L"efs\\secretA.txt"))
        return 0;
    if (!WriteSecret(L"efs\\secretA.txt", 512, 7, 11) || !EncryptPath(L"efs\\secretA.txt"))
        return 0;
    Line("Stage", "stop after re-encrypt");
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

    h = CreateFileW(L"C:\\temp\\ntfs_efs_stage.txt", GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        if (buf[0] == 'Z' || buf[0] == 'z')
            g_stage = 'Z';
        else if (buf[0] == 'L' || buf[0] == 'l')
            g_stage = 'L';
        else if (buf[0] == 'P' || buf[0] == 'p')
            g_stage = 'P';
        else if (buf[0] == 'R' || buf[0] == 'r')
            g_stage = 'R';
        else
            g_stage = 'S';
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
    Line("Probe", "ntfs-efs");
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
        if (g_stage == 'Z')
            ok = StageZ();
        else if (g_stage == 'L')
            ok = StageL();
        else if (g_stage == 'P')
            ok = StageP();
        else if (g_stage == 'R')
            ok = StageR();
        else
            ok = StageS();
    }
    FlushVol();
    Text(ok ? "NTFS-EFS-PROBE-DONE\tok\r\n" : "NTFS-EFS-PROBE-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(3000);
    ShutdownGuest();
    return ok ? 0 : 1;
}
