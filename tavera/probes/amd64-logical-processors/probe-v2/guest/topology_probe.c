/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/amd64-logical-processors/probe-v2/guest/topology_probe.c
 * PURPOSE:     Record Ex topology plus CPUID cache leaves 0x4 and 0x8000001D
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * API-behavior probe for tavera/specs/amd64-logical-processors/ (ALP-009).
 * Sources for names and printed fields: Learn GetLogicalProcessorInformationEx
 * family, and in-tree sdk/include/xdk/ketypes.h plus Intel/Amd Cpuid.h leaf
 * indices (CPUID_CACHE_PARAMS 0x04, CPUID_CACHE_PARAMS_AMD 0x8000001D).
 * Leaf numbers and decoded widths are the probe's read, not Windows fields.
 * Resolve exports through GetProcAddress. Do not open WDK headers for layouts.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef RelationAll
#define RelationProcessorCore 0
#define RelationNumaNode 1
#define RelationCache 2
#define RelationProcessorPackage 3
#define RelationGroup 4
#define RelationProcessorDie 5
#define RelationNumaNodeEx 6
#define RelationProcessorModule 7
#define RelationAll 0xffff
#endif

/* Leaf indices from sdk/include/reactos/x86x64/Intel/Cpuid.h, Amd/Cpuid.h, Cpuid.h. */
#define PROBE_CPUID_EXT_TOPOLOGY 0x0Bu
#define PROBE_CPUID_V2_EXT_TOPOLOGY 0x1Fu
#define PROBE_CPUID_CACHE_PARAMS 0x04u
#define PROBE_CPUID_CACHE_PARAMS_AMD 0x8000001Du
#define PROBE_CPUID_AMD_ADDR_SIZE 0x80000008u
#define PROBE_CPUID_AMD_TOPOLOGY 0x8000001Eu
#define PROBE_LEVEL_SMT 0x01u
#define PROBE_LEVEL_CORE 0x02u
#define PROBE_LEVEL_MODULE 0x03u
#define PROBE_LEVEL_DIE 0x05u
#define PROBE_CACHE_TYPE_NULL 0x00u
#define PROBE_CACHE_TYPE_DATA 0x01u
#define PROBE_CACHE_TYPE_INSTR 0x02u
#define PROBE_CACHE_TYPE_UNIFIED 0x03u

typedef BOOL (WINAPI *PFN_GetLogicalProcessorInformationEx)(
    DWORD Relationship,
    void *Buffer,
    PDWORD ReturnedLength);

typedef DWORD (WINAPI *PFN_GetCurrentProcessorNumber)(void);

typedef struct _PROBE_GROUP_AFFINITY {
    ULONG_PTR Mask;
    WORD Group;
    WORD Reserved[3];
} PROBE_GROUP_AFFINITY;

typedef struct _PROBE_PROCESSOR_RELATIONSHIP {
    BYTE Flags;
    BYTE EfficiencyClass;
    BYTE Reserved[20];
    WORD GroupCount;
    PROBE_GROUP_AFFINITY GroupMask[1];
} PROBE_PROCESSOR_RELATIONSHIP;

typedef struct _PROBE_NUMA_NODE_RELATIONSHIP {
    ULONG NodeNumber;
    BYTE Reserved[20];
    PROBE_GROUP_AFFINITY GroupMask;
} PROBE_NUMA_NODE_RELATIONSHIP;

typedef struct _PROBE_CACHE_RELATIONSHIP {
    BYTE Level;
    BYTE Associativity;
    WORD LineSize;
    ULONG CacheSize;
    ULONG Type;
    BYTE Reserved[20];
    PROBE_GROUP_AFFINITY GroupMask;
} PROBE_CACHE_RELATIONSHIP;

typedef struct _PROBE_PROCESSOR_GROUP_INFO {
    BYTE MaximumProcessorCount;
    BYTE ActiveProcessorCount;
    BYTE Reserved[38];
    ULONG_PTR ActiveProcessorMask;
} PROBE_PROCESSOR_GROUP_INFO;

typedef struct _PROBE_GROUP_RELATIONSHIP {
    WORD MaximumGroupCount;
    WORD ActiveGroupCount;
    BYTE Reserved[20];
    PROBE_PROCESSOR_GROUP_INFO GroupInfo[1];
} PROBE_GROUP_RELATIONSHIP;

typedef struct _PROBE_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    DWORD Relationship;
    DWORD Size;
    union {
        PROBE_PROCESSOR_RELATIONSHIP Processor;
        PROBE_NUMA_NODE_RELATIONSHIP NumaNode;
        PROBE_CACHE_RELATIONSHIP Cache;
        PROBE_GROUP_RELATIONSHIP Group;
    } u;
} PROBE_SLPIEX;

static HANDLE g_com = INVALID_HANDLE_VALUE;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static volatile LONG g_place_stop;
static volatile DWORD g_place_counts[2][64];
static ULONG_PTR g_core_masks[64];
static DWORD g_core_count;

static void
emit_raw(const char *line)
{
    DWORD written;
    size_t len = strlen(line);
    char dbg[560];

    if (g_com != INVALID_HANDLE_VALUE) {
        WriteFile(g_com, line, (DWORD)len, &written, NULL);
        WriteFile(g_com, "\r\n", 2, &written, NULL);
        FlushFileBuffers(g_com);
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        WriteFile(g_file, line, (DWORD)len, &written, NULL);
        WriteFile(g_file, "\r\n", 2, &written, NULL);
        FlushFileBuffers(g_file);
    }
    /*
     * LiveImg_Debug keeps KD on COM1; CreateFile on COM1/COM2 can fail while
     * freeldr still wrote freeldr noise to the second -serial file. Mirror
     * every DIFF line through OutputDebugString so the KD serial capture
     * records it (tavera-4c1t.kd.log: Started setup, stub GLPIE, ExitProcess,
     * no DIFF on either serial file).
     */
    _snprintf(dbg, sizeof(dbg), "%s\n", line);
    dbg[sizeof(dbg) - 1] = '\0';
    OutputDebugStringA(dbg);
}

static void
emitf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    emit_raw(buf);
}

static HANDLE
open_com_port(const char *name)
{
    HANDLE handle;
    DCB dcb;

    handle = CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return handle;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        BuildCommDCBA("baud=115200 parity=N data=8 stop=1", &dcb);
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(handle, &dcb);
    return handle;
}

static HANDLE
open_com(void)
{
    /*
     * Windows WTG capture (this Invoke): single -serial file: is COM1.
     * Try COM1 then COM2. LiveImg_Debug with KD on COM1 can hang on
     * CreateFile — that path should keep using OutputDebugStringA (still
     * mirrored below) or skip COM; do not call this open on that boot.
     */
    HANDLE handle;

    handle = open_com_port("COM1");
    if (handle != INVALID_HANDLE_VALUE) {
        return handle;
    }
    return open_com_port("COM2");
}

static void
open_sink_log(void)
{
    char letter;
    char marker[32];
    char path[64];

    for (letter = 'A'; letter <= 'Z'; letter++) {
        _snprintf(marker, sizeof(marker), "%c:\\SINK.TXT", letter);
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        _snprintf(path, sizeof(path), "%c:\\result.log", letter);
        g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        return;
    }
}

static unsigned
popcount_mask(ULONG_PTR mask)
{
    unsigned n = 0;

    while (mask) {
        n += (unsigned)(mask & 1);
        mask >>= 1;
    }
    return n;
}

/*
 * Dump one deterministic-cache leaf (0x4 or 0x8000001D). Field widths and the
 * +1 size formula follow in-tree CPUID_CACHE_PARAMS_* comments; they are the
 * probe's read, not Windows Ex fields.
 */
static void
dump_cache_leaf(unsigned leaf, const char *tag, unsigned max_supported)
{
    unsigned sub;
    int info[4];

    if (max_supported < leaf) {
        emitf("DIFF CPUID %s absent max=0x%x", tag, max_supported);
        return;
    }
    for (sub = 0; sub < 32; sub++) {
        unsigned cache_type;
        unsigned cache_level;
        unsigned line;
        unsigned partitions;
        unsigned ways;
        unsigned sets;
        unsigned share_ids;
        unsigned long long size_bytes;

        __cpuidex(info, (int)leaf, (int)sub);
        cache_type = (unsigned)(info[0] & 0x1f);
        if (cache_type == PROBE_CACHE_TYPE_NULL) {
            emitf("DIFF CPUID %s sub=%u type=null", tag, sub);
            break;
        }
        cache_level = (unsigned)((info[0] >> 5) & 0x7);
        share_ids = (unsigned)((info[0] >> 14) & 0xfff);
        line = (unsigned)(info[1] & 0xfff) + 1u;
        partitions = (unsigned)((info[1] >> 12) & 0x3ff) + 1u;
        ways = (unsigned)((info[1] >> 22) & 0x3ff) + 1u;
        sets = (unsigned)info[2] + 1u;
        size_bytes = (unsigned long long)line * partitions * ways * sets;
        emitf("DIFF CPUID %s sub=%u type=%u level=%u line=%u ways=%u parts=%u sets=%u size=%llu share_ids_raw=%u eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x",
              tag, sub, cache_type, cache_level, line, ways, partitions, sets,
              size_bytes, share_ids,
              (unsigned)info[0], (unsigned)info[1], (unsigned)info[2], (unsigned)info[3]);
    }
}

static void
dump_cpuid(void)
{
    int info[4];
    unsigned max_basic;
    unsigned max_ext;
    unsigned sub;
    unsigned die = 0;
    unsigned module_lvl = 0;
    unsigned leaf = 0;

    __cpuid(info, 0);
    max_basic = (unsigned)info[0];
    emitf("DIFF CPUID max_basic=0x%x", max_basic);

    dump_cache_leaf(PROBE_CPUID_CACHE_PARAMS, "cache4", max_basic);

    if (max_basic >= PROBE_CPUID_V2_EXT_TOPOLOGY) {
        leaf = PROBE_CPUID_V2_EXT_TOPOLOGY;
    } else if (max_basic >= PROBE_CPUID_EXT_TOPOLOGY) {
        leaf = PROBE_CPUID_EXT_TOPOLOGY;
    }

    if (leaf != 0) {
        emitf("DIFF CPUID topology_leaf=0x%x", leaf);
        for (sub = 0; sub < 8; sub++) {
            unsigned level_type;
            unsigned shift;

            __cpuidex(info, (int)leaf, (int)sub);
            shift = (unsigned)(info[0] & 0x1f);
            level_type = (unsigned)((info[2] >> 8) & 0xff);
            emitf("DIFF CPUID topo sub=%u shift=%u level_type=%u edx=0x%x",
                  sub, shift, level_type, (unsigned)info[3]);
            if (level_type == 0 && shift == 0 && info[1] == 0) {
                break;
            }
            if (level_type == PROBE_LEVEL_DIE) {
                die = 1;
            }
            if (level_type == PROBE_LEVEL_MODULE) {
                module_lvl = 1;
            }
        }
    } else {
        emit_raw("DIFF CPUID topology_leaf=none");
    }

    __cpuid(info, (int)0x80000000);
    max_ext = (unsigned)info[0];
    emitf("DIFF CPUID max_ext=0x%x", max_ext);
    dump_cache_leaf(PROBE_CPUID_CACHE_PARAMS_AMD, "amdcache", max_ext);
    if (max_ext >= PROBE_CPUID_AMD_ADDR_SIZE) {
        unsigned nc;
        unsigned apic_core_id_size;

        __cpuid(info, (int)PROBE_CPUID_AMD_ADDR_SIZE);
        nc = (unsigned)(info[2] & 0xff);
        apic_core_id_size = (unsigned)((info[2] >> 12) & 0xf);
        emitf("DIFF CPUID amd80000008 nc=%u apic_core_id_size=%u", nc, apic_core_id_size);
    }
    if (max_ext >= PROBE_CPUID_AMD_TOPOLOGY) {
        unsigned core_id;
        unsigned threads_per_core;

        __cpuid(info, (int)PROBE_CPUID_AMD_TOPOLOGY);
        core_id = (unsigned)(info[1] & 0xff);
        threads_per_core = (unsigned)((info[1] >> 8) & 0xff);
        emitf("DIFF CPUID amd8000001e core_id=%u threads_per_core=%u eax=0x%x",
              core_id, threads_per_core, (unsigned)info[0]);
    }

    emitf("DIFF CPUID_DIE %u", die);
    emitf("DIFF CPUID_MODULE %u", module_lvl);
}

static int
core_index_for_cpu(DWORD cpu)
{
    DWORD i;
    ULONG_PTR bit;

    if (cpu >= 64) {
        return -1;
    }
    bit = (ULONG_PTR)1 << cpu;
    for (i = 0; i < g_core_count; i++) {
        if (g_core_masks[i] & bit) {
            return (int)i;
        }
    }
    return -1;
}

static DWORD WINAPI
place_worker(void *param)
{
    DWORD id = (DWORD)(ULONG_PTR)param;
    PFN_GetCurrentProcessorNumber get_cpu;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");

    get_cpu = (PFN_GetCurrentProcessorNumber)GetProcAddress(k32, "GetCurrentProcessorNumber");
    if (!get_cpu) {
        return 1;
    }

    if (id > 1) {
        return 1;
    }
    while (InterlockedCompareExchange(&g_place_stop, 0, 0) == 0) {
        DWORD cpu = get_cpu();
        if (cpu < 64) {
            InterlockedIncrement((volatile LONG *)&g_place_counts[id][cpu]);
        }
    }
    return 0;
}

static DWORD
majority_cpu(DWORD thread_id)
{
    DWORD i;
    DWORD best_cpu = 0;
    DWORD best = 0;

    for (i = 0; i < 64; i++) {
        DWORD n = g_place_counts[thread_id][i];
        if (n > best) {
            best = n;
            best_cpu = i;
        }
    }
    return best_cpu;
}

static void
run_placement(void)
{
    HANDLE t0;
    HANDLE t1;
    DWORD best0_cpu;
    DWORD best1_cpu;
    int c0;
    int c1;
    int shared;

    memset((void *)g_place_counts, 0, sizeof(g_place_counts));
    InterlockedExchange(&g_place_stop, 0);
    t0 = CreateThread(NULL, 0, place_worker, (void *)(ULONG_PTR)0, 0, NULL);
    t1 = CreateThread(NULL, 0, place_worker, (void *)(ULONG_PTR)1, 0, NULL);
    if (!t0 || !t1) {
        emit_raw("DIFF PLACE create_failed");
        if (t0) {
            CloseHandle(t0);
        }
        if (t1) {
            CloseHandle(t1);
        }
        return;
    }

    /* 200 ms sample window is the probe's read, not a Windows field. */
    Sleep(200);
    InterlockedExchange(&g_place_stop, 1);
    WaitForSingleObject(t0, 5000);
    WaitForSingleObject(t1, 5000);
    CloseHandle(t0);
    CloseHandle(t1);

    best0_cpu = majority_cpu(0);
    best1_cpu = majority_cpu(1);
    c0 = core_index_for_cpu(best0_cpu);
    c1 = core_index_for_cpu(best1_cpu);
    shared = (c0 >= 0 && c1 >= 0 && c0 == c1) ? 1 : 0;
    emitf("DIFF PLACE cpu_a=%u count_a=%u cpu_b=%u count_b=%u shared_core=%d",
          best0_cpu, g_place_counts[0][best0_cpu],
          best1_cpu, g_place_counts[1][best1_cpu],
          shared);
}

static void
dump_ex(PFN_GetLogicalProcessorInformationEx fn)
{
    DWORD len = 0;
    DWORD needed;
    BYTE *buf;
    BYTE *end;
    BYTE *cur;
    ULONG_PTR logical_or = 0;
    DWORD die_records = 0;
    DWORD module_records = 0;
    BOOL ok;

    SetLastError(0);
    ok = fn(RelationAll, NULL, &len);
    if (ok || GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        emitf("DIFF EX size_query ok=%d err=%u len=%u", ok ? 1 : 0, GetLastError(), len);
        return;
    }
    needed = len;
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
    if (!buf) {
        emit_raw("DIFF EX alloc_failed");
        return;
    }
    len = needed;
    if (!fn(RelationAll, buf, &len)) {
        emitf("DIFF EX fill_failed err=%u", GetLastError());
        HeapFree(GetProcessHeap(), 0, buf);
        return;
    }

    end = buf + len;
    cur = buf;
    g_core_count = 0;
    while (cur + sizeof(DWORD) * 2 <= end) {
        PROBE_SLPIEX *rec = (PROBE_SLPIEX *)cur;
        DWORD size = rec->Size;
        WORD gi;

        if (size < sizeof(DWORD) * 2 || cur + size > end) {
            emitf("DIFF EX bad_size size=%u", size);
            break;
        }

        switch (rec->Relationship) {
        case RelationProcessorCore:
        case RelationProcessorPackage:
        case RelationProcessorDie:
        case RelationProcessorModule: {
            ULONG_PTR mask = 0;
            WORD group = 0;
            if (rec->u.Processor.GroupCount >= 1) {
                mask = rec->u.Processor.GroupMask[0].Mask;
                group = rec->u.Processor.GroupMask[0].Group;
            }
            if (rec->Relationship == RelationProcessorCore) {
                emitf("DIFF REL core flags=%u eff=%u group=%u mask=0x%llx",
                      rec->u.Processor.Flags,
                      rec->u.Processor.EfficiencyClass,
                      group,
                      (unsigned long long)mask);
                logical_or |= mask;
                if (g_core_count < 64) {
                    g_core_masks[g_core_count++] = mask;
                }
            } else if (rec->Relationship == RelationProcessorPackage) {
                emitf("DIFF REL package flags=%u eff=%u group=%u mask=0x%llx",
                      rec->u.Processor.Flags,
                      rec->u.Processor.EfficiencyClass,
                      group,
                      (unsigned long long)mask);
            } else if (rec->Relationship == RelationProcessorDie) {
                die_records++;
                emitf("DIFF REL die flags=%u eff=%u group=%u mask=0x%llx",
                      rec->u.Processor.Flags,
                      rec->u.Processor.EfficiencyClass,
                      group,
                      (unsigned long long)mask);
            } else {
                module_records++;
                emitf("DIFF REL module flags=%u eff=%u group=%u mask=0x%llx",
                      rec->u.Processor.Flags,
                      rec->u.Processor.EfficiencyClass,
                      group,
                      (unsigned long long)mask);
            }
            break;
        }
        case RelationNumaNode:
        case RelationNumaNodeEx:
            emitf("DIFF REL numa node=%u group=%u mask=0x%llx",
                  rec->u.NumaNode.NodeNumber,
                  rec->u.NumaNode.GroupMask.Group,
                  (unsigned long long)rec->u.NumaNode.GroupMask.Mask);
            break;
        case RelationCache:
            emitf("DIFF REL cache level=%u assoc=%u line=%u size=%u type=%u group=%u mask=0x%llx",
                  rec->u.Cache.Level,
                  rec->u.Cache.Associativity,
                  rec->u.Cache.LineSize,
                  rec->u.Cache.CacheSize,
                  rec->u.Cache.Type,
                  rec->u.Cache.GroupMask.Group,
                  (unsigned long long)rec->u.Cache.GroupMask.Mask);
            break;
        case RelationGroup:
            emitf("DIFF REL group max=%u active=%u",
                  rec->u.Group.MaximumGroupCount,
                  rec->u.Group.ActiveGroupCount);
            for (gi = 0; gi < rec->u.Group.ActiveGroupCount && gi < 16; gi++) {
                emitf("DIFF REL groupinfo i=%u max=%u active=%u mask=0x%llx",
                      gi,
                      rec->u.Group.GroupInfo[gi].MaximumProcessorCount,
                      rec->u.Group.GroupInfo[gi].ActiveProcessorCount,
                      (unsigned long long)rec->u.Group.GroupInfo[gi].ActiveProcessorMask);
            }
            break;
        default:
            emitf("DIFF REL other relationship=%u size=%u", rec->Relationship, size);
            break;
        }
        cur += size;
    }

    emitf("DIFF LOGICAL %u", popcount_mask(logical_or));
    emitf("DIFF CORE_COUNT %u", g_core_count);
    emitf("DIFF EX_DIE_RECORDS %u", die_records);
    emitf("DIFF EX_MODULE_RECORDS %u", module_records);
    HeapFree(GetProcessHeap(), 0, buf);
}

int
main(int argc, char **argv)
{
    HMODULE k32;
    PFN_GetLogicalProcessorInformationEx get_ex;
    const char *shape = "unknown";

    if (argc >= 2 && argv[1] && argv[1][0]) {
        shape = argv[1];
    } else {
        /* Live CD CmdLine cannot carry a space (mkhive); optional shape.txt. */
        HANDLE shape_file = CreateFileA("C:\\temp\\shape.txt", GENERIC_READ, FILE_SHARE_READ,
                                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (shape_file != INVALID_HANDLE_VALUE) {
            static char shape_buf[64];
            DWORD got = 0;
            memset(shape_buf, 0, sizeof(shape_buf));
            if (ReadFile(shape_file, shape_buf, sizeof(shape_buf) - 1, &got, NULL) && got > 0) {
                while (got > 0 && (shape_buf[got - 1] == '\r' || shape_buf[got - 1] == '\n' ||
                                   shape_buf[got - 1] == ' ')) {
                    shape_buf[--got] = '\0';
                }
                if (shape_buf[0]) {
                    shape = shape_buf;
                }
            }
            CloseHandle(shape_file);
        }
    }

    g_com = open_com();
    open_sink_log();

    emitf("DIFF SHAPE %s", shape);
    emitf("DIFF BUILD probe-v2");

    k32 = GetModuleHandleA("kernel32.dll");
    get_ex = (PFN_GetLogicalProcessorInformationEx)GetProcAddress(
        k32, "GetLogicalProcessorInformationEx");
    if (!get_ex) {
        emit_raw("DIFF EX MISSING");
        emit_raw("DIFF END OK");
        return 1;
    }

    dump_ex(get_ex);
    dump_cpuid();
    run_placement();
    emit_raw("DIFF END OK");

    if (g_com != INVALID_HANDLE_VALUE) {
        CloseHandle(g_com);
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
    }
    return 0;
}
