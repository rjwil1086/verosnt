/*
 * PROJECT:     Tevera OS
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/probes/ntfs/probe-v4/host/ntfs_raw_dump.c
 * PURPOSE:     Dump NTFS regions for differential probe-v2 captures
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Host side of the raw-format probe. Reads a raw disk image Windows wrote
 * and emits capture blobs plus a manifesto. Uses boot/MFT/attribute-type
 * constants already in sdk/lib/fslib/ntfslib/ntfslib.h and the same reserved
 * names as MS-FSCC. It does not invent field meanings; the specs cite the
 * resulting files under output/ntfs/probe-v1/.
 *
 * Sources:
 * - sdk/lib/fslib/ntfslib/ntfslib.h (boot offsets, attribute types, record size)
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85
 * - https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATTR_STDINFO   0x10
#define ATTR_FILENAME  0x30
#define ATTR_DATA      0x80
#define ATTR_INDEXROOT 0x90
#define ATTR_INDEXALLOC 0xA0
#define ATTR_END       0xFFFFFFFFu

#define MAX_RUNS 128
#define NAME_CAP 128

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

static int utf16_eq_ascii(const uint8_t *name, int chars, const char *ascii)
{
    int i;
    for (i = 0; ascii[i]; i++) {
        if (i >= chars)
            return 0;
        if (r16(name + i * 2) != (uint16_t)(unsigned char)ascii[i])
            return 0;
    }
    return i == chars;
}

static int write_blob(const char *path, const uint8_t *data, size_t n)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
        return -1;
    if (fwrite(data, 1, n, fp) != n) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int read_record(const uint8_t *img, size_t n, uint64_t vol,
                       const int64_t *lcns, const uint64_t *lens, int runs,
                       uint32_t cluster, uint32_t rec_bytes, uint64_t index,
                       uint8_t *rec)
{
    uint64_t want = index * rec_bytes;
    uint64_t pos = 0;
    int i;

    for (i = 0; i < runs; i++) {
        uint64_t bytes = lens[i] * cluster;
        if (want >= pos && want + rec_bytes <= pos + bytes) {
            uint64_t off = vol + (uint64_t)lcns[i] * cluster + (want - pos);
            if (lcns[i] < 0 || off + rec_bytes > n)
                return -1;
            memcpy(rec, img + off, rec_bytes);
            return apply_fixup(rec, rec_bytes, 512);
        }
        pos += bytes;
    }
    return -1;
}

static const uint8_t *find_attr(const uint8_t *rec, uint32_t used, uint32_t type,
                                const char *stream_name, int name_chars)
{
    uint32_t aoff = r16(rec + 0x14);
    while (aoff + 0x18 <= used) {
        uint32_t at = r32(rec + aoff);
        uint32_t alen = r32(rec + aoff + 4);
        uint8_t nonres = rec[aoff + 8];
        uint8_t nlen = rec[aoff + 9];
        uint16_t noff = r16(rec + aoff + 0x0A);
        if (at == ATTR_END || alen < 0x18)
            break;
        if (at == type) {
            int match = 1;
            if (stream_name) {
                if (nlen != name_chars)
                    match = 0;
                else if (!utf16_eq_ascii(rec + aoff + noff, nlen, stream_name))
                    match = 0;
            } else if (nlen != 0) {
                match = 0;
            }
            if (match)
                return rec + aoff;
        }
        (void)nonres;
        aoff += alen;
    }
    return NULL;
}

static int dump_nonresident(const uint8_t *img, size_t n, uint64_t vol,
                            uint32_t cluster, const uint8_t *attr,
                            const char *out_path, FILE *man, const char *label)
{
    uint16_t run_off = r16(attr + 0x20);
    uint32_t alen = r32(attr + 4);
    int64_t lcns[MAX_RUNS];
    uint64_t lens[MAX_RUNS];
    int runs = 0;
    uint64_t data_size = (uint64_t)r64(attr + 0x30);
    uint8_t *buf;
    uint64_t got = 0;
    int i;

    if (attr[8] == 0)
        return -1;
    if (decode_runs(attr + run_off, attr + alen, lcns, lens, MAX_RUNS, &runs) != 0)
        return -1;
    if (data_size == 0) {
        for (i = 0; i < runs; i++) {
            if (lcns[i] >= 0)
                data_size += lens[i] * cluster;
        }
    }
    /* Cap dump for oversized $LogFile: first 1 MiB is enough for a first pass. */
    if (data_size > 1024ull * 1024ull)
        data_size = 1024ull * 1024ull;
    buf = (uint8_t *)calloc(1, (size_t)data_size ? (size_t)data_size : 1);
    if (buf == NULL)
        return -1;
    for (i = 0; i < runs && got < data_size; i++) {
        uint64_t take = lens[i] * cluster;
        uint64_t off;
        if (lcns[i] < 0) {
            if (got + take > data_size)
                take = data_size - got;
            got += take;
            continue;
        }
        off = vol + (uint64_t)lcns[i] * cluster;
        if (got + take > data_size)
            take = data_size - got;
        if (off + take > n) {
            free(buf);
            return -1;
        }
        memcpy(buf + got, img + off, (size_t)take);
        fprintf(man, "RUN\t%s\tlcn=%lld\tclusters=%llu\tfile_off=%llu\n",
                label, (long long)lcns[i], (unsigned long long)lens[i],
                (unsigned long long)got);
        got += take;
    }
    if (write_blob(out_path, buf, (size_t)got) != 0) {
        free(buf);
        return -1;
    }
    fprintf(man, "BLOB\t%s\t%s\tbytes=%llu\n", label, out_path, (unsigned long long)got);
    free(buf);
    return 0;
}

static int dump_resident(const uint8_t *attr, const char *out_path, FILE *man,
                         const char *label)
{
    uint32_t vlen = r32(attr + 0x10);
    uint16_t voff = r16(attr + 0x14);
    if (attr[8] != 0)
        return -1;
    if (write_blob(out_path, attr + voff, vlen) != 0)
        return -1;
    fprintf(man, "BLOB\t%s\t%s\tbytes=%u\tresident=1\n", label, out_path, vlen);
    return 0;
}

static int filename_in_record(const uint8_t *rec, uint32_t used, const char *want)
{
    uint32_t aoff = r16(rec + 0x14);
    while (aoff + 0x18 <= used) {
        uint32_t at = r32(rec + aoff);
        uint32_t alen = r32(rec + aoff + 4);
        if (at == ATTR_END || alen < 0x18)
            break;
        if (at == ATTR_FILENAME && rec[aoff + 8] == 0) {
            uint32_t vlen = r32(rec + aoff + 0x10);
            uint16_t voff = r16(rec + aoff + 0x14);
            const uint8_t *val = rec + aoff + voff;
            if (vlen >= 0x42) {
                int nlen = val[0x40];
                if (utf16_eq_ascii(val + 0x42, nlen, want))
                    return 1;
            }
        }
        aoff += alen;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *image;
    const char *outdir;
    FILE *fp;
    FILE *man;
    uint8_t *img;
    size_t n, got;
    uint64_t vol = 0;
    uint32_t bps, spc, cluster, rec_bytes;
    uint64_t mft_lcn;
    int8_t cpr;
    uint8_t *rec;
    int64_t lcns[MAX_RUNS];
    uint64_t lens[MAX_RUNS];
    int runs = 0;
    char path[512];
    char manpath[512];
    int i;
    int found_quota = 0;
    int found_secret_a = 0;
    int found_secret_b = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: ntfs_raw_dump <image> <outdir>\n");
        return 2;
    }
    image = argv[1];
    outdir = argv[2];

    fp = fopen(image, "rb");
    if (fp == NULL) {
        fprintf(stderr, "open image failed\n");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    img = (uint8_t *)malloc(n);
    if (img == NULL) {
        fclose(fp);
        return 1;
    }
    got = fread(img, 1, n, fp);
    fclose(fp);
    if (got != n) {
        free(img);
        return 1;
    }

    if (n >= 512 && memcmp(img + 3, "NTFS    ", 8) != 0) {
        int part;
        for (part = 0; part < 4; part++) {
            const uint8_t *ent = img + 0x1BE + part * 16;
            if (ent[4] == 0x07) {
                vol = (uint64_t)r32(ent + 8) * 512;
                break;
            }
        }
    }
    if (vol + 512 > n || memcmp(img + vol + 3, "NTFS    ", 8) != 0) {
        fprintf(stderr, "no NTFS boot sector\n");
        free(img);
        return 1;
    }

    snprintf(manpath, sizeof(manpath), "%s/manifest.txt", outdir);
    man = fopen(manpath, "wb");
    if (man == NULL) {
        free(img);
        return 1;
    }
    fprintf(man, "NTFS-RAW-DUMP\n");
    fprintf(man, "IMAGE\t%s\n", image);
    fprintf(man, "VOL_OFF\t%llu\n", (unsigned long long)vol);

    snprintf(path, sizeof(path), "%s/boot_sector.bin", outdir);
    if (write_blob(path, img + vol, 512) != 0) {
        fclose(man);
        free(img);
        return 1;
    }
    fprintf(man, "BLOB\tboot_sector\t%s\tbytes=512\n", path);

    bps = r16(img + vol + 0x0B);
    spc = img[vol + 0x0D];
    cluster = bps * spc;
    mft_lcn = r64(img + vol + 0x30);
    cpr = (int8_t)img[vol + 0x40];
    if (cpr > 0)
        rec_bytes = (uint32_t)cpr * cluster;
    else if (cpr < 0)
        rec_bytes = 1u << (uint32_t)(-cpr);
    else
        rec_bytes = 0;
    fprintf(man, "FIELD\tbps\t%u\n", bps);
    fprintf(man, "FIELD\tspc\t%u\n", spc);
    fprintf(man, "FIELD\tcluster\t%u\n", cluster);
    fprintf(man, "FIELD\tmft_lcn\t%llu\n", (unsigned long long)mft_lcn);
    fprintf(man, "FIELD\trec_bytes\t%u\n", rec_bytes);

    rec = (uint8_t *)malloc(rec_bytes);
    if (rec == NULL || rec_bytes < 512) {
        fclose(man);
        free(img);
        free(rec);
        return 1;
    }
    memcpy(rec, img + vol + mft_lcn * cluster, rec_bytes);
    if (apply_fixup(rec, rec_bytes, bps) != 0) {
        fprintf(stderr, "MFT record 0 fixup failed\n");
        fclose(man);
        free(img);
        free(rec);
        return 1;
    }
    {
        const uint8_t *data = find_attr(rec, r32(rec + 0x18), ATTR_DATA, NULL, 0);
        if (data == NULL || data[8] == 0 ||
            decode_runs(data + r16(data + 0x20), data + r32(data + 4),
                        lcns, lens, MAX_RUNS, &runs) != 0) {
            fprintf(stderr, "MFT data runs failed\n");
            fclose(man);
            free(img);
            free(rec);
            return 1;
        }
    }

    /* Record 2 is $LogFile per MS-FSCC / How NTFS Works / ntfslib METAFILE_LOGFILE. */
    if (read_record(img, n, vol, lcns, lens, runs, cluster, rec_bytes, 2, rec) != 0) {
        fprintf(stderr, "read $LogFile record failed\n");
        fclose(man);
        free(img);
        free(rec);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/mft_record_2_logfile.bin", outdir);
    write_blob(path, rec, rec_bytes);
    fprintf(man, "BLOB\tmft_record_2\t%s\tbytes=%u\n", path, rec_bytes);
    {
        const uint8_t *data = find_attr(rec, r32(rec + 0x18), ATTR_DATA, NULL, 0);
        if (data) {
            snprintf(path, sizeof(path), "%s/logfile_data.bin", outdir);
            if (data[8])
                dump_nonresident(img, n, vol, cluster, data, path, man, "logfile_data");
            else
                dump_resident(data, path, man, "logfile_data");
        }
    }

    /* Scan MFT for $Quota and secret.txt by $FILE_NAME (user files may be past 256). */
    for (i = 0; i < 4096; i++) {
        uint32_t used;
        if (read_record(img, n, vol, lcns, lens, runs, cluster, rec_bytes, (uint64_t)i, rec) != 0)
            continue;
        if (memcmp(rec, "FILE", 4) != 0)
            continue;
        used = r32(rec + 0x18);
        if (used > rec_bytes)
            continue;
        if (!found_quota && filename_in_record(rec, used, "$Quota")) {
            const uint8_t *q = find_attr(rec, used, ATTR_INDEXALLOC, "$Q", 2);
            const uint8_t *qr = find_attr(rec, used, ATTR_INDEXROOT, "$Q", 2);
            snprintf(path, sizeof(path), "%s/mft_record_%d_quota.bin", outdir, i);
            write_blob(path, rec, rec_bytes);
            fprintf(man, "BLOB\tmft_quota_record\t%s\trecord=%d\n", path, i);
            if (q) {
                snprintf(path, sizeof(path), "%s/quota_q_index_allocation.bin", outdir);
                dump_nonresident(img, n, vol, cluster, q, path, man, "quota_q_index_allocation");
            }
            if (qr) {
                snprintf(path, sizeof(path), "%s/quota_q_index_root.bin", outdir);
                dump_resident(qr, path, man, "quota_q_index_root");
            }
            found_quota = 1;
        }
        if (!found_secret_a && filename_in_record(rec, used, "secretA.txt")) {
            const uint8_t *data = find_attr(rec, used, ATTR_DATA, NULL, 0);
            const uint8_t *efs = find_attr(rec, used, 0x100, "$EFS", 4);
            snprintf(path, sizeof(path), "%s/mft_record_%d_secretA.bin", outdir, i);
            write_blob(path, rec, rec_bytes);
            fprintf(man, "BLOB\tmft_secretA_record\t%s\trecord=%d\n", path, i);
            if (data) {
                snprintf(path, sizeof(path), "%s/secretA_data.bin", outdir);
                if (data[8])
                    dump_nonresident(img, n, vol, cluster, data, path, man, "secretA_data");
                else
                    dump_resident(data, path, man, "secretA_data");
            }
            if (efs) {
                snprintf(path, sizeof(path), "%s/secretA_logged_utility.bin", outdir);
                if (efs[8])
                    dump_nonresident(img, n, vol, cluster, efs, path, man, "secretA_logged_utility");
                else
                    dump_resident(efs, path, man, "secretA_logged_utility");
            }
            found_secret_a = 1;
        }
        if (!found_secret_b && filename_in_record(rec, used, "secretB.txt")) {
            const uint8_t *data = find_attr(rec, used, ATTR_DATA, NULL, 0);
            const uint8_t *efs = find_attr(rec, used, 0x100, "$EFS", 4);
            snprintf(path, sizeof(path), "%s/mft_record_%d_secretB.bin", outdir, i);
            write_blob(path, rec, rec_bytes);
            fprintf(man, "BLOB\tmft_secretB_record\t%s\trecord=%d\n", path, i);
            if (data) {
                snprintf(path, sizeof(path), "%s/secretB_data.bin", outdir);
                if (data[8])
                    dump_nonresident(img, n, vol, cluster, data, path, man, "secretB_data");
                else
                    dump_resident(data, path, man, "secretB_data");
            }
            if (efs) {
                snprintf(path, sizeof(path), "%s/secretB_logged_utility.bin", outdir);
                if (efs[8])
                    dump_nonresident(img, n, vol, cluster, efs, path, man, "secretB_logged_utility");
                else
                    dump_resident(efs, path, man, "secretB_logged_utility");
            }
            found_secret_b = 1;
        }
        if (found_quota && found_secret_a && found_secret_b)
            break;
    }

    fprintf(man, "FOUND\tquota\t%d\n", found_quota);
    fprintf(man, "FOUND\tsecretA\t%d\n", found_secret_a);
    fprintf(man, "FOUND\tsecretB\t%d\n", found_secret_b);
    fprintf(man, "NTFS-RAW-DUMP-END\n");
    fclose(man);
    free(img);
    free(rec);
    printf("wrote %s\n", manpath);
    return 0;
}
