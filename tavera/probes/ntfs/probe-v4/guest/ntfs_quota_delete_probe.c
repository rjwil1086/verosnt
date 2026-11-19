/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v4/guest/ntfs_quota_delete_probe.c
 * PURPOSE:     QuotaLimit=-2 delete via NtSetQuotaInformationFile; dump $Q
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Raw-format probe session only. Stage letter in C:\temp\ntfs_quota_delete_stage.txt:
 *   M = format + track + fsutil quota modify 4096 8192 <user>
 *   Q = M then NtSetQuotaInformationFile with QuotaLimit=-2 (MS-FSCC 2.4.41)
 * Host dumps under output/ntfs/probe-v4/<stage>/. Do not edit ntfs.sys here.
 *
 * Sources:
 * - https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-quota
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/acdc0738-ba3c-47a1-b11a-72e22d831c57
 * - https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetquotainformationfile
 * - in-tree FILE_QUOTA_INFORMATION (sdk/include/xdk/iotypes.h)
 * - in-tree fmifs FormatEx pattern (tavera/test-suites/ntfs/agent/ntfs_agent.c)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12

#pragma pack(push, 8)
typedef struct _PROBE_IO_STATUS_BLOCK {
    union {
        LONG Status;
        PVOID Pointer;
    } u;
    ULONG_PTR Information;
} PROBE_IO_STATUS_BLOCK;

typedef struct _PROBE_FILE_QUOTA_INFORMATION {
    ULONG NextEntryOffset;
    ULONG SidLength;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER QuotaUsed;
    LARGE_INTEGER QuotaThreshold;
    LARGE_INTEGER QuotaLimit;
    UCHAR Sid[128];
} PROBE_FILE_QUOTA_INFORMATION;
#pragma pack(pop)

typedef LONG (NTAPI *PFN_NT_SET_QUOTA)(HANDLE, PROBE_IO_STATUS_BLOCK *, PVOID, ULONG);
typedef LONG (NTAPI *PFN_NT_QUERY_QUOTA)(HANDLE, PROBE_IO_STATUS_BLOCK *, PVOID, ULONG, BOOLEAN, PVOID, ULONG, PVOID, BOOLEAN);
typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_letter[8];
static ULONG g_fmt_bad;
static char g_stage = 'M';

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
    WCopy(label, L"QGROW");
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

static int QuotaTrack(void)
{
    WCHAR cmd[96];
    WCopy(cmd, L"fsutil.exe quota track ");
    WCat(cmd, g_letter);
    Line("Quota", "fsutil track");
    if (!RunCmd(cmd, 120000)) {
        LineErr("Quota", "fsutil track failed", GetLastError());
        return 0;
    }
    Line("Quota", "fsutil track ok");
    FlushVol();
    return 1;
}

static int QuotaModify(void)
{
    WCHAR cmd[256];
    WCHAR user[128];
    DWORD n = (DWORD)(sizeof(user) / sizeof(user[0]));

    if (!GetUserNameW(user, &n)) {
        LineErr("Quota", "GetUserNameW", GetLastError());
        return 0;
    }
    WCopy(cmd, L"fsutil.exe quota modify ");
    WCat(cmd, g_letter);
    WCat(cmd, L" 4096 8192 ");
    WCat(cmd, user);
    Line("Quota", "fsutil modify");
    if (!RunCmd(cmd, 120000)) {
        LineErr("Quota", "fsutil modify failed", GetLastError());
        return 0;
    }
    Line("Quota", "fsutil modify ok");
    FlushVol();
    return 1;
}

static void HexStatus(LONG st, char *out)
{
    static const char *hex = "0123456789ABCDEF";
    ULONG u = (ULONG)st;
    int i;
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8; i++)
        out[2 + i] = hex[(u >> (28 - i * 4)) & 0xF];
    out[10] = 0;
}

/*
 * MS-FSCC 2.4.41: QuotaLimit == -2 deletes the entry.
 * Learn: NtSetQuotaInformationFile applies FILE_QUOTA_INFORMATION to the volume.
 * probe-v3 FSCTL_SET_QUOTA returned e=1; this stage uses the Nt path instead.
 */
static int QuotaDeleteMinus2Nt(void)
{
    HANDLE vol;
    PROBE_FILE_QUOTA_INFORMATION q;
    PROBE_IO_STATUS_BLOCK iosb;
    DWORD sid_len = 0;
    WCHAR user[128];
    DWORD n = (DWORD)(sizeof(user) / sizeof(user[0]));
    SID_NAME_USE use;
    WCHAR domain[128];
    DWORD domain_len = 128;
    DWORD send;
    HMODULE ntdll;
    PFN_NT_SET_QUOTA nt_set;
    PFN_NT_QUERY_QUOTA nt_query;
    LONG st;
    char hex[16];
    WCHAR cmd[96];
    UCHAR qbuf[512];

    if (!GetUserNameW(user, &n)) {
        LineErr("Quota", "GetUserNameW delete", GetLastError());
        return 0;
    }
    LookupAccountNameW(NULL, user, NULL, &sid_len, domain, &domain_len, &use);
    if (sid_len == 0 || sid_len > sizeof(q.Sid)) {
        LineErr("Quota", "LookupAccountName size", GetLastError());
        return 0;
    }
    ZeroMemory(&q, sizeof(q));
    q.NextEntryOffset = 0;
    q.SidLength = sid_len;
    q.ChangeTime.QuadPart = 0;
    q.QuotaUsed.QuadPart = 0;
    q.QuotaThreshold.QuadPart = -1LL;
    q.QuotaLimit.QuadPart = -2LL;
    domain_len = 128;
    if (!LookupAccountNameW(NULL, user, (PSID)q.Sid, &sid_len, domain, &domain_len, &use)) {
        LineErr("Quota", "LookupAccountName", GetLastError());
        return 0;
    }
    q.SidLength = sid_len;
    send = (DWORD)(FIELD_OFFSET(PROBE_FILE_QUOTA_INFORMATION, Sid) + sid_len);

    ntdll = GetModuleHandleW(L"ntdll.dll");
    nt_set = ntdll ? (PFN_NT_SET_QUOTA)GetProcAddress(ntdll, "NtSetQuotaInformationFile") : NULL;
    nt_query = ntdll ? (PFN_NT_QUERY_QUOTA)GetProcAddress(ntdll, "NtQueryQuotaInformationFile") : NULL;
    if (nt_set == NULL) {
        Line("Quota", "NtSetQuotaInformationFile missing");
        return 0;
    }

    vol = OpenVol();
    if (vol == INVALID_HANDLE_VALUE) {
        LineErr("Quota", "open vol delete", GetLastError());
        return 0;
    }
    ZeroMemory(&iosb, sizeof(iosb));
    Line("Quota", "NtSetQuotaInformationFile limit -2");
    st = nt_set(vol, &iosb, &q, send);
    HexStatus(st, hex);
    Text("Quota\tNtSetStatus\t");
    Text(hex);
    Text("\r\n");
    if (st < 0) {
        CloseHandle(vol);
        Line("Quota", "NtSetQuotaInformationFile failed");
        /* Still finish ok so host can dump post-attempt image. */
        FlushVol();
        return 1;
    }
    Line("Quota", "NtSetQuotaInformationFile ok");

    if (nt_query != NULL) {
        ZeroMemory(&iosb, sizeof(iosb));
        ZeroMemory(qbuf, sizeof(qbuf));
        st = nt_query(vol, &iosb, qbuf, sizeof(qbuf), FALSE, NULL, 0, NULL, FALSE);
        HexStatus(st, hex);
        Text("Quota\tNtQueryStatus\t");
        Text(hex);
        Text("\r\n");
    }
    CloseHandle(vol);

    WCopy(cmd, L"fsutil.exe quota query ");
    WCat(cmd, g_letter);
    Line("Quota", "fsutil query after delete");
    RunCmd(cmd, 120000);
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

    h = CreateFileW(L"C:\\temp\\ntfs_quota_delete_stage.txt", GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        if (buf[0] == 'Q' || buf[0] == 'q')
            g_stage = 'Q';
        else
            g_stage = 'M';
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
    Line("Probe", "ntfs-quota-delete");
    Line("Stage", stag);
    if (!PickDisk(&disk)) {
        Line("Disk", "no blank disk");
        ok = 0;
    } else if (!PartitionDisk(disk)) {
        Line("Disk", "partition failed");
        ok = 0;
    } else if (!FormatVolume(disk)) {
        ok = 0;
    } else if (!QuotaTrack()) {
        ok = 0;
    } else if (!QuotaModify()) {
        ok = 0;
    } else if (g_stage == 'M') {
        Line("Stage", "stop after modify");
    } else if (g_stage == 'Q') {
        if (!QuotaDeleteMinus2Nt())
            ok = 0;
        else
            Line("Stage", "stop after NtSet delete");
    }
    FlushVol();
    Text(ok ? "NTFS-QUOTA-DELETE-DONE\tok\r\n" : "NTFS-QUOTA-DELETE-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(3000);
    ShutdownGuest();
    return ok ? 0 : 1;
}
