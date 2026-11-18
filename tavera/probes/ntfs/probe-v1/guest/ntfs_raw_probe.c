/*
 * PROJECT:     Tevera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v1/guest/ntfs_raw_probe.c
 * PURPOSE:     Windows guest probe that writes NTFS quota, EFS, and log traffic
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Raw-format probe session only. Runs on Windows 10 22H2 against a blank
 * attached disk. Documented APIs only. Captures are taken on the host from
 * the resulting raw image (see tavera/probes/ntfs/probe-v1/host). Do not edit
 * drivers/filesystems/ntfs from this probe.
 *
 * Sources: Learn EncryptFile / FSCTL_SET_QUOTA / FormatEx usage already
 * exercised by unittests/ntfs/agent/ntfs_agent.c; MS-FSCC reserved names.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);
typedef BOOL (WINAPI *PFN_ENCRYPT)(LPCWSTR);
typedef LONG (NTAPI *PFN_SHUTDOWN)(ULONG);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_share[8];
static WCHAR g_letter[8];
static ULONG g_fmt_bad;

static void Text(const char *s)
{
    DWORD wrote = 0;
    if (g_log == INVALID_HANDLE_VALUE || s == NULL)
        return;
    WriteFile(g_log, s, (DWORD)lstrlenA(s), &wrote, NULL);
    FlushFileBuffers(g_log);
}

static void Line(const char *tag, const char *detail)
{
    Text(tag);
    Text("\t");
    Text(detail ? detail : "");
    Text("\r\n");
}

static BOOLEAN NTAPI FormatCb(ULONG Command, ULONG SubAction, PVOID ActionInfo)
{
    (void)SubAction;
    (void)ActionInfo;
    if (Command == 0x0B)
        g_fmt_bad = 1;
    return TRUE;
}

static int WEq(const WCHAR *a, const WCHAR *b)
{
    return lstrcmpiW(a, b) == 0;
}

static void FindShare(void)
{
    WCHAR root[] = L"A:\\";
    WCHAR name[32];
    DWORD bits = GetLogicalDrives();
    int i;

    g_share[0] = 0;
    for (i = 0; i < 26; i++) {
        if (((bits >> i) & 1) == 0)
            continue;
        root[0] = (WCHAR)(L'A' + i);
        if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, name, 32))
            continue;
        if (WEq(name, L"FAT") || WEq(name, L"FAT32")) {
            g_share[0] = root[0];
            g_share[1] = L':';
            g_share[2] = 0;
            return;
        }
    }
}

static int FindBlankDisk(void)
{
    WCHAR root[] = L"A:\\";
    WCHAR name[32];
    DWORD bits = GetLogicalDrives();
    DWORD flags = 0;
    int i;

    g_letter[0] = 0;
    for (i = 0; i < 26; i++) {
        if (((bits >> i) & 1) == 0)
            continue;
        root[0] = (WCHAR)(L'A' + i);
        if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, name, 32)) {
            /* Unformatted / raw volume often fails GetVolumeInformation. */
            g_letter[0] = root[0];
            g_letter[1] = L':';
            g_letter[2] = 0;
            return 1;
        }
        if (name[0] == 0) {
            g_letter[0] = root[0];
            g_letter[1] = L':';
            g_letter[2] = 0;
            return 1;
        }
    }
    return 0;
}

static int FormatVolume(void)
{
    HMODULE fmifs;
    PFN_FORMATEX format_ex;
    WCHAR root[8];

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
    g_fmt_bad = 0;
    root[0] = g_letter[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = 0;
    format_ex(root, FMIFS_HARDDISK, L"NTFS", L"RAWPROBE", TRUE, 0, FormatCb);
    if (g_fmt_bad) {
        Line("Format", "FormatEx reported failure");
        return 0;
    }
    Line("Format", "ok");
    return 1;
}

static void PathOnVol(WCHAR *out, const WCHAR *name)
{
    out[0] = g_letter[0];
    out[1] = L':';
    out[2] = L'\\';
    out[3] = 0;
    lstrcatW(out, name);
}

static int WriteTraffic(void)
{
    WCHAR path[260];
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
    /* Rename and delete to push more metadata through $LogFile. */
    {
        WCHAR other[260];
        PathOnVol(other, L"logtraffic2.bin");
        if (!MoveFileW(path, other)) {
            Line("LogTraffic", "rename failed");
            return 0;
        }
        if (!DeleteFileW(other)) {
            Line("LogTraffic", "delete failed");
            return 0;
        }
    }
    Line("LogTraffic", "ok");
    return 1;
}

static int SetQuotaSample(void)
{
    UCHAR info[64];
    DWORD got = 0;
    WCHAR path[260];
    HANDLE h;

    PathOnVol(path, L"quota.txt");
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Line("Quota", "open failed");
        return 0;
    }
    ZeroMemory(info, sizeof(info));
    /* FSCTL_SET_QUOTA = CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_BUFFERED, FILE_WRITE_DATA) */
    if (!DeviceIoControl(h, 0x00098080, info, sizeof(info), NULL, 0, &got, NULL)) {
        Line("Quota", "FSCTL_SET_QUOTA empty failed");
        CloseHandle(h);
        return 0;
    }
    ZeroMemory(info, sizeof(info));
    *(ULONG *)(info + 4) = 12;
    *(LONGLONG *)(info + 24) = 4096;
    *(LONGLONG *)(info + 32) = 8192;
    info[40] = 1;
    info[41] = 1;
    info[47] = 5;
    *(ULONG *)(info + 48) = 42;
    if (!DeviceIoControl(h, 0x00098080, info, 52, NULL, 0, &got, NULL)) {
        Line("Quota", "new owner failed");
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    Line("Quota", "ok");
    return 1;
}

static int EncryptSample(void)
{
    HMODULE adv;
    PFN_ENCRYPT enc;
    WCHAR path[260];
    HANDLE h;
    DWORD wrote = 0;
    UCHAR plain[512];
    DWORD i;

    for (i = 0; i < sizeof(plain); i++)
        plain[i] = (UCHAR)(i * 3 + 1);
    PathOnVol(path, L"secret.txt");
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, plain, sizeof(plain), &wrote, NULL) ||
        wrote != sizeof(plain)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Line("Encrypt", "write failed");
        return 0;
    }
    CloseHandle(h);
    adv = LoadLibraryW(L"advapi32.dll");
    enc = adv ? (PFN_ENCRYPT)GetProcAddress(adv, "EncryptFileW") : NULL;
    if (enc == NULL || !enc(path)) {
        Line("Encrypt", "EncryptFileW failed");
        return 0;
    }
    Line("Encrypt", "ok");
    return 1;
}

static void OpenLog(void)
{
    WCHAR path[64];

    path[0] = g_share[0];
    path[1] = L':';
    path[2] = L'\\';
    path[3] = 0;
    lstrcatW(path, L"result.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
}

static void ShutdownGuest(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_SHUTDOWN shutdown;

    if (ntdll == NULL)
        return;
    shutdown = (PFN_SHUTDOWN)GetProcAddress(ntdll, "NtShutdownSystem");
    if (shutdown != NULL)
        shutdown(1); /* ShutdownPowerOff */
}

void MainCRTStartup(void)
{
    int ok = 1;

    FindShare();
    if (g_share[0] == 0) {
        /* No FAT share: still try to leave a local log is useless; exit. */
        ExitProcess(2);
    }
    OpenLog();
    Line("Probe", "ntfs-raw");
    if (!FindBlankDisk()) {
        Line("Disk", "no blank volume");
        ok = 0;
    } else {
        Line("Disk", "found");
        if (!FormatVolume())
            ok = 0;
        else {
            if (!WriteTraffic())
                ok = 0;
            if (!SetQuotaSample())
                ok = 0;
            if (!EncryptSample())
                ok = 0;
        }
    }
    Text(ok ? "NTFS-RAW-PROBE-DONE\tok\r\n" : "NTFS-RAW-PROBE-DONE\tfail\r\n");
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    ShutdownGuest();
    ExitProcess(ok ? 0 : 1);
}
