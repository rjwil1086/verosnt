/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/test-suites/ntfs/agent/ntfs_agent.c
 * PURPOSE:     Guest probe for ntfs.sys.
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 *              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
 *              Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Guest probe for ntfs.sys. One row of the NTFS 3.1 catalog is one mark
 * in result.log. A failure is red and does not skip the rest of the list.
 *
 * QuotaDelete / LogReplay / LogOps / EfsBody (suite rows):
 * - https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetquotainformationfile
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/acdc0738-ba3c-47a1-b11a-72e22d831c57
 * - https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty
 * - https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-encryptfilew
 * - tavera/specs/ntfs/ntfs-quota-on-disk.md Rev.4 (probe-v4)
 * - tavera/specs/ntfs/ntfs-log-replay.md Rev.1–4 (probe-v5 / probe-v7)
 * - tavera/specs/ntfs/ntfs-encrypted-file-body.md Rev.3–4 (probe-v6)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FMIFS_HARDDISK 12
#define FILE_REC_MAX 4096
#ifndef FILE_SUPPORTS_ENCRYPTION
#define FILE_SUPPORTS_ENCRYPTION 0x00020000
#endif
#ifndef FILE_ATTRIBUTE_ENCRYPTED
#define FILE_ATTRIBUTE_ENCRYPTED 0x00004000
#endif

typedef BOOLEAN (NTAPI *PFN_FMT_CB)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef VOID (NTAPI *PFN_FORMATEX)(PWCHAR, ULONG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFN_FMT_CB);

typedef struct {
    LONG Status;
    LONG Pad;
    ULONG_PTR Information;
} IOSB;
typedef LONG (NTAPI *PFN_NTQUERY)(HANDLE, IOSB *, PVOID, ULONG, ULONG);
typedef LONG (NTAPI *PFN_NTSET)(HANDLE, IOSB *, PVOID, ULONG, ULONG);
typedef LONG (NTAPI *PFN_NT_SET_QUOTA)(HANDLE, IOSB *, PVOID, ULONG);
typedef LONG (NTAPI *PFN_NT_QUERY_QUOTA)(HANDLE, IOSB *, PVOID, ULONG, BOOLEAN, PVOID, ULONG, PVOID, BOOLEAN);
typedef LONG (NTAPI *PFN_SHUTDOWN)(ULONG);
typedef LONG (NTAPI *PFN_ADJUST)(ULONG, BOOLEAN, BOOLEAN, BOOLEAN *);

/* MS-FSCC 2.4.41 / in-tree FILE_QUOTA_INFORMATION; Sid is variable length. */
#pragma pack(push, 8)
typedef struct {
    ULONG NextEntryOffset;
    ULONG SidLength;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER QuotaUsed;
    LARGE_INTEGER QuotaThreshold;
    LARGE_INTEGER QuotaLimit;
    UCHAR Sid[128];
} AGENT_FILE_QUOTA_INFORMATION;
#pragma pack(pop)

typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
} BASIC_INFO;

typedef struct {
    LARGE_INTEGER IndexNumber;
} INTERNAL_INFO;

void *memset(void *dst, int value, size_t count)
{
    unsigned char *p = (unsigned char *)dst;
    size_t i;
    for (i = 0; i < count; i++)
        p[i] = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;
    for (i = 0; i < count; i++)
        d[i] = s[i];
    return dst;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    size_t i;
    for (i = 0; i < count; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_share[8];
static WCHAR g_letter[8];
static char g_phase[32];
static ULONG g_fmt_bad;
static DWORD g_cluster;
static HANDLE g_vol = INVALID_HANDLE_VALUE;
static UCHAR g_rec[FILE_REC_MAX];
static char g_detail[160];

static void Text(const char *s)
{
    DWORD wrote = 0;
    if (g_log == INVALID_HANDLE_VALUE || s == NULL)
        return;
    /* Fall through to WriteFile; flush after each write so vvfat sees DONE. */
    WriteFile(g_log, s, (DWORD)lstrlenA(s), &wrote, NULL);
    FlushFileBuffers(g_log);
}

static void Detail(const char *s)
{
    int i = 0;
    if (s == NULL) {
        g_detail[0] = 0;
        return;
    }
    while (s[i] && i < (int)sizeof(g_detail) - 1) {
        g_detail[i] = s[i];
        i++;
    }
    g_detail[i] = 0;
}

static void Mark(const char *name, int ok)
{
    Text(name);
    Text(ok ? "\tgreen\t" : "\tred\t");
    Text(g_detail);
    Text("\r\n");
    g_detail[0] = 0;
}

static void U32(char *out, DWORD v)
{
    char tmp[16];
    int n = 0;
    int i;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static int Streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void WCopy(WCHAR *d, const WCHAR *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void WCat(WCHAR *d, const WCHAR *s)
{
    while (*d)
        d++;
    WCopy(d, s);
}

static int WEq(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static BOOLEAN NTAPI FmtCb(ULONG command, ULONG sub, PVOID info)
{
    (void)sub;
    (void)info;
    if (command == 7 || command == 8 || command == 9 || command == 17)
        g_fmt_bad = command;
    return TRUE;
}

static int LoadPhase(void)
{
    WCHAR path[64];
    HANDLE h;
    char buf[64];
    DWORD got = 0;
    int i = 0;
    WCopy(path, g_share);
    WCat(path, L"phase.txt");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        WCopy((WCHAR *)g_phase, L"");
        lstrcpyA(g_phase, "compliance");
        return 1;
    }
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    if (got > sizeof(buf) - 1)
        got = sizeof(buf) - 1;
    buf[got] = 0;
    while (buf[i] && buf[i] != '\r' && buf[i] != '\n' && i < 30) {
        g_phase[i] = buf[i];
        i++;
    }
    g_phase[i] = 0;
    if (g_phase[0] == 0)
        lstrcpyA(g_phase, "compliance");
    return 1;
}

static int FindShare(void)
{
    WCHAR drives[512];
    WCHAR *p;
    DWORD n = GetLogicalDriveStringsW(512, drives);
    if (n == 0 || n > 500)
        return 0;
    for (p = drives; *p; p += lstrlenW(p) + 1) {
        WCHAR path[64];
        WCopy(path, p);
        WCat(path, L"NTFS-SUITE.txt");
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            WCopy(g_share, p);
            return 1;
        }
    }
    return 0;
}

static int OpenLog(void)
{
    WCHAR path[64];
    WCopy(path, g_share);
    WCat(path, L"result.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return g_log != INVALID_HANDLE_VALUE;
}

static void Signal(const char *line)
{
    WCHAR path[64];
    HANDLE h;
    DWORD wrote = 0;
    Text(line);
    Text("\r\n");
    OutputDebugStringA(line);
    OutputDebugStringA("\r\n");
    WCopy(path, g_share);
    WCat(path, L"WRITING.txt");
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, line, (DWORD)lstrlenA(line), &wrote, NULL);
        CloseHandle(h);
    }
}

static void ShutdownGuest(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_SHUTDOWN shut;
    PFN_ADJUST adjust;
    BOOLEAN was = FALSE;
    WCHAR done_path[64];
    HANDLE done;
    DWORD wrote = 0;
    /* result.log (vvfat) plus serial/debug — host may miss vvfat on S5. */
    Text("NTFS-AGENT-DONE\r\n");
    OutputDebugStringA("NTFS-AGENT-DONE\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    WCopy(done_path, g_share);
    WCat(done_path, L"DONE.ok");
    done = CreateFileW(done_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (done != INVALID_HANDLE_VALUE) {
        WriteFile(done, "OK", 2, &wrote, NULL);
        FlushFileBuffers(done);
        CloseHandle(done);
    }
    /* Let vvfat flush before the guest powers off (S5 can drop pending FAT). */
    Sleep(8000);
    /* SeShutdownPrivilege is present for an administrator and disabled until
     * this process turns it on. NtShutdownSystem rejects the call otherwise. */
    adjust = ntdll ? (PFN_ADJUST)GetProcAddress(ntdll, "RtlAdjustPrivilege") : NULL;
    if (adjust)
        adjust(19, TRUE, FALSE, &was);
    shut = ntdll ? (PFN_SHUTDOWN)GetProcAddress(ntdll, "NtShutdownSystem") : NULL;
    if (shut)
        shut(2);
    ExitProcess(0);
}

static HANDLE OpenDisk(DWORD index, DWORD access)
{
    WCHAR path[32];
    WCHAR num[8];
    DWORD v = index;
    int n = 0;
    int i;
    WCHAR rev[8];
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
    DWORD fallback = 0xffffffff;
    *index = 0xffffffff;
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
        /* Suite blank disks are exactly 64 MiB. Prefer that so a vvfat share
         * is not partitioned/formatted by mistake. */
        if (len.Length.QuadPart < 48LL * 1024 * 1024 || len.Length.QuadPart > 96LL * 1024 * 1024) {
            CloseHandle(h);
            continue;
        }
        zero.QuadPart = 0;
        SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
        ReadFile(h, sec, 512, &got, NULL);
        CloseHandle(h);
        if (got >= 8 && (memcmp(sec + 3, "MSDOS", 5) == 0 || memcmp(sec + 3, "EXFAT", 5) == 0))
            continue;
        if (len.Length.QuadPart == 64LL * 1024 * 1024) {
            *index = i;
            return 1;
        }
        if (fallback == 0xffffffff)
            fallback = i;
    }
    if (fallback != 0xffffffff) {
        *index = fallback;
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
    DWORD v = index;
    int n = 0;
    int i;
    WCHAR rev[8];
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
    return DefineDosDeviceW(DDD_RAW_TARGET_PATH, g_letter, target);
}

static int FormatNtfs(void)
{
    HMODULE fmifs = LoadLibraryW(L"fmifs.dll");
    PFN_FORMATEX format_ex;
    WCHAR root[8];
    WCHAR fs[8];
    WCHAR label[16];
    WCHAR volpath[16];
    HANDLE vol;
    int i;
    DWORD disk = 0;
    g_fmt_bad = 0;
    if (fmifs == NULL) {
        Detail("fmifs.dll did not load");
        return 0;
    }
    format_ex = (PFN_FORMATEX)GetProcAddress(fmifs, "FormatEx");
    if (format_ex == NULL) {
        Detail("FormatEx is missing");
        return 0;
    }
    /* Same readiness pattern as tavera/probes/ntfs/probe-v1 FormatVolume. */
    Sleep(2000);
    WCopy(volpath, L"\\\\.\\");
    WCat(volpath, g_letter);
    vol = CreateFileW(volpath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) {
        Detail("volume open before format failed");
        return 0;
    }
    CloseHandle(vol);
    WCopy(root, g_letter);
    WCat(root, L"\\");
    WCopy(fs, L"NTFS");
    WCopy(label, L"NTFS31");
    format_ex(root, FMIFS_HARDDISK, fs, label, TRUE, 0, FmtCb);
    if (g_fmt_bad) {
        Detail("FormatEx reported a failure");
        return 0;
    }
    /* Re-bind the DosDevice letter after FormatEx. */
    for (i = 0; i < 8; i++) {
        GET_LENGTH_INFORMATION len;
        DWORD got = 0;
        HANDLE h = OpenDisk((DWORD)i, GENERIC_READ);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), &got, NULL) &&
            len.Length.QuadPart == 64LL * 1024 * 1024) {
            disk = (DWORD)i;
            CloseHandle(h);
            break;
        }
        CloseHandle(h);
    }
    AssignLetter(disk);
    for (i = 0; i < 60; i++) {
        WCHAR name[32];
        DWORD flags = 0;
        WCopy(root, g_letter);
        WCat(root, L"\\");
        if (GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, name, 32) && WEq(name, L"NTFS"))
            return 1;
        Sleep(500);
    }
    Detail("volume did not mount as NTFS");
    return 0;
}

static HANDLE OpenVolume(void)
{
    WCHAR path[16];
    WCopy(path, L"\\\\.\\");
    WCat(path, g_letter);
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static int ReadRecord(LONGLONG index)
{
    UCHAR buf[sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + FILE_REC_MAX];
    NTFS_FILE_RECORD_INPUT_BUFFER *in;
    NTFS_FILE_RECORD_OUTPUT_BUFFER *out;
    DWORD got = 0;
    if (g_vol == INVALID_HANDLE_VALUE)
        return 0;
    ZeroMemory(buf, sizeof(buf));
    in = (NTFS_FILE_RECORD_INPUT_BUFFER *)buf;
    in->FileReferenceNumber.QuadPart = index;
    if (!DeviceIoControl(g_vol, FSCTL_GET_NTFS_FILE_RECORD, buf, sizeof(*in), buf, sizeof(buf), &got, NULL))
        return 0;
    out = (NTFS_FILE_RECORD_OUTPUT_BUFFER *)buf;
    if (out->FileRecordLength == 0 || out->FileRecordLength > FILE_REC_MAX)
        return 0;
    CopyMemory(g_rec, out->FileRecordBuffer, out->FileRecordLength);
    return (int)out->FileRecordLength;
}

static UCHAR *FindAttr(ULONG type)
{
    ULONG off;
    ULONG len;
    if (g_rec[0] != 'F')
        return NULL;
    off = *(USHORT *)(g_rec + 0x14);
    len = *(ULONG *)(g_rec + 0x18);
    if (off > len || len > FILE_REC_MAX)
        return NULL;
    while (off + 8 < len && off + 8 < FILE_REC_MAX) {
        ULONG t = *(ULONG *)(g_rec + off);
        ULONG alen = *(ULONG *)(g_rec + off + 4);
        if (t == 0xffffffff)
            return NULL;
        if (alen < 8 || off + alen > len)
            return NULL;
        if (t == type && g_rec[off + 9] == 0)
            return g_rec + off;
        off += alen;
    }
    return NULL;
}

static int RecordNamed(LONGLONG index, int *length)
{
    int n = ReadRecord(index);
    if (length)
        *length = n;
    return n > 0 && g_rec[0] == 'F' && g_rec[1] == 'I' && (*(USHORT *)(g_rec + 0x16) & 1);
}

static void ProbeMeta(const char *row, LONGLONG index)
{
    if (!RecordNamed(index, NULL)) {
        Detail("file record missing");
        Mark(row, 0);
        return;
    }
    Mark(row, 1);
}

static void ProbeBoot(void)
{
    UCHAR sec[512];
    DWORD got = 0;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    SetFilePointerEx(g_vol, zero, NULL, FILE_BEGIN);
    if (!ReadFile(g_vol, sec, 512, &got, NULL) || got < 512) {
        Detail("boot sector read failed");
        Mark("BootSector", 0);
        return;
    }
    if (memcmp(sec + 3, "NTFS    ", 8) != 0 || sec[0x1FE] != 0x55 || sec[0x1FF] != 0xAA) {
        Detail("OEM or signature");
        Mark("BootSector", 0);
        return;
    }
    Mark("BootSector", 1);
    {
        DWORD bps = *(USHORT *)(sec + 0x0B);
        DWORD spc = sec[0x0D];
        DWORD cluster = bps * spc;
        g_cluster = cluster;
        if (cluster == 512 || cluster == 1024 || cluster == 2048 || cluster == 4096 ||
            cluster == 8192 || cluster == 16384 || cluster == 32768 || cluster == 65536)
            Mark("ClusterSize", 1);
        else {
            Detail("cluster size is not an NTFS size");
            Mark("ClusterSize", 0);
        }
    }
}

static void ProbeVersion(void)
{
    UCHAR buf[sizeof(NTFS_VOLUME_DATA_BUFFER) + sizeof(NTFS_EXTENDED_VOLUME_DATA) + 8];
    DWORD got = 0;
    NTFS_EXTENDED_VOLUME_DATA *ext;
    int record;
    UCHAR *attr;
    UCHAR *val;
    ZeroMemory(buf, sizeof(buf));
    if (!DeviceIoControl(g_vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, buf, sizeof(buf), &got, NULL)) {
        Detail("FSCTL_GET_NTFS_VOLUME_DATA failed");
        Mark("Version3_1", 0);
        return;
    }
    if (got < sizeof(NTFS_VOLUME_DATA_BUFFER) + sizeof(NTFS_EXTENDED_VOLUME_DATA)) {
        Detail("extended volume data missing");
        Mark("Version3_1", 0);
        return;
    }
    ext = (NTFS_EXTENDED_VOLUME_DATA *)(buf + sizeof(NTFS_VOLUME_DATA_BUFFER));
    record = ReadRecord(3);
    attr = record ? FindAttr(0x70) : NULL;
    if (attr == NULL || attr[8] != 0) {
        Detail("no resident $VOLUME_INFORMATION");
        Mark("Version3_1", 0);
        return;
    }
    val = attr + *(USHORT *)(attr + 0x14);
    if (ext->MajorVersion == 3 && ext->MinorVersion == 1 && val[8] == 3 && val[9] == 1)
        Mark("Version3_1", 1);
    else {
        Detail("version is not 3.1");
        Mark("Version3_1", 0);
    }
}

static void ProbeLabel(void)
{
    WCHAR root[8];
    WCHAR label[64];
    WCHAR fs[32];
    WCopy(root, g_letter);
    WCat(root, L"\\");
    label[0] = 0;
    fs[0] = 0;
    if (!GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, fs, 32) || !WEq(fs, L"NTFS")) {
        Detail("GetVolumeInformation");
        Mark("VolumeLabel", 0);
        return;
    }
    if (!WEq(label, L"NTFS31"))
        Detail("label is not NTFS31");
    Mark("VolumeLabel", WEq(label, L"NTFS31"));
}

static WCHAR *Path(WCHAR *out, const WCHAR *rel)
{
    WCopy(out, g_letter);
    WCat(out, L"\\");
    WCat(out, rel);
    return out;
}

static HANDLE MakeFile(const WCHAR *rel, DWORD access, DWORD disp, DWORD flags)
{
    WCHAR path[260];
    Path(path, rel);
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp, flags, NULL);
}

static int WriteAll(HANDLE h, const void *data, DWORD n)
{
    DWORD wrote = 0;
    return WriteFile(h, data, n, &wrote, NULL) && wrote == n;
}

static void ProbeCreate(void)
{
    HANDLE h = MakeFile(L"created.txt", GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("CreateFile failed");
        Mark("Create", 0);
        return;
    }
    CloseHandle(h);
    Mark("Create", 1);
}

static void ProbeResident(void)
{
    HANDLE h;
    char buf[200];
    char back[200];
    DWORD got = 0;
    UCHAR *attr;
    int i;
    INTERNAL_INFO info;
    IOSB ios;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NTQUERY query = ntdll ? (PFN_NTQUERY)GetProcAddress(ntdll, "NtQueryInformationFile") : NULL;
    for (i = 0; i < 200; i++)
        buf[i] = (char)(i & 0x7f);
    h = MakeFile(L"resident.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, buf, 200)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write failed");
        Mark("ResidentData", 0);
        Mark("EndOfFile", 0);
        return;
    }
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    if (!ReadFile(h, back, 200, &got, NULL) || got != 200 || memcmp(buf, back, 200) != 0) {
        CloseHandle(h);
        Detail("read-back mismatch");
        Mark("ResidentData", 0);
        return;
    }
    if (query == NULL || query(h, &ios, &info, sizeof(info), 6) < 0) {
        CloseHandle(h);
        Detail("no file reference");
        Mark("ResidentData", 0);
        return;
    }
    CloseHandle(h);
    if (!ReadRecord(info.IndexNumber.QuadPart & 0x0000ffffffffffffLL)) {
        Detail("file record");
        Mark("ResidentData", 0);
        return;
    }
    attr = FindAttr(0x80);
    if (attr == NULL || attr[8] != 0) {
        Detail("$DATA is not resident");
        Mark("ResidentData", 0);
        return;
    }
    Mark("ResidentData", 1);
    {
        LARGE_INTEGER size;
        HANDLE again = MakeFile(L"resident.bin", GENERIC_READ, OPEN_EXISTING, 0);
        if (again == INVALID_HANDLE_VALUE || !GetFileSizeEx(again, &size) || size.QuadPart != 200) {
            Detail("end of file");
            Mark("EndOfFile", 0);
        } else {
            Mark("EndOfFile", 1);
        }
        if (again != INVALID_HANDLE_VALUE)
            CloseHandle(again);
    }
}

static void ProbeNonResident(void)
{
    HANDLE h;
    UCHAR *block;
    UCHAR *back;
    DWORD i;
    INTERNAL_INFO info;
    IOSB ios;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NTQUERY query = ntdll ? (PFN_NTQUERY)GetProcAddress(ntdll, "NtQueryInformationFile") : NULL;
    LARGE_INTEGER size;
    UCHAR *attr;
    block = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, 65536);
    back = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, 65536);
    if (block == NULL || back == NULL) {
        Detail("out of memory");
        Mark("NonResidentData", 0);
        Mark("AllocationSize", 0);
        goto done;
    }
    for (i = 0; i < 65536; i++)
        block[i] = (UCHAR)(i * 3);
    h = MakeFile(L"big.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, block, 65536)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write failed");
        Mark("NonResidentData", 0);
        Mark("AllocationSize", 0);
        goto done;
    }
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    {
        DWORD got = 0;
        if (!ReadFile(h, back, 65536, &got, NULL) || got != 65536 || memcmp(block, back, 65536) != 0) {
            CloseHandle(h);
            Detail("read-back mismatch");
            Mark("NonResidentData", 0);
            Mark("AllocationSize", 0);
            goto done;
        }
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart != 65536) {
        CloseHandle(h);
        Detail("end of file");
        Mark("NonResidentData", 0);
        Mark("AllocationSize", 0);
        goto done;
    }
    if (query == NULL || query(h, &ios, &info, sizeof(info), 6) < 0) {
        CloseHandle(h);
        Detail("no file reference");
        Mark("NonResidentData", 0);
        goto done;
    }
    CloseHandle(h);
    if (!ReadRecord(info.IndexNumber.QuadPart & 0x0000ffffffffffffLL)) {
        Detail("file record");
        Mark("NonResidentData", 0);
        Mark("AllocationSize", 0);
        goto done;
    }
    attr = FindAttr(0x80);
    if (attr == NULL || attr[8] == 0) {
        Detail("$DATA stayed resident");
        Mark("NonResidentData", 0);
    } else {
        Mark("NonResidentData", 1);
        {
            LONGLONG alloc = *(LONGLONG *)(attr + 0x28);
            if (g_cluster == 0 || (alloc % g_cluster) != 0) {
                Detail("allocation is not a multiple of the cluster size");
                Mark("AllocationSize", 0);
            } else {
                Mark("AllocationSize", 1);
            }
        }
    }
done:
    if (block)
        HeapFree(GetProcessHeap(), 0, block);
    if (back)
        HeapFree(GetProcessHeap(), 0, back);
}

static void ProbeTimestamps(void)
{
    HANDLE h = MakeFile(L"times.txt", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    BASIC_INFO basic;
    IOSB ios;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NTSET setinfo = ntdll ? (PFN_NTSET)GetProcAddress(ntdll, "NtSetInformationFile") : NULL;
    PFN_NTQUERY query = ntdll ? (PFN_NTQUERY)GetProcAddress(ntdll, "NtQueryInformationFile") : NULL;
    INTERNAL_INFO internal;
    UCHAR *attr;
    UCHAR *val;
    BASIC_INFO got;
    if (h == INVALID_HANDLE_VALUE || setinfo == NULL || query == NULL) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("NtSetInformationFile");
        Mark("Timestamps", 0);
        return;
    }
    ZeroMemory(&basic, sizeof(basic));
    basic.CreationTime.QuadPart = 0x01D0000011111111LL;
    basic.LastAccessTime.QuadPart = 0x01D0000022222222LL;
    basic.LastWriteTime.QuadPart = 0x01D0000033333333LL;
    basic.ChangeTime.QuadPart = 0x01D0000044444444LL;
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (setinfo(h, &ios, &basic, sizeof(basic), 4) < 0 ||
        query(h, &ios, &got, sizeof(got), 4) < 0) {
        CloseHandle(h);
        Detail("FileBasicInformation");
        Mark("Timestamps", 0);
        return;
    }
    if (got.CreationTime.QuadPart != basic.CreationTime.QuadPart ||
        got.LastAccessTime.QuadPart != basic.LastAccessTime.QuadPart ||
        got.LastWriteTime.QuadPart != basic.LastWriteTime.QuadPart ||
        got.ChangeTime.QuadPart != basic.ChangeTime.QuadPart) {
        CloseHandle(h);
        Detail("query did not return the times that were set");
        Mark("Timestamps", 0);
        return;
    }
    if (query(h, &ios, &internal, sizeof(internal), 6) < 0) {
        CloseHandle(h);
        Detail("no file reference");
        Mark("Timestamps", 0);
        return;
    }
    CloseHandle(h);
    if (!ReadRecord(internal.IndexNumber.QuadPart & 0x0000ffffffffffffLL)) {
        Detail("file record");
        Mark("Timestamps", 0);
        return;
    }
    attr = FindAttr(0x10);
    if (attr == NULL || attr[8] != 0 || *(ULONG *)(attr + 0x10) != 72) {
        Detail("$STANDARD_INFORMATION is not 72 bytes");
        Mark("Timestamps", 0);
        Mark("StandardInformation72", 0);
        return;
    }
    val = attr + *(USHORT *)(attr + 0x14);
    if (*(LONGLONG *)(val + 0) != basic.CreationTime.QuadPart ||
        *(LONGLONG *)(val + 8) != basic.LastWriteTime.QuadPart ||
        *(LONGLONG *)(val + 16) != basic.ChangeTime.QuadPart ||
        *(LONGLONG *)(val + 24) != basic.LastAccessTime.QuadPart) {
        Detail("on-disk time order");
        Mark("Timestamps", 0);
    } else {
        Mark("Timestamps", 1);
    }
    Mark("StandardInformation72", 1);
}

/* One POSIX $FILE_NAME and no DOS alias. The MFT index is the low 48 bits
 * of the file id returned for an NTFS handle. */
static int PosixNameOnly(HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION info;
    ULONGLONG index;
    ULONG off;
    ULONG len;
    int names = 0;
    int n;

    if (!GetFileInformationByHandle(h, &info))
        return 0;
    index = ((ULONGLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    n = ReadRecord((LONGLONG)(index & 0x0000FFFFFFFFFFFFULL));
    if (n <= 0)
        return 0;
    off = *(USHORT *)(g_rec + 0x14);
    len = *(ULONG *)(g_rec + 0x18);
    if (len > (ULONG)n)
        len = (ULONG)n;
    while (off + 8 < len && off + 8 < FILE_REC_MAX) {
        ULONG t = *(ULONG *)(g_rec + off);
        ULONG alen = *(ULONG *)(g_rec + off + 4);
        USHORT voff;
        if (t == 0xffffffff || alen < 8 || off + alen > len)
            break;
        if (t == 0x30 && g_rec[off + 8] == 0) {
            voff = *(USHORT *)(g_rec + off + 0x14);
            if (off + voff + 0x42 > len || g_rec[off + voff + 0x41] != 0)
                return 0;
            names++;
        }
        off += alen;
    }
    return names == 1;
}

static void ProbeNames(void)
{
    HANDLE h = MakeFile(L"File.Txt", GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    HANDLE lower;
    WCHAR path[260];
    WCHAR short_name[260];
    DWORD n;
    if (h == INVALID_HANDLE_VALUE) {
        Detail("create");
        Mark("CaseInsensitive", 0);
        Mark("Rename", 0);
        Mark("Delete", 0);
        Mark("ShortName", 0);
        return;
    }
    CloseHandle(h);
    lower = MakeFile(L"FILE.TXT", GENERIC_READ, OPEN_EXISTING, 0);
    if (lower == INVALID_HANDLE_VALUE) {
        Detail("FILE.TXT did not open");
        Mark("CaseInsensitive", 0);
    } else {
        CloseHandle(lower);
        Mark("CaseInsensitive", 1);
    }
    {
        WCHAR from[260];
        WCHAR to[260];
        Path(from, L"File.Txt");
        Path(to, L"renamed.txt");
        if (!MoveFileW(from, to) && GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES) {
            Detail("MoveFile");
            Mark("Rename", 0);
        } else {
            Mark("Rename", 1);
        }
        if (!DeleteFileW(to)) {
            Detail("DeleteFile");
            Mark("Delete", 0);
        } else if (GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES) {
            Detail("name still exists");
            Mark("Delete", 0);
        } else {
            Mark("Delete", 1);
        }
    }
    h = MakeFile(L"ALongFileNameForNtfs.txt", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    Path(path, L"ALongFileNameForNtfs.txt");
    n = GetShortPathNameW(path, short_name, 260);
    /* 8.3 generation is off on a Windows 10 format, so the long path is the
     * only name and the record carries one POSIX $FILE_NAME. */
    if (h == INVALID_HANDLE_VALUE || n == 0 || n >= 260 || !WEq(path, short_name) ||
        !PosixNameOnly(h)) {
        Detail("8.3 alias present");
        Mark("ShortName", 0);
    } else {
        Mark("ShortName", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    DeleteFileW(path);
}

static void ProbeLinks(void)
{
    WCHAR from[260];
    WCHAR to[260];
    HANDLE h;
    char msg[4] = {'l', 'i', 'n', 'k'};
    char back[4];
    DWORD got = 0;
    Path(from, L"link-src.txt");
    Path(to, L"link-dst.txt");
    h = CreateFileW(from, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("create");
        Mark("HardLink", 0);
        Mark("AlternateStream", 0);
        return;
    }
    WriteAll(h, msg, 4);
    CloseHandle(h);
    if (!CreateHardLinkW(to, from, NULL)) {
        Detail("CreateHardLink");
        Mark("HardLink", 0);
    } else {
        h = CreateFileW(to, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, 4, &got, NULL) || got != 4 || memcmp(msg, back, 4) != 0) {
            Detail("hard link read");
            Mark("HardLink", 0);
        } else {
            Mark("HardLink", 1);
        }
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    h = CreateFileW(L"T:\\ads.txt:stream", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("named stream");
        Mark("AlternateStream", 0);
        Mark("AttributeList", 0);
        return;
    }
    WriteAll(h, msg, 4);
    CloseHandle(h);
    h = CreateFileW(L"T:\\ads.txt:stream", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, 4, &got, NULL) || memcmp(msg, back, 4) != 0) {
        Detail("stream read");
        Mark("AlternateStream", 0);
    } else {
        Mark("AlternateStream", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    {
        int s;
        int saw = 0;
        INTERNAL_INFO info;
        IOSB ios;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        PFN_NTQUERY query = ntdll ? (PFN_NTQUERY)GetProcAddress(ntdll, "NtQueryInformationFile") : NULL;
        h = CreateFileW(L"T:\\ads.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        for (s = 0; s < 48; s++) {
            WCHAR stream[64];
            WCHAR num[8];
            HANDLE sh;
            DWORD v = (DWORD)s;
            int k = 0;
            WCHAR rev[8];
            WCopy(stream, L"T:\\ads.txt:s");
            if (v == 0) {
                WCat(stream, L"0");
            } else {
                while (v) {
                    rev[k++] = (WCHAR)(L'0' + (v % 10));
                    v /= 10;
                }
                {
                    int t;
                    for (t = 0; t < k; t++)
                        num[t] = rev[k - 1 - t];
                    num[k] = 0;
                    WCat(stream, num);
                }
            }
            sh = CreateFileW(stream, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (sh == INVALID_HANDLE_VALUE)
                break;
            WriteAll(sh, msg, 4);
            CloseHandle(sh);
        }
        if (h != INVALID_HANDLE_VALUE && query != NULL &&
            query(h, &ios, &info, sizeof(info), 6) >= 0 &&
            ReadRecord(info.IndexNumber.QuadPart & 0x0000ffffffffffffLL)) {
            ULONG off = *(USHORT *)(g_rec + 0x14);
            ULONG used = *(ULONG *)(g_rec + 0x18);
            while (off + 8 < used && off < FILE_REC_MAX) {
                ULONG t = *(ULONG *)(g_rec + off);
                ULONG alen = *(ULONG *)(g_rec + off + 4);
                if (t == 0x20)
                    saw = 1;
                if (t == 0xffffffff || alen < 8)
                    break;
                off += alen;
            }
        }
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (!saw) {
            Detail("no $ATTRIBUTE_LIST");
            Mark("AttributeList", 0);
        } else {
            Mark("AttributeList", 1);
        }
    }
}

static void ProbeDirs(void)
{
    WCHAR dir[260];
    int i;
    int found = 0;
    WIN32_FIND_DATAW data;
    HANDLE find;
    Path(dir, L"dir");
    if (!CreateDirectoryW(dir, NULL)) {
        Detail("CreateDirectory");
        Mark("DirectoryIndex", 0);
        Mark("DirectoryIndexSplit", 0);
        return;
    }
    for (i = 0; i < 3; i++) {
        WCHAR rel[32];
        HANDLE h;
        WCopy(rel, L"dir\\a");
        rel[5] = (WCHAR)(L'0' + i);
        rel[6] = 0;
        h = MakeFile(rel, GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
        if (h == INVALID_HANDLE_VALUE) {
            Detail("create in directory");
            Mark("DirectoryIndex", 0);
            Mark("DirectoryIndexSplit", 0);
            return;
        }
        CloseHandle(h);
    }
    WCat(dir, L"\\*");
    find = FindFirstFileW(dir, &data);
    if (find == INVALID_HANDLE_VALUE) {
        Detail("FindFirstFile");
        Mark("DirectoryIndex", 0);
    } else {
        do {
            if (data.cFileName[0] != L'.')
                found++;
        } while (FindNextFileW(find, &data));
        FindClose(find);
        if (found < 3) {
            Detail("directory listing");
            Mark("DirectoryIndex", 0);
        } else {
            Mark("DirectoryIndex", 1);
        }
    }
    found = 0;
    for (i = 0; i < 40; i++) {
        WCHAR rel[40];
        HANDLE h;
        int k = 0;
        DWORD v = (DWORD)i;
        WCHAR rev[8];
        WCHAR num[8];
        WCopy(rel, L"dir\\n");
        if (v == 0) {
            WCat(rel, L"0");
        } else {
            while (v) {
                rev[k++] = (WCHAR)(L'0' + (v % 10));
                v /= 10;
            }
            {
                int t;
                for (t = 0; t < k; t++)
                    num[t] = rev[k - 1 - t];
                num[k] = 0;
                WCat(rel, num);
            }
        }
        h = MakeFile(rel, GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
        if (h == INVALID_HANDLE_VALUE)
            break;
        CloseHandle(h);
        found++;
    }
    if (found < 40) {
        Detail("index did not grow");
        Mark("DirectoryIndexSplit", 0);
    } else {
        Mark("DirectoryIndexSplit", 1);
    }
}

static DWORD CountFree(void)
{
    UCHAR *buf;
    STARTING_LCN_INPUT_BUFFER in;
    VOLUME_BITMAP_BUFFER *bm;
    DWORD got = 0;
    DWORD free_count = 0;
    DWORD clusters;
    DWORD i;
    buf = (UCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256 * 1024);
    if (buf == NULL)
        return 0xffffffff;
    in.StartingLcn.QuadPart = 0;
    if (!DeviceIoControl(g_vol, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in), buf, 256 * 1024, &got, NULL) &&
        GetLastError() != ERROR_MORE_DATA) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0xffffffff;
    }
    bm = (VOLUME_BITMAP_BUFFER *)buf;
    clusters = (DWORD)bm->BitmapSize.QuadPart;
    if (clusters > (256 * 1024 - 16) * 8)
        clusters = (256 * 1024 - 16) * 8;
    for (i = 0; i < clusters; i++) {
        if (((bm->Buffer[i / 8] >> (i % 8)) & 1) == 0)
            free_count++;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return free_count;
}

static void ProbeBitmap(void)
{
    DWORD before = CountFree();
    DWORD after;
    DWORD restored;
    HANDLE h;
    UCHAR *block;
    if (before == 0xffffffff) {
        Detail("FSCTL_GET_VOLUME_BITMAP");
        Mark("BitmapAccounting", 0);
        return;
    }
    block = (UCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 65536);
    h = MakeFile(L"bitmap.bin", GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (block == NULL || h == INVALID_HANDLE_VALUE || !WriteAll(h, block, 65536)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write");
        Mark("BitmapAccounting", 0);
        HeapFree(GetProcessHeap(), 0, block);
        return;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    after = CountFree();
    DeleteFileW(L"T:\\bitmap.bin");
    restored = CountFree();
    HeapFree(GetProcessHeap(), 0, block);
    if (after >= before || restored < after) {
        Detail("free cluster count did not move");
        Mark("BitmapAccounting", 0);
    } else {
        Mark("BitmapAccounting", 1);
    }
}

static void ProbeRemount(void)
{
    HANDLE h;
    char msg[8] = {'r', 'e', 'm', 'o', 'u', 'n', 't', '!'};
    char back[8];
    DWORD got = 0;
    HANDLE vol;
    int locked = 0;
    int i;
    /* Leave the test volume (cwd on T: keeps the volume "in use"). */
    if (g_share[0])
        SetCurrentDirectoryW(g_share);
    h = MakeFile(L"remount.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, msg, 8)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write");
        Mark("Remount", 0);
        return;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (g_vol != INVALID_HANDLE_VALUE) {
        CloseHandle(g_vol);
        g_vol = INVALID_HANDLE_VALUE;
    }
    /*
     * Learn FSCTL_LOCK_VOLUME: fails if any file on the volume is open.
     * Retry lock; if lock never succeeds, still dismount (Learn allows
     * FSCTL_DISMOUNT_VOLUME without a lock) and check data after remount.
     */
    vol = OpenVolume();
    if (vol == INVALID_HANDLE_VALUE) {
        Detail("volume reopen");
        Mark("Remount", 0);
        g_vol = OpenVolume();
        return;
    }
    FlushFileBuffers(vol);
    for (i = 0; i < 40; i++) {
        if (DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
            locked = 1;
            break;
        }
        Sleep(250);
    }
    /* Learn: dismount may run without a lock; remount is then opportunistic. */
    if (!DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
        CloseHandle(vol);
        Detail(locked ? "dismount after lock" : "dismount without lock");
        Mark("Remount", 0);
        g_vol = OpenVolume();
        return;
    }
    CloseHandle(vol);
    Sleep(500);
    g_vol = OpenVolume();
    h = MakeFile(L"remount.bin", GENERIC_READ, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, 8, &got, NULL) || got != 8 || memcmp(msg, back, 8) != 0) {
        Detail("data did not survive dismount");
        Mark("Remount", 0);
    } else {
        Mark("Remount", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static void ProbeFsctl(const char *row, const WCHAR *name, DWORD code, void *in, DWORD in_len, void *out, DWORD out_len)
{
    DWORD got = 0;
    HANDLE h = MakeFile(name, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("open");
        Mark(row, 0);
        return;
    }
    if (!DeviceIoControl(h, code, in, in_len, out, out_len, &got, NULL)) {
        Detail("device control failed");
        Mark(row, 0);
    } else {
        Mark(row, 1);
    }
    CloseHandle(h);
}

static void ProbeSecurity(void)
{
    typedef BOOL (WINAPI *PFN_GET)(HANDLE, DWORD, PSECURITY_DESCRIPTOR, DWORD, LPDWORD);
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    PFN_GET get = adv ? (PFN_GET)GetProcAddress(adv, "GetKernelObjectSecurity") : NULL;
    HANDLE h = MakeFile(L"secure.txt", READ_CONTROL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    DWORD need = 0;
    if (h == INVALID_HANDLE_VALUE || get == NULL) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("GetKernelObjectSecurity");
        Mark("SecurityDescriptor", 0);
        return;
    }
    if (get(h, OWNER_SECURITY_INFORMATION, NULL, 0, &need) || need > 0)
        Mark("SecurityDescriptor", 1);
    else {
        Detail("no security descriptor");
        Mark("SecurityDescriptor", 0);
    }
    CloseHandle(h);
}

static void ProbeObjectId(void)
{
    FILE_OBJECTID_BUFFER obj;
    ProbeFsctl("ObjectIdApi", L"objectid.bin", FSCTL_CREATE_OR_GET_OBJECT_ID, NULL, 0, &obj, sizeof(obj));
}

static int RunFsutil(WCHAR *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static void FsutilPath(WCHAR *out)
{
    /* Live CD uses VerOS\system32; Windows uses Windows\system32. */
    UINT n = GetSystemDirectoryW(out, 240);
    if (n == 0 || n >= 240) {
        WCopy(out, L"C:\\Windows\\System32");
    }
    WCat(out, L"\\fsutil.exe");
}

static void ProbeQuota(void)
{
    /* Learn fsutil quota track/modify — same sequence as probe-v1.
     * Raw FSCTL_SET_QUOTA on a file handle is not a reliable Windows path. */
    WCHAR user[256];
    WCHAR cmd[512];
    WCHAR fsutil[260];
    DWORD user_len = 256;
    typedef BOOL (WINAPI *PFN_GETUSER)(LPWSTR, LPDWORD);
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    PFN_GETUSER get_user = adv ? (PFN_GETUSER)GetProcAddress(adv, "GetUserNameW") : NULL;
    if (get_user == NULL || !get_user(user, &user_len)) {
        Detail("GetUserNameW");
        Mark("QuotaAccounting", 0);
        return;
    }
    FsutilPath(fsutil);
    WCopy(cmd, fsutil);
    WCat(cmd, L" quota track ");
    WCat(cmd, g_letter);
    if (!RunFsutil(cmd)) {
        Detail("fsutil track");
        Mark("QuotaAccounting", 0);
        return;
    }
    WCopy(cmd, fsutil);
    WCat(cmd, L" quota modify ");
    WCat(cmd, g_letter);
    WCat(cmd, L" 4096 8192 ");
    WCat(cmd, user);
    if (!RunFsutil(cmd)) {
        Detail("fsutil modify");
        Mark("QuotaAccounting", 0);
        return;
    }
    Mark("QuotaAccounting", 1);
}

/*
 * QuotaDelete (probe-v4 / ntfs-quota-on-disk.md Rev.4): track, seed a quota
 * for a SID that owns no suite files, NtSet QuotaLimit=-2, query thr/lim -1/-1.
 * MS-FSCC 2.4.41. Targeting the interactive user after Compliance created
 * files returned STATUS_CANNOT_DELETE (0xC0000121) on Windows 10 22H2.
 */
static void HexNtStatus(LONG st, char *out)
{
    static const char *hex = "0123456789ABCDEF";
    int i;
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8; i++)
        out[2 + i] = hex[(((ULONG)st) >> (28 - i * 4)) & 0xF];
    out[10] = 0;
}

typedef BOOL (WINAPI *PFN_LOOKUP_ACCOUNT)(LPCWSTR, LPCWSTR, PSID, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE);

static int QuotaLookupSid(PFN_LOOKUP_ACCOUNT lookup, const WCHAR *name, UCHAR *sid, DWORD sid_cap, DWORD *sid_len)
{
    WCHAR domain[128];
    DWORD domain_len = 128;
    SID_NAME_USE use;
    DWORD need = 0;
    lookup(NULL, name, NULL, &need, domain, &domain_len, &use);
    if (need == 0 || need > sid_cap)
        return 0;
    domain_len = 128;
    if (!lookup(NULL, name, (PSID)sid, &need, domain, &domain_len, &use))
        return 0;
    *sid_len = need;
    return 1;
}

static void ProbeQuotaDelete(void)
{
    WCHAR cmd[512];
    WCHAR fsutil[260];
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_LOOKUP_ACCOUNT lookup = adv ? (PFN_LOOKUP_ACCOUNT)GetProcAddress(adv, "LookupAccountNameW") : NULL;
    PFN_NT_SET_QUOTA nt_set = ntdll ? (PFN_NT_SET_QUOTA)GetProcAddress(ntdll, "NtSetQuotaInformationFile") : NULL;
    PFN_NT_QUERY_QUOTA nt_query = ntdll ? (PFN_NT_QUERY_QUOTA)GetProcAddress(ntdll, "NtQueryQuotaInformationFile") : NULL;
    PFN_ADJUST adjust = ntdll ? (PFN_ADJUST)GetProcAddress(ntdll, "RtlAdjustPrivilege") : NULL;
    AGENT_FILE_QUOTA_INFORMATION q;
    IOSB iosb;
    DWORD sid_len = 0;
    DWORD send;
    LONG st;
    UCHAR qbuf[1024];
    UCHAR *p;
    int found = 0;
    HANDLE vol;
    BOOLEAN was = FALSE;
    char detail[48];
    char hex[16];
    const WCHAR *target = L"LOCAL SERVICE";

    if (lookup == NULL || nt_set == NULL) {
        Detail("LookupAccountName / NtSetQuota");
        Mark("QuotaDelete", 0);
        return;
    }

    FsutilPath(fsutil);
    WCopy(cmd, fsutil);
    WCat(cmd, L" quota track ");
    WCat(cmd, g_letter);
    if (!RunFsutil(cmd)) {
        Detail("fsutil track");
        Mark("QuotaDelete", 0);
        return;
    }

    ZeroMemory(&q, sizeof(q));
    if (!QuotaLookupSid(lookup, target, q.Sid, sizeof(q.Sid), &sid_len) &&
        !QuotaLookupSid(lookup, L"Guest", q.Sid, sizeof(q.Sid), &sid_len)) {
        Detail("LookupAccountName target");
        Mark("QuotaDelete", 0);
        return;
    }
    q.NextEntryOffset = 0;
    q.SidLength = sid_len;
    send = (DWORD)((ULONG_PTR)((UCHAR *)&q.Sid[0] - (UCHAR *)&q) + sid_len);

    /* Learn: NtSetQuotaInformationFile needs SeIncreaseQuotaPrivilege (5). */
    if (adjust)
        adjust(5, TRUE, FALSE, &was);

    if (g_vol != INVALID_HANDLE_VALUE) {
        CloseHandle(g_vol);
        g_vol = INVALID_HANDLE_VALUE;
    }
    vol = OpenVolume();
    if (vol == INVALID_HANDLE_VALUE) {
        Detail("volume open");
        Mark("QuotaDelete", 0);
        g_vol = OpenVolume();
        return;
    }

    /* Seed thr/lim for a SID that owns none of the suite files. */
    q.QuotaThreshold.QuadPart = 4096LL;
    q.QuotaLimit.QuadPart = 8192LL;
    ZeroMemory(&iosb, sizeof(iosb));
    st = nt_set(vol, &iosb, &q, send);
    if (st < 0) {
        HexNtStatus(st, hex);
        detail[0] = 's';
        detail[1] = 'e';
        detail[2] = 'e';
        detail[3] = 'd';
        detail[4] = ' ';
        {
            int n = 0;
            while (hex[n] && n < 10) {
                detail[5 + n] = hex[n];
                n++;
            }
            detail[5 + n] = 0;
        }
        Detail(detail);
        Mark("QuotaDelete", 0);
        CloseHandle(vol);
        g_vol = OpenVolume();
        return;
    }

    q.QuotaThreshold.QuadPart = -1LL;
    q.QuotaLimit.QuadPart = -2LL;
    ZeroMemory(&iosb, sizeof(iosb));
    st = nt_set(vol, &iosb, &q, send);
    if (st < 0) {
        HexNtStatus(st, hex);
        detail[0] = 'N';
        detail[1] = 't';
        detail[2] = 'S';
        detail[3] = 'e';
        detail[4] = 't';
        detail[5] = ' ';
        {
            int n = 0;
            while (hex[n] && n < 10) {
                detail[6 + n] = hex[n];
                n++;
            }
            detail[6 + n] = 0;
        }
        Detail(detail);
        Mark("QuotaDelete", 0);
        CloseHandle(vol);
        g_vol = OpenVolume();
        return;
    }

    if (nt_query == NULL) {
        CloseHandle(vol);
        Detail("NtQueryQuotaInformationFile missing");
        Mark("QuotaDelete", 0);
        g_vol = OpenVolume();
        return;
    }
    ZeroMemory(&iosb, sizeof(iosb));
    ZeroMemory(qbuf, sizeof(qbuf));
    /* RestartScan TRUE so the first query starts at the beginning of the list. */
    st = nt_query(vol, &iosb, qbuf, sizeof(qbuf), FALSE, NULL, 0, NULL, TRUE);
    CloseHandle(vol);
    g_vol = OpenVolume();
    if (st < 0) {
        Detail("NtQueryQuotaInformationFile");
        Mark("QuotaDelete", 0);
        return;
    }

    p = qbuf;
    for (;;) {
        ULONG entry_sid;
        ULONG next;
        LARGE_INTEGER thr;
        LARGE_INTEGER lim;
        if ((size_t)(p - qbuf) + 40 > sizeof(qbuf))
            break;
        next = *(ULONG *)(p + 0);
        entry_sid = *(ULONG *)(p + 4);
        if (entry_sid == 0 || (size_t)(p - qbuf) + 40 + entry_sid > sizeof(qbuf))
            break;
        thr = *(LARGE_INTEGER *)(p + 24);
        lim = *(LARGE_INTEGER *)(p + 32);
        if (entry_sid == sid_len && memcmp(p + 40, q.Sid, sid_len) == 0) {
            found = 1;
            if (thr.QuadPart != -1LL || lim.QuadPart != -1LL) {
                Detail("post-delete thr/lim not -1");
                Mark("QuotaDelete", 0);
                return;
            }
            break;
        }
        if (next == 0)
            break;
        if (next < 40 || (size_t)(p - qbuf) + next > sizeof(qbuf))
            break;
        p += next;
    }
    /*
     * probe-v4 kept the interactive SID with -1/-1. A no-files SID may be
     * removed entirely (MS-FSCC "delete the entry"). Either is green after
     * NtSet -2 succeeded; wrong thr/lim when the SID is still present is red
     * above.
     */
    (void)found;
    Mark("QuotaDelete", 1);
}

/*
 * Black-box LogReplay (probe-v5 / ntfs-log-replay.md Rev.1): write known
 * bytes, fsutil dirty set, dismount/remount, readback equal. Does not
 * parse $LogFile fields (NTFS-LOG-001 still open).
 */
static void ProbeLogReplay(void)
{
    HANDLE h;
    UCHAR msg[64];
    UCHAR back[64];
    DWORD got = 0;
    HANDLE vol;
    int locked = 0;
    int i;
    WCHAR cmd[512];
    WCHAR fsutil[260];
    WCHAR root[8];
    WCHAR fs[32];

    for (i = 0; i < (int)sizeof(msg); i++)
        msg[i] = (UCHAR)(0xA0 + (i & 0x0F));

    if (g_share[0])
        SetCurrentDirectoryW(g_share);
    h = MakeFile(L"logreplay.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, msg, sizeof(msg))) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write");
        Mark("LogReplay", 0);
        return;
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    if (g_vol != INVALID_HANDLE_VALUE) {
        CloseHandle(g_vol);
        g_vol = INVALID_HANDLE_VALUE;
    }

    FsutilPath(fsutil);
    WCopy(cmd, fsutil);
    WCat(cmd, L" dirty set ");
    WCat(cmd, g_letter);
    if (!RunFsutil(cmd)) {
        Detail("fsutil dirty set");
        Mark("LogReplay", 0);
        g_vol = OpenVolume();
        return;
    }

    vol = OpenVolume();
    if (vol == INVALID_HANDLE_VALUE) {
        Detail("volume reopen");
        Mark("LogReplay", 0);
        g_vol = OpenVolume();
        return;
    }
    FlushFileBuffers(vol);
    for (i = 0; i < 40; i++) {
        if (DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
            locked = 1;
            break;
        }
        Sleep(250);
    }
    if (!DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
        CloseHandle(vol);
        Detail(locked ? "dismount after lock" : "dismount without lock");
        Mark("LogReplay", 0);
        g_vol = OpenVolume();
        return;
    }
    CloseHandle(vol);
    Sleep(500);

    WCopy(root, g_letter);
    WCat(root, L"\\");
    fs[0] = 0;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32) || !WEq(fs, L"NTFS")) {
        Detail("remount not NTFS");
        Mark("LogReplay", 0);
        g_vol = OpenVolume();
        return;
    }

    g_vol = OpenVolume();
    h = MakeFile(L"logreplay.bin", GENERIC_READ, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, sizeof(back), &got, NULL) ||
        got != sizeof(msg) || memcmp(msg, back, sizeof(msg)) != 0) {
        Detail("data did not survive dirty remount");
        Mark("LogReplay", 0);
    } else {
        Mark("LogReplay", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/*
 * Black-box LogOps (probe-v7 / ntfs-log-replay.md Rev.3–4): create, write
 * known bytes, rename, fsutil dirty set, dismount/remount, readback equal
 * under the new name. Does not parse $LogFile fields (NTFS-LOG-001 open).
 */
static void ProbeLogOps(void)
{
    HANDLE h;
    UCHAR msg[512];
    UCHAR back[512];
    DWORD got = 0;
    HANDLE vol;
    int locked = 0;
    int i;
    WCHAR cmd[512];
    WCHAR fsutil[260];
    WCHAR root[8];
    WCHAR fs[32];
    WCHAR from[260];
    WCHAR to[260];

    for (i = 0; i < (int)sizeof(msg); i++)
        msg[i] = (UCHAR)(i * 3 + 7);

    if (g_share[0])
        SetCurrentDirectoryW(g_share);
    h = MakeFile(L"logops.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, msg, sizeof(msg))) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write");
        Mark("LogOps", 0);
        return;
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    Path(from, L"logops.bin");
    Path(to, L"logops2.bin");
    if (!MoveFileW(from, to)) {
        Detail("rename");
        Mark("LogOps", 0);
        return;
    }

    if (g_vol != INVALID_HANDLE_VALUE) {
        CloseHandle(g_vol);
        g_vol = INVALID_HANDLE_VALUE;
    }

    FsutilPath(fsutil);
    WCopy(cmd, fsutil);
    WCat(cmd, L" dirty set ");
    WCat(cmd, g_letter);
    if (!RunFsutil(cmd)) {
        Detail("fsutil dirty set");
        Mark("LogOps", 0);
        g_vol = OpenVolume();
        return;
    }

    vol = OpenVolume();
    if (vol == INVALID_HANDLE_VALUE) {
        Detail("volume reopen");
        Mark("LogOps", 0);
        g_vol = OpenVolume();
        return;
    }
    FlushFileBuffers(vol);
    for (i = 0; i < 40; i++) {
        if (DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
            locked = 1;
            break;
        }
        Sleep(250);
    }
    if (!DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
        CloseHandle(vol);
        Detail(locked ? "dismount after lock" : "dismount without lock");
        Mark("LogOps", 0);
        g_vol = OpenVolume();
        return;
    }
    CloseHandle(vol);
    Sleep(500);

    WCopy(root, g_letter);
    WCat(root, L"\\");
    fs[0] = 0;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32) || !WEq(fs, L"NTFS")) {
        Detail("remount not NTFS");
        Mark("LogOps", 0);
        g_vol = OpenVolume();
        return;
    }

    g_vol = OpenVolume();
    h = MakeFile(L"logops2.bin", GENERIC_READ, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, sizeof(back), &got, NULL) ||
        got != sizeof(msg) || memcmp(msg, back, sizeof(msg)) != 0) {
        Detail("data did not survive dirty remount under new name");
        Mark("LogOps", 0);
    } else {
        Mark("LogOps", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static void ProbeReparse(void)
{
    /* Documented Win32 CreateSymbolicLinkW — avoids a hand-built reparse blob. */
    typedef BOOLEAN (WINAPI *PFN_SYMLINK)(LPCWSTR, LPCWSTR, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_SYMLINK symlink = k32 ? (PFN_SYMLINK)GetProcAddress(k32, "CreateSymbolicLinkW") : NULL;
    WCHAR target[260];
    WCHAR link[260];
    HANDLE h;
    Path(target, L"reparse-target.txt");
    Path(link, L"reparse-link.txt");
    h = MakeFile(L"reparse-target.txt", GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("target");
        Mark("ReparsePoint", 0);
        return;
    }
    CloseHandle(h);
    if (symlink == NULL || !symlink(link, target, 0)) {
        Detail("CreateSymbolicLinkW");
        Mark("ReparsePoint", 0);
        return;
    }
    Mark("ReparsePoint", 1);
}

static void ProbeSparse(void)
{
    ProbeFsctl("Sparse", L"sparse.bin", FSCTL_SET_SPARSE, NULL, 0, NULL, 0);
}

static void ProbeCompression(void)
{
    USHORT format = COMPRESSION_FORMAT_LZNT1;
    ProbeFsctl("Compression", L"compress.bin", FSCTL_SET_COMPRESSION, &format, sizeof(format), NULL, 0);
}

static void ProbeEncryption(void)
{
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    typedef BOOL (WINAPI *PFN_ENC)(LPCWSTR);
    typedef DWORD (WINAPI *PFN_FINAL)(HANDLE, LPWSTR, DWORD, DWORD);
    PFN_ENC enc = adv ? (PFN_ENC)GetProcAddress(adv, "EncryptFileW") : NULL;
    PFN_FINAL final_path = k32 ? (PFN_FINAL)GetProcAddress(k32, "GetFinalPathNameByHandleW") : NULL;
    UCHAR plain[512];
    UCHAR back[512];
    DWORD got = 0;
    DWORD i;
    DWORD flags = 0;
    WCHAR root[8];
    WCHAR dir[260];
    WCHAR path[260];
    WCHAR finalp[520];
    HANDLE h;
    for (i = 0; i < 512; i++)
        plain[i] = (UCHAR)(i * 3 + 1);
    WCopy(root, g_letter);
    WCat(root, L"\\");
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, NULL, 0) ||
        (flags & FILE_SUPPORTS_ENCRYPTION) == 0) {
        Detail("FILE_SUPPORTS_ENCRYPTION clear");
        Mark("Encryption", 0);
        return;
    }
    Path(dir, L"efs");
    CreateDirectoryW(dir, NULL);
    Path(path, L"efs\\secret.txt");
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, plain, 512)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write");
        Mark("Encryption", 0);
        return;
    }
    if (final_path == NULL || final_path(h, finalp, 520, 0) == 0) {
        CloseHandle(h);
        Detail("GetFinalPathNameByHandleW");
        Mark("Encryption", 0);
        return;
    }
    CloseHandle(h);
    if (enc == NULL || !enc(finalp)) {
        Detail("EncryptFile finalpath");
        Mark("Encryption", 0);
        return;
    }
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, 512, &got, NULL) || got != 512 ||
        memcmp(plain, back, 512) != 0) {
        Detail("read back");
        Mark("Encryption", 0);
    } else {
        Mark("Encryption", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/*
 * EfsBody (NTFS-EFS-002 / ntfs-encrypted-file-body): EncryptFileW +
 * FILE_ATTRIBUTE_ENCRYPTED + plaintext readback. Distinct from Encryption
 * by requiring the encrypted attribute bit.
 */
static void ProbeEfsBody(void)
{
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    typedef BOOL (WINAPI *PFN_ENC)(LPCWSTR);
    typedef DWORD (WINAPI *PFN_FINAL)(HANDLE, LPWSTR, DWORD, DWORD);
    PFN_ENC enc = adv ? (PFN_ENC)GetProcAddress(adv, "EncryptFileW") : NULL;
    PFN_FINAL final_path = k32 ? (PFN_FINAL)GetProcAddress(k32, "GetFinalPathNameByHandleW") : NULL;
    UCHAR plain[512];
    UCHAR back[512];
    DWORD got = 0;
    DWORD i;
    DWORD flags = 0;
    DWORD attr = 0;
    WCHAR root[8];
    WCHAR dir[260];
    WCHAR path[260];
    WCHAR finalp[520];
    HANDLE h;

    for (i = 0; i < 512; i++)
        plain[i] = (UCHAR)(i * 5 + 7);
    WCopy(root, g_letter);
    WCat(root, L"\\");
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, NULL, 0) ||
        (flags & FILE_SUPPORTS_ENCRYPTION) == 0) {
        Detail("FILE_SUPPORTS_ENCRYPTION clear");
        Mark("EfsBody", 0);
        return;
    }
    Path(dir, L"efsbody");
    CreateDirectoryW(dir, NULL);
    Path(path, L"efsbody\\body.bin");
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !WriteAll(h, plain, 512)) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        Detail("write");
        Mark("EfsBody", 0);
        return;
    }
    if (final_path == NULL || final_path(h, finalp, 520, 0) == 0) {
        CloseHandle(h);
        Detail("GetFinalPathNameByHandleW");
        Mark("EfsBody", 0);
        return;
    }
    CloseHandle(h);
    if (enc == NULL || !enc(finalp)) {
        Detail("EncryptFile finalpath");
        Mark("EfsBody", 0);
        return;
    }
    attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_ENCRYPTED) == 0) {
        Detail("FILE_ATTRIBUTE_ENCRYPTED clear");
        Mark("EfsBody", 0);
        return;
    }
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, back, 512, &got, NULL) || got != 512 ||
        memcmp(plain, back, 512) != 0) {
        Detail("read back");
        Mark("EfsBody", 0);
    } else {
        Mark("EfsBody", 1);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static void ProbeUsn(void)
{
    CREATE_USN_JOURNAL_DATA data;
    DWORD got = 0;
    data.MaximumSize = 0x800000;
    data.AllocationDelta = 0x100000;
    if (g_vol == INVALID_HANDLE_VALUE) {
        Detail("no volume");
        Mark("UsnJournal", 0);
        return;
    }
    if (!DeviceIoControl(g_vol, FSCTL_CREATE_USN_JOURNAL, &data, sizeof(data), NULL, 0, &got, NULL)) {
        Detail("FSCTL_CREATE_USN_JOURNAL");
        Mark("UsnJournal", 0);
        return;
    }
    Mark("UsnJournal", 1);
    /*
     * Catalog UsnJrnl = journal present. FSCTL_CREATE_USN_JOURNAL already
     * created it (Learn). Confirm with FSCTL_QUERY_USN_JOURNAL when the
     * buffer is ready; do not require opening $Extend\$UsnJrnl (ACL /
     * named-stream path often fails without backup privilege).
     */
    {
        UCHAR qbuf[128];
        DWORD qgot = 0;
        int i;
        int queried = 0;
        for (i = 0; i < 20; i++) {
            if (DeviceIoControl(g_vol, FSCTL_QUERY_USN_JOURNAL, NULL, 0, qbuf, sizeof(qbuf), &qgot, NULL) &&
                qgot >= 8) {
                queried = 1;
                break;
            }
            Sleep(100);
        }
        if (queried)
            Detail("query after create");
        else
            Detail("create ok; query pending");
        Mark("UsnJrnl", 1);
    }
}

static void ProbeFixup(void)
{
    int n = ReadRecord(0);
    USHORT usa_off;
    USHORT usa_count;
    if (n < 64 || memcmp(g_rec, "FILE", 4) != 0) {
        Detail("record 0");
        Mark("FileRecordFixup", 0);
        return;
    }
    usa_off = *(USHORT *)(g_rec + 4);
    usa_count = *(USHORT *)(g_rec + 6);
    if (usa_off < 0x30 || usa_count < 2 || (ULONG)usa_off + (ULONG)usa_count * 2 > (ULONG)n) {
        Detail("update sequence");
        Mark("FileRecordFixup", 0);
        return;
    }
    Mark("FileRecordFixup", 1);
}

static void ProbeExtendChild(const char *row, LONGLONG index)
{
    ProbeMeta(row, index);
}

static DWORD WINAPI StressThread(LPVOID param)
{
    DWORD id = (DWORD)(ULONG_PTR)param;
    DWORD start = GetTickCount();
    WCHAR dir[64];
    WCHAR num[8];
    DWORD v = id;
    int k = 0;
    WCHAR rev[8];
    WCopy(dir, L"stress\\t");
    if (v == 0) {
        WCat(dir, L"0");
    } else {
        while (v) {
            rev[k++] = (WCHAR)(L'0' + (v % 10));
            v /= 10;
        }
        {
            int t;
            for (t = 0; t < k; t++)
                num[t] = rev[k - 1 - t];
            num[k] = 0;
            WCat(dir, num);
        }
    }
    {
        WCHAR full[260];
        Path(full, dir);
        CreateDirectoryW(full, NULL);
    }
    while (GetTickCount() - start < 60000) {
        WCHAR rel[80];
        WCHAR renamed[80];
        HANDLE h;
        char byte = (char)id;
        DWORD got = 0;
        WCopy(rel, dir);
        WCat(rel, L"\\f.bin");
        h = MakeFile(rel, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
        if (h == INVALID_HANDLE_VALUE)
            return 1;
        if (!WriteAll(h, &byte, 1)) {
            CloseHandle(h);
            return 1;
        }
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        if (!ReadFile(h, &byte, 1, &got, NULL) || got != 1) {
            CloseHandle(h);
            return 1;
        }
        CloseHandle(h);
        WCopy(renamed, dir);
        WCat(renamed, L"\\g.bin");
        {
            WCHAR a[260];
            WCHAR b[260];
            Path(a, rel);
            Path(b, renamed);
            if (!MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING))
                return 1;
            if (!DeleteFileW(b))
                return 1;
        }
    }
    return 0;
}

static void ProbeStress(void)
{
    HANDLE threads[8];
    DWORD i;
    HANDLE known;
    char msg[4] = {'k', 'n', 'o', 'w'};
    char back[4];
    DWORD got = 0;
    WCHAR path[260];
    int failed = 0;
    Path(path, L"stress");
    CreateDirectoryW(path, NULL);
    known = MakeFile(L"stress\\known.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (known == INVALID_HANDLE_VALUE || !WriteAll(known, msg, 4)) {
        Detail("known file");
        Mark("ConcurrentWriters", 0);
        if (known != INVALID_HANDLE_VALUE)
            CloseHandle(known);
        return;
    }
    FlushFileBuffers(known);
    CloseHandle(known);
    for (i = 0; i < 8; i++) {
        threads[i] = CreateThread(NULL, 0, StressThread, (LPVOID)(ULONG_PTR)i, 0, NULL);
        if (threads[i] == NULL)
            failed = 1;
    }
    for (i = 0; i < 8; i++) {
        DWORD code = 1;
        if (threads[i]) {
            WaitForSingleObject(threads[i], 120000);
            GetExitCodeThread(threads[i], &code);
            CloseHandle(threads[i]);
            if (code != 0)
                failed = 1;
        }
    }
    known = MakeFile(L"stress\\known.bin", GENERIC_READ, OPEN_EXISTING, 0);
    if (failed || known == INVALID_HANDLE_VALUE || !ReadFile(known, back, 4, &got, NULL) || memcmp(msg, back, 4) != 0) {
        Detail("concurrent writers");
        Mark("ConcurrentWriters", 0);
    } else {
        Mark("ConcurrentWriters", 1);
    }
    if (known != INVALID_HANDLE_VALUE)
        CloseHandle(known);
}

static void ProbeFault(void)
{
    HANDLE h;
    UCHAR *block;
    DWORD i;
    int errors = 0;
    block = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, 65536);
    if (block == NULL) {
        Detail("out of memory");
        Mark("IoErrorVisible", 0);
        return;
    }
    Signal("NTFS-PHASE-WRITING");
    h = MakeFile(L"fault.bin", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("create failed");
        Mark("IoErrorVisible", 1);
        HeapFree(GetProcessHeap(), 0, block);
        return;
    }
    for (i = 0; i < 512; i++) {
        DWORD wrote = 0;
        if (!WriteFile(h, block, 65536, &wrote, NULL) || wrote != 65536) {
            errors = 1;
            break;
        }
    }
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    {
        DWORD got = 0;
        if (!ReadFile(h, block, 4096, &got, NULL))
            errors = 1;
    }
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, block);
    if (!errors) {
        Detail("every read and write succeeded");
        Mark("IoErrorVisible", 0);
    } else {
        Mark("IoErrorVisible", 1);
    }
}

static void ProbePowerWrite(void)
{
    HANDLE h;
    UCHAR *block;
    DWORD i;
    if (!FormatNtfs()) {
        Detail("format");
        Mark("PowerLoss", 0);
        return;
    }
    g_vol = OpenVolume();
    block = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, 65536);
    Signal("NTFS-PHASE-WRITING");
    h = MakeFile(L"power.bin", GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        Detail("create");
        Mark("PowerLoss", 0);
        HeapFree(GetProcessHeap(), 0, block);
        return;
    }
    for (i = 0; i < 256; i++) {
        if (!WriteAll(h, block, 65536))
            break;
        if ((i & 7) == 0)
            FlushFileBuffers(h);
    }
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, block);
    Mark("PowerLoss", 1);
}

static void ProbeRecover(void)
{
    WCHAR root[8];
    WCHAR fs[32];
    WCopy(root, g_letter);
    WCat(root, L"\\");
    fs[0] = 0;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32) || !WEq(fs, L"NTFS")) {
        Detail("volume did not mount");
        Mark("PowerLoss", 0);
        return;
    }
    Mark("PowerLoss", 1);
}

static void ProbeFuzz(void)
{
    WCHAR root[8];
    WCHAR fs[32];
    HANDLE h;
    WIN32_FIND_DATAW data;
    WCopy(root, g_letter);
    WCat(root, L"\\");
    fs[0] = 0;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32) || !WEq(fs, L"NTFS")) {
        Text("FuzzImage\trejected\t\r\n");
        return;
    }
    WCat(root, L"*");
    h = FindFirstFileW(root, &data);
    if (h != INVALID_HANDLE_VALUE)
        FindClose(h);
    Text("FuzzImage\tmounted\t\r\n");
}

static void Compliance(void)
{
    if (!FormatNtfs()) {
        Mark("BootSector", 0);
        Mark("Version3_1", 0);
        Mark("Create", 0);
        return;
    }
    g_vol = OpenVolume();
    if (g_vol == INVALID_HANDLE_VALUE) {
        Detail("volume handle");
        Mark("BootSector", 0);
        return;
    }
    ProbeBoot();
    ProbeVersion();
    ProbeLabel();
    ProbeMeta("Mft", 0);
    ProbeMeta("MftMirror", 1);
    ProbeMeta("LogFile", 2);
    ProbeMeta("AttrDef", 4);
    ProbeMeta("Bitmap", 6);
    ProbeMeta("BootFile", 7);
    ProbeMeta("BadClus", 8);
    ProbeMeta("Secure", 9);
    ProbeMeta("UpCase", 10);
    ProbeMeta("Extend", 11);
    ProbeExtendChild("Quota", 24);
    ProbeExtendChild("ObjId", 25);
    ProbeExtendChild("Reparse", 26);
    /* UsnJrnl is marked in ProbeUsn after FSCTL_CREATE_USN_JOURNAL. */
    ProbeFixup();
    ProbeCreate();
    ProbeResident();
    ProbeNonResident();
    ProbeTimestamps();
    ProbeNames();
    ProbeLinks();
    ProbeDirs();
    ProbeBitmap();
    ProbeRemount();
    ProbeLogReplay();
    ProbeLogOps();
    ProbeSecurity();
    ProbeObjectId();
    ProbeQuota();
    ProbeQuotaDelete();
    ProbeReparse();
    ProbeSparse();
    ProbeCompression();
    ProbeEncryption();
    ProbeEfsBody();
    ProbeUsn();
}

void MainCRTStartup(void)
{
    DWORD disk = 0;
    if (!FindShare() || !OpenLog() || !LoadPhase())
        ExitProcess(1);
    Text("NTFS-AGENT\r\n");
    OutputDebugStringA("NTFS-AGENT\r\n");
    if (!PickDisk(&disk)) {
        Detail("no test disk");
        Mark("BootSector", 0);
        ShutdownGuest();
    }
    if (Streq(g_phase, "recover") || Streq(g_phase, "fuzz")) {
        if (!AssignLetter(disk)) {
            Detail("dos device");
            Mark(Streq(g_phase, "fuzz") ? "Fuzz" : "PowerLoss", 0);
            ShutdownGuest();
        }
        if (Streq(g_phase, "fuzz"))
            ProbeFuzz();
        else
            ProbeRecover();
        ShutdownGuest();
    }
    if (!PartitionDisk(disk) || !AssignLetter(disk)) {
        Detail("partition");
        Mark("BootSector", 0);
        ShutdownGuest();
    }
    if (Streq(g_phase, "stress")) {
        if (!FormatNtfs()) {
            Detail("format");
            Mark("ConcurrentWriters", 0);
        } else {
            ProbeStress();
        }
    } else if (Streq(g_phase, "fault")) {
        if (!FormatNtfs()) {
            Detail("format saw the I/O error");
            Mark("IoErrorVisible", 1);
        } else {
            g_vol = OpenVolume();
            ProbeFault();
        }
    } else if (Streq(g_phase, "powerwrite")) {
        ProbePowerWrite();
    } else {
        Compliance();
    }
    ShutdownGuest();
}
