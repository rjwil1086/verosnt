/*
 * PROJECT:     Tavera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v8/host/ntfs_mount_frs_dump.c
 * PURPOSE:     Dump named FRS from kept disk for host-mount chkdsk leads
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Raw-format probe only. Streams the image (no full mmap). Dumps FRS listed
 * on the command line (defaults: 0x17, 0x7C7, 0x7D3 from issue-22 chkdsk text
 * in output/ntfs/issue-22-host-mount/host-mount-obs.txt). Consistency checks
 * mirror drivers/filesystems/ntfs/fsctl.c NtfsFileRecordIsConsistent using
 * ATTR_RECORD / FILE record fields already in sdk/lib/fslib/ntfslib/ntfslib.h
 * and drivers/filesystems/ntfs/ntfs.h. Do not edit ntfs.sys here.
 *
 * Sources:
 * - drivers/filesystems/ntfs/fsctl.c (NtfsFileRecordIsConsistent)
 * - sdk/lib/fslib/ntfslib/ntfslib.h (ATTR_RECORD / file-record offsets)
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702
 * - https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk
 */

#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define fseeko _fseeki64
#endif

#define ATTR_DATA 0x80
#define ATTR_FILENAME 0x30
#define ATTR_END 0xFFFFFFFFu
#define MAX_RUNS 128
#define MAX_FRS 16

static uint16_t r16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t r64(const uint8_t *p)
{
    return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

static int apply_fixup(uint8_t *rec, uint32_t rec_bytes, uint32_t sector)
{
    uint16_t usa_off = r16(rec + 4);
    uint16_t usa_count = r16(rec + 6);
    uint32_t sectors;
    uint32_t i;
    uint16_t usn;

    if (sector < 512 || rec_bytes < sector || (rec_bytes % sector) != 0)
        return -1;
    sectors = rec_bytes / sector;
    if (usa_count != sectors + 1)
        return -1;
    if ((uint32_t)usa_off + (uint32_t)usa_count * 2 > rec_bytes)
        return -1;
    usn = r16(rec + usa_off);
    for (i = 0; i < sectors; i++) {
        uint8_t *tail = rec + (i + 1) * sector - 2;
        if (r16(tail) != usn)
            return -1;
        tail[0] = rec[usa_off + 2 + i * 2];
        tail[1] = rec[usa_off + 3 + i * 2];
    }
    return 0;
}

static int decode_runs(const uint8_t *p, const uint8_t *end,
                       int64_t *lcns, uint64_t *lens, int max, int *out)
{
    int64_t lcn = 0;
    int n = 0;

    while (p < end && *p != 0) {
        uint8_t head = *p++;
        int llen = head & 0x0f;
        int olen = head >> 4;
        uint64_t len = 0;
        int64_t off = 0;
        int i;

        if (llen == 0 || llen > 8 || olen > 8 || p + llen + olen > end)
            return -1;
        for (i = 0; i < llen; i++)
            len |= (uint64_t)p[i] << (8 * i);
        p += llen;
        if (olen) {
            for (i = 0; i < olen; i++)
                off |= (int64_t)p[i] << (8 * i);
            if (p[olen - 1] & 0x80) {
                for (i = olen; i < 8; i++)
                    off |= (int64_t)0xff << (8 * i);
            }
            p += olen;
            lcn += off;
        }
        if (n >= max)
            return -1;
        lens[n] = len;
        lcns[n] = (olen == 0) ? (int64_t)-1 : lcn;
        n++;
    }
    *out = n;
    return 0;
}

static int read_at(FILE *fp, uint64_t off, void *buf, size_t n)
{
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, n, fp) == n ? 0 : -1;
}

static int read_record(FILE *fp, uint64_t vol, const int64_t *lcns,
                       const uint64_t *lens, int runs, uint32_t cluster,
                       uint32_t rec_bytes, uint32_t bps, uint64_t index,
                       uint8_t *rec)
{
    uint64_t want = index * rec_bytes;
    uint64_t pos = 0;
    int i;

    for (i = 0; i < runs; i++) {
        uint64_t bytes = lens[i] * cluster;
        if (want >= pos && want + rec_bytes <= pos + bytes) {
            uint64_t off = vol + (uint64_t)lcns[i] * cluster + (want - pos);
            if (lcns[i] < 0)
                return -1;
            if (read_at(fp, off, rec, rec_bytes) != 0)
                return -1;
            return apply_fixup(rec, rec_bytes, bps);
        }
        pos += bytes;
    }
    return -1;
}

static void name_ascii(const uint8_t *attr, char *out, int outcap)
{
    uint8_t nlen = attr[9];
    uint16_t noff = r16(attr + 0x0A);
    int i;
    int o = 0;
    out[0] = 0;
    if (nlen == 0 || noff + nlen * 2 > r32(attr + 4))
        return;
    for (i = 0; i < nlen && o + 1 < outcap; i++) {
        uint16_t c = r16(attr + noff + i * 2);
        out[o++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[o] = 0;
}

/* Same bounds as NtfsFileRecordIsConsistent (fsctl.c). */
static void check_record(FILE *rep, const uint8_t *rec, uint32_t rec_bytes,
                         uint64_t index, uint32_t cluster)
{
    uint32_t used;
    uint32_t off;
    int attr_i = 0;

    fprintf(rep, "FRS\t0x%llX\n", (unsigned long long)index);
    if (memcmp(rec, "FILE", 4) != 0) {
        fprintf(rep, "FAIL\tmagic_not_FILE\n");
        return;
    }
    used = r32(rec + 0x18);
    off = r16(rec + 0x14);
    fprintf(rep, "FIELD\tbytes_in_use\t%u\n", used);
    fprintf(rep, "FIELD\tattr_offset\t%u\n", off);
    if (off < 0x30 || used > rec_bytes || off > used) {
        fprintf(rep, "FAIL\trecord_header_bounds\n");
        return;
    }

    while (off + 0x18 <= used) {
        const uint8_t *attr = rec + off;
        uint32_t type = r32(attr);
        uint32_t alen = r32(attr + 4);
        uint8_t nonres = attr[8];
        char nm[64];
        int fail = 0;

        if (type == ATTR_END || alen < 0x18)
            break;
        if (alen > used - off || (alen & 7) != 0) {
            fprintf(rep, "FAIL\tattr_length\ttype=0x%X\toffset=%u\tlen=%u\n",
                    type, off, alen);
            break;
        }
        name_ascii(attr, nm, (int)sizeof(nm));
        fprintf(rep, "ATTR\ttype=0x%X\tnonres=%u\tlen=%u\tname=%s\n",
                type, nonres, alen, nm[0] ? nm : "(unnamed)");

        if (nonres == 0) {
            uint32_t vlen = r32(attr + 0x10);
            uint16_t voff = r16(attr + 0x14);
            fprintf(rep, "RES\tvlen=%u\tvoff=%u\n", vlen, voff);
            if (vlen > alen || voff > alen - vlen)
                fail = 1;
        } else if (nonres == 1) {
            uint16_t mpoff = r16(attr + 0x20);
            fprintf(rep, "NONRES\tmapping_pairs_off=%u\tlowest_vcn=%llu\thighest_vcn=%llu\t"
                    "allocated=%lld\tdata_size=%lld\tinit_size=%lld\n",
                    mpoff,
                    (unsigned long long)r64(attr + 0x10),
                    (unsigned long long)r64(attr + 0x18),
                    (long long)r64(attr + 0x28),
                    (long long)r64(attr + 0x30),
                    (long long)r64(attr + 0x38));
            if (mpoff < 0x40 || mpoff > alen)
                fail = 1;
            else {
                int64_t lcns[MAX_RUNS];
                uint64_t lens[MAX_RUNS];
                int runs = 0;
                uint64_t lowest = r64(attr + 0x10);
                uint64_t highest = r64(attr + 0x18);
                int64_t allocated = (int64_t)r64(attr + 0x28);
                uint64_t sum = 0;
                int ri;
                if (decode_runs(attr + mpoff, attr + alen, lcns, lens, MAX_RUNS, &runs) != 0) {
                    fprintf(rep, "FAIL\trun_decode\ttype=0x%X\n", type);
                    fail = 1;
                } else {
                    for (ri = 0; ri < runs; ri++)
                        sum += lens[ri];
                    fprintf(rep, "RUNS\tcount=%d\tsum_clusters=%llu\tvcn_span=%llu\n",
                            runs,
                            (unsigned long long)sum,
                            (unsigned long long)(highest >= lowest ? (highest - lowest + 1) : 0));
                    if (highest >= lowest && sum != (highest - lowest + 1)) {
                        fprintf(rep, "FAIL\trun_vs_vcn\ttype=0x%X\tname=%s\tsum=%llu\tspan=%llu\n",
                                type, nm[0] ? nm : "(unnamed)",
                                (unsigned long long)sum,
                                (unsigned long long)(highest - lowest + 1));
                    }
                    fprintf(rep, "ALLOC_BYTES\t%lld\n", (long long)allocated);
                    if (cluster != 0 && allocated >= 0) {
                        uint64_t alloc_clusters = (uint64_t)allocated / cluster;
                        fprintf(rep, "ALLOC_CLUSTERS\t%llu\n",
                                (unsigned long long)alloc_clusters);
                        if ((uint64_t)allocated % cluster != 0)
                            fprintf(rep, "FAIL\talloc_not_cluster_multiple\ttype=0x%X\tname=%s\n",
                                    type, nm[0] ? nm : "(unnamed)");
                        if (alloc_clusters != sum)
                            fprintf(rep,
                                    "FAIL\talloc_vs_runs\ttype=0x%X\tname=%s\t"
                                    "alloc_clusters=%llu\trun_sum=%llu\n",
                                    type, nm[0] ? nm : "(unnamed)",
                                    (unsigned long long)alloc_clusters,
                                    (unsigned long long)sum);
                    }
                }
            }
        } else {
            fail = 1;
        }

        if (fail)
            fprintf(rep, "FAIL\tattr_bounds\ttype=0x%X\tname=%s\toffset=%u\n",
                    type, nm[0] ? nm : "(unnamed)", off);
        else if (type == ATTR_DATA)
            fprintf(rep, "DATA_ATTR_OK\tname=%s\n", nm[0] ? nm : "(unnamed)");

        /* FILE_NAME for identity */
        if (type == ATTR_FILENAME && nonres == 0) {
            uint16_t voff = r16(attr + 0x14);
            uint32_t vlen = r32(attr + 0x10);
            if (voff + 0x42 <= alen && vlen >= 0x42) {
                const uint8_t *fn = attr + voff;
                uint8_t nlen = fn[0x40];
                char fname[128];
                int i;
                int o = 0;
                for (i = 0; i < nlen && o + 1 < (int)sizeof(fname); i++) {
                    uint16_t c = r16(fn + 0x42 + i * 2);
                    fname[o++] = (c >= 32 && c < 127) ? (char)c : '?';
                }
                fname[o] = 0;
                fprintf(rep, "FILENAME\t%s\n", fname);
            }
        }

        off += alen;
        attr_i++;
        if (attr_i > 256)
            break;
    }
    fprintf(rep, "FRS_END\t0x%llX\n", (unsigned long long)index);
}

static int write_blob(const char *path, const uint8_t *data, size_t n)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    if (fwrite(data, 1, n, fp) != n) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *image;
    const char *outdir;
    FILE *fp;
    FILE *rep;
    uint8_t boot[512];
    uint8_t *rec = NULL;
    uint64_t vol = 0;
    uint32_t bps, spc, cluster, rec_bytes;
    uint64_t mft_lcn;
    int8_t cpr;
    int64_t lcns[MAX_RUNS];
    uint64_t lens[MAX_RUNS];
    int runs = 0;
    uint64_t targets[MAX_FRS];
    int ntargets = 0;
    int i;
    char path[512];
    char reppath[512];

    if (argc < 3) {
        fprintf(stderr, "usage: ntfs_mount_frs_dump <image> <outdir> [frs_hex...]\n");
        return 2;
    }
    image = argv[1];
    outdir = argv[2];
    if (argc == 3) {
        targets[0] = 0x17;
        targets[1] = 0x7C7;
        targets[2] = 0x7D3;
        ntargets = 3;
    } else {
        for (i = 3; i < argc && ntargets < MAX_FRS; i++) {
            targets[ntargets++] = strtoull(argv[i], NULL, 0);
        }
    }

    fp = fopen(image, "rb");
    if (!fp) {
        fprintf(stderr, "open image failed\n");
        return 1;
    }
    if (read_at(fp, 0, boot, 512) != 0) {
        fclose(fp);
        return 1;
    }
    if (memcmp(boot + 3, "NTFS    ", 8) != 0) {
        int part;
        for (part = 0; part < 4; part++) {
            const uint8_t *ent = boot + 0x1BE + part * 16;
            if (ent[4] == 0x07) {
                vol = (uint64_t)r32(ent + 8) * 512;
                break;
            }
        }
        if (read_at(fp, vol, boot, 512) != 0 || memcmp(boot + 3, "NTFS    ", 8) != 0) {
            fprintf(stderr, "no NTFS boot sector\n");
            fclose(fp);
            return 1;
        }
    }

    bps = r16(boot + 0x0B);
    spc = boot[0x0D];
    cluster = bps * spc;
    mft_lcn = r64(boot + 0x30);
    cpr = (int8_t)boot[0x40];
    if (cpr > 0)
        rec_bytes = (uint32_t)cpr * cluster;
    else if (cpr < 0)
        rec_bytes = 1u << (uint32_t)(-cpr);
    else
        rec_bytes = 0;
    if (rec_bytes < 512 || bps == 0) {
        fclose(fp);
        return 1;
    }

    rec = (uint8_t *)malloc(rec_bytes);
    if (!rec) {
        fclose(fp);
        return 1;
    }
    if (read_at(fp, vol + mft_lcn * cluster, rec, rec_bytes) != 0 ||
        apply_fixup(rec, rec_bytes, bps) != 0) {
        fprintf(stderr, "MFT record 0 failed\n");
        free(rec);
        fclose(fp);
        return 1;
    }
    {
        /* unnamed $DATA on record 0 */
        uint32_t used = r32(rec + 0x18);
        uint32_t aoff = r16(rec + 0x14);
        const uint8_t *data = NULL;
        while (aoff + 0x18 <= used) {
            uint32_t type = r32(rec + aoff);
            uint32_t alen = r32(rec + aoff + 4);
            if (type == ATTR_END || alen < 0x18)
                break;
            if (type == ATTR_DATA && rec[aoff + 9] == 0 && rec[aoff + 8] == 1) {
                data = rec + aoff;
                break;
            }
            aoff += alen;
        }
        if (!data || decode_runs(data + r16(data + 0x20), data + r32(data + 4),
                                 lcns, lens, MAX_RUNS, &runs) != 0) {
            fprintf(stderr, "MFT data runs failed\n");
            free(rec);
            fclose(fp);
            return 1;
        }
    }

    snprintf(reppath, sizeof(reppath), "%s/mount-frs-report.txt", outdir);
    rep = fopen(reppath, "wb");
    if (!rep) {
        free(rec);
        fclose(fp);
        return 1;
    }
    fprintf(rep, "NTFS-MOUNT-FRS-REPORT\n");
    fprintf(rep, "IMAGE\t%s\n", image);
    fprintf(rep, "VOL_OFF\t%llu\n", (unsigned long long)vol);
    fprintf(rep, "FIELD\tbps\t%u\n", bps);
    fprintf(rep, "FIELD\tcluster\t%u\n", cluster);
    fprintf(rep, "FIELD\trec_bytes\t%u\n", rec_bytes);
    fprintf(rep, "FIELD\tmft_lcn\t%llu\n", (unsigned long long)mft_lcn);

    snprintf(path, sizeof(path), "%s/boot_sector.bin", outdir);
    write_blob(path, boot, 512);
    fprintf(rep, "BLOB\tboot_sector\t%s\n", path);

    for (i = 0; i < ntargets; i++) {
        if (read_record(fp, vol, lcns, lens, runs, cluster, rec_bytes, bps,
                        targets[i], rec) != 0) {
            fprintf(rep, "FRS\t0x%llX\n", (unsigned long long)targets[i]);
            fprintf(rep, "FAIL\tread_or_fixup\n");
            fprintf(rep, "FRS_END\t0x%llX\n", (unsigned long long)targets[i]);
            continue;
        }
        snprintf(path, sizeof(path), "%s/mft_record_0x%llX.bin", outdir,
                 (unsigned long long)targets[i]);
        write_blob(path, rec, rec_bytes);
        fprintf(rep, "BLOB\tmft_record\t0x%llX\t%s\n",
                (unsigned long long)targets[i], path);
        check_record(rep, rec, rec_bytes, targets[i], cluster);
    }

    fprintf(rep, "NTFS-MOUNT-FRS-REPORT-END\n");
    fclose(rep);
    free(rec);
    fclose(fp);
    printf("wrote %s\n", reppath);
    return 0;
}
