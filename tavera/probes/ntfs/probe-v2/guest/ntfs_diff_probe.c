/*
 * PROJECT:     Tevera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v2/guest/ntfs_diff_probe.c
 * PURPOSE:     Differential NTFS raw probe (quota/EFS/log variants A vs B)
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Raw-format probe session only. Variant letter in C:\temp\ntfs_diff_variant.txt
 * ('A' or 'B'). Captures under output/ntfs/probe-v2/<variant>/ from host dump.
 * Do not edit drivers/filesystems/ntfs from this probe.
 *
 * Disk partition + FormatEx callback failure codes follow the same pattern as
 * unittests/ntfs/agent/ntfs_agent.c (in-tree). FormatEx lives in fmifs.dll.
 * Callback command numbers for failure match that agent (7/8/9/17); DONE is
 * not a failure (sdk/include/reactos/libs/fmifs/fmifs.h CALLBACKCOMMAND).
 *
 * Sources:
 * - https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-encryptfilew
 * - https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-quota
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/e5a70738-7ee4-46d9-a5f7-6644daa49a51
 * - MS-FSCC reserved / stream names; in-tree fmifs.h; unittests/ntfs/agent/ntfs_agent.c.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12
#ifndef FILE_SUPPORTS_ENCRYPTION
#define FILE_SUPPORTS_ENCRYPTION 0x00020000
#endif

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);
typedef BOOL (WINAPI *PFN_ENCRYPT)(LPCWSTR);
typedef DWORD (WINAPI *PFN_FINALPATH)(HANDLE, LPWSTR, DWORD, DWORD);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_letter[8];
static ULONG g_fmt_bad;
static char g_variant = 'A';
static ULONG g_quota_thresh = 4096;
static ULONG g_quota_limit = 8192;

static void Text(const char *s)
{
    DWORD wrote = 0;
    if (g_log == INVALID_HANDLE_VALUE || s == NULL)
        return;
    WriteFile(g_log, s, (DWORD)lstrlenA(s), &wrote, NULL);
    /* Serial needs no flush storm; volume log still flushes at close. */
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

/* Same failure commands as unittests/ntfs/agent/ntfs_agent.c FmtCb. */
static BOOLEAN NTAPI FormatCb(ULONG Command, ULONG SubAction, PVOID ActionInfo)
{
    char buf[16];
    (void)SubAction;
    (void)ActionInfo;
    /* Log only non-progress commands (15 = STRUCTUREPROGRESS in fmifs.h). */
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

/* Same size window as unittests/ntfs/agent/ntfs_agent.c (suite uses 64 MiB). */
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
    WCopy(label, L"RAWPROBE");
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

static int WriteTraffic(void)
{
    WCHAR path[260];
    WCHAR other[260];
    HANDLE h;
    DWORD wrote = 0;
    char body[4096];
    DWORD i;

    for (i = 0; i < sizeof(body); i++)
        body[i] = (char)(i * 7 + 3);

    PathOnVol(path, L"logtraffic.bin");
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Line("LogTraffic", "create failed");
        return 0;
    }
    if (!WriteFile(h, body, sizeof(body), &wrote, NULL) || wrote != sizeof(body)) {
        CloseHandle(h);
        Line("LogTraffic", "write failed");
        return 0;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    /* Variant A: create only (file remains). Variant B: rename+delete. */
    if (g_variant == 'A') {
        Line("LogTraffic", "create-only");
        return 1;
    }
    PathOnVol(other, L"logtraffic2.bin");
    if (!MoveFileW(path, other)) {
        Line("LogTraffic", "rename failed");
        return 0;
    }
    if (!DeleteFileW(other)) {
        Line("LogTraffic", "delete failed");
        return 0;
    }
    Line("LogTraffic", "create-rename-delete");
    return 1;
}

static void LineErr(const char *tag, const char *detail, DWORD err)
{
    char buf[48];
    char num[16];
    int i = 0;
    const char *p = detail ? detail : "";
    while (*p && i < 24) {
        buf[i++] = *p++;
    }
    buf[i++] = ' ';
    buf[i++] = 'e';
    buf[i++] = '=';
    U32Text(err, num);
    p = num;
    while (*p && i < 47)
        buf[i++] = *p++;
    buf[i] = 0;
    Line(tag, buf);
}

static HANDLE OpenVol(void)
{
    WCHAR path[16];
    WCopy(path, L"\\\\.\\");
    WCat(path, g_letter);
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static int RunCmd(WCHAR *cmd, DWORD wait_ms)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    if (WaitForSingleObject(pi.hProcess, wait_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/*
 * Enable tracking with fsutil (Learn fsutil quota). MS-FSCC says a client set of
 * FILE_VC_QUOTA_TRACK via FileFsControlInformation is ignored; COM DiskQuota hung
 * on this guest, so use the documented fsutil path instead.
 */
static int SetQuotaSample(void)
{
    WCHAR cmd[96];
    HANDLE vol;

    WCopy(cmd, L"fsutil.exe quota track ");
    WCat(cmd, g_letter);
    Line("Quota", "fsutil track");
    if (!RunCmd(cmd, 120000)) {
        LineErr("Quota", "fsutil track failed", GetLastError());
        return 0;
    }
    Line("Quota", "fsutil track ok");

    /* Learn fsutil quota modify: threshold limit username (bytes). */
    WCopy(cmd, L"fsutil.exe quota modify ");
    WCat(cmd, g_letter);
    WCat(cmd, L" ");
    {
        char num[16];
        WCHAR wnum[16];
        int i;
        U32Text(g_quota_thresh, num);
        for (i = 0; num[i]; i++)
            wnum[i] = (WCHAR)num[i];
        wnum[i] = 0;
        WCat(cmd, wnum);
        WCat(cmd, L" ");
        U32Text(g_quota_limit, num);
        for (i = 0; num[i]; i++)
            wnum[i] = (WCHAR)num[i];
        wnum[i] = 0;
        WCat(cmd, wnum);
        WCat(cmd, L" ");
    }
    {
        WCHAR user[128];
        DWORD n = (DWORD)(sizeof(user) / sizeof(user[0]));
        if (!GetUserNameW(user, &n)) {
            LineErr("Quota", "GetUserNameW", GetLastError());
            return 0;
        }
        WCat(cmd, user);
    }
    Line("Quota", "fsutil modify");
    if (!RunCmd(cmd, 120000)) {
        LineErr("Quota", "fsutil modify failed", GetLastError());
        return 0;
    }
    LineErr("Quota", "thresh", g_quota_thresh);
    LineErr("Quota", "limit", g_quota_limit);
    Line("Quota", "fsutil modify ok");

    vol = OpenVol();
    if (vol != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(vol);
        CloseHandle(vol);
    }
    Line("Quota", "ok");
    return 1;
}

static int EncryptOne(const WCHAR *rel, UCHAR mul, UCHAR add)
{
    HMODULE k32;
    HMODULE adv;
    PFN_ENCRYPT enc;
    PFN_FINALPATH final_path;
    WCHAR path[260];
    WCHAR full[520];
    HANDLE h;
    DWORD wrote = 0;
    DWORD n;
    UCHAR plain[512];
    DWORD i;

    PathOnVol(path, rel);
    for (i = 0; i < sizeof(plain); i++)
        plain[i] = (UCHAR)(i * mul + add);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, plain, sizeof(plain), &wrote, NULL) ||
        wrote != sizeof(plain)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        LineErr("Encrypt", "write failed", GetLastError());
        return 0;
    }
    FlushFileBuffers(h);
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
        Line("Encrypt", "EncryptFileW missing");
        return 0;
    }
    if (full[0] != 0 && enc(full))
        return 1;
    if (enc(path))
        return 1;
    LineErr("Encrypt", "EncryptFileW", GetLastError());
    return 0;
}

static int EncryptSample(void)
{
    WCHAR dir[260];
    WCHAR root[8];
    DWORD flags = 0;

    WCopy(root, g_letter);
    WCat(root, L"\\");
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, NULL, 0)) {
        LineErr("Encrypt", "GetVolumeInformation", GetLastError());
        return 0;
    }
    if ((flags & FILE_SUPPORTS_ENCRYPTION) == 0) {
        Line("Encrypt", "volume lacks FS_FILE_ENCRYPTION");
        return 0;
    }
    Line("Encrypt", "FS_FILE_ENCRYPTION set");
    PathOnVol(dir, L"efs");
    CreateDirectoryW(dir, NULL);
    /* Pattern A: i*3+1. Pattern B file: i*5+7 (same-boot EFS diff). */
    if (!EncryptOne(L"efs\\secretA.txt", 3, 1))
        return 0;
    Line("Encrypt", "secretA ok");
    if (!EncryptOne(L"efs\\secretB.txt", 5, 7))
        return 0;
    Line("Encrypt", "secretB ok");
    return 1;
}

static void OpenLog(void)
{
    /* Host captures via QEMU -serial file: (avoid fat:rw, which aborted this guest). */
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

static void LoadVariant(void)
{
    HANDLE h;
    char buf[8];
    DWORD got = 0;

    h = CreateFileW(L"C:\\temp\\ntfs_diff_variant.txt", GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        if (buf[0] == 'B' || buf[0] == 'b') {
            g_variant = 'B';
            g_quota_thresh = 8192;
            g_quota_limit = 16384;
        } else {
            g_variant = 'A';
            g_quota_thresh = 4096;
            g_quota_limit = 8192;
        }
    }
    CloseHandle(h);
}

int main(void)
{
    int ok = 1;
    DWORD disk = 0;
    char vtag[2];

    OpenLog();
    if (g_log == INVALID_HANDLE_VALUE)
        return 2;
    LoadVariant();
    vtag[0] = g_variant;
    vtag[1] = 0;
    Line("Probe", "ntfs-diff");
    Line("Variant", vtag);
    if (!PickDisk(&disk)) {
        Line("Disk", "no blank disk");
        ok = 0;
    } else if (!PartitionDisk(disk)) {
        Line("Disk", "partition failed");
        ok = 0;
    } else {
        Line("Disk", "ready");
        if (!FormatVolume(disk))
            ok = 0;
        else {
            if (!WriteTraffic())
                ok = 0;
            Text(ok ? "NTFS-DIFF-PARTIAL\tlog-ok\r\n" : "NTFS-DIFF-PARTIAL\tlog-fail\r\n");
            if (!SetQuotaSample())
                ok = 0;
            if (!EncryptSample())
                ok = 0;
        }
    }
    {
        HANDLE vol = OpenVol();
        if (vol != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(vol);
            CloseHandle(vol);
        }
    }
    Text(ok ? "NTFS-DIFF-PROBE-DONE\tok\r\n" : "NTFS-DIFF-PROBE-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(3000);
    ShutdownGuest();
    return ok ? 0 : 1;
}
