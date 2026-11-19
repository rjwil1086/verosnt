/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/test-suites/ntfs/host/ntfs_check.c
 * PURPOSE:     Walk a raw disk image.
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
 */

/*
 * Walk a raw disk image. A file record, run, or cluster bitmap that
 * disagrees with the volume is corrupt.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RUNS 128

static int g_quiet;

static int fail(const char *why)
{
    if (!g_quiet)
        printf("CHECK corrupt %s\n", why);
    return -1;
}

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
                       int64_t *lcns, uint64_t *lens, int cap, int *n)
{
    int64_t lcn = 0;
    *n = 0;
    while (p < end && *p != 0) {
        uint8_t head = *p++;
        int llen = head & 0x0f;
        int olen = head >> 4;
        uint64_t len = 0;
        int i;
        if (llen == 0 || llen > 8 || olen > 8 || p + llen + olen > end)
            return -1;
        for (i = 0; i < llen; i++)
            len |= (uint64_t)(*p++) << (8 * i);
        if (len == 0)
            return -1;
        if (olen == 0) {
            lcn = -1;
        } else {
            int64_t delta = 0;
            for (i = 0; i < olen; i++)
                delta |= (int64_t)(*p++) << (8 * i);
            if (olen < 8 && (p[-1] & 0x80)) {
                int b;
                for (b = olen; b < 8; b++)
                    delta |= (int64_t)0xff << (8 * b);
            }
            lcn += delta;
        }
        if (*n >= cap)
            return -1;
        lcns[*n] = lcn;
        lens[*n] = len;
        (*n)++;
    }
    return 0;
}

/* Returns 0 when the record's attributes and runs stay inside the volume. */
static int walk_record(uint8_t *rec, uint32_t rec_bytes, uint32_t sector,
                       uint64_t clusters, int collect,
                       int64_t *lcns, uint64_t *lens, int *runs, int cap)
{
    uint32_t off;
    uint32_t used;
    if (memcmp(rec, "FILE", 4) != 0)
        return -1;
    if (apply_fixup(rec, rec_bytes, sector) != 0)
        return -1;
    off = r16(rec + 0x14);
    used = r32(rec + 0x18);
    if (off < 0x30 || used > rec_bytes || off > used)
        return -1;
    while (off + 8 <= used) {
        uint32_t type = r32(rec + off);
        uint32_t len = r32(rec + off + 4);
        uint8_t nonres;
        if (type == 0xffffffff)
            return 0;
        if (len < 24 || (len & 7) != 0 || off + len > used)
            return -1;
        nonres = rec[off + 8];
        if (nonres == 0) {
            uint32_t vlen = r32(rec + off + 0x10);
            uint16_t voff = r16(rec + off + 0x14);
            if ((uint32_t)voff + vlen > len)
                return -1;
        } else if (nonres == 1) {
            uint16_t map = r16(rec + off + 0x20);
            int local = 0;
            int i;
            int unnamed = rec[off + 9] == 0;
            if (map < 0x40 || map > len)
                return -1;
            if (decode_runs(rec + off + map, rec + off + len,
                            lcns + *runs, lens + *runs, cap - *runs, &local) != 0)
                return -1;
            for (i = 0; i < local; i++) {
                int64_t start = lcns[*runs + i];
                uint64_t count = lens[*runs + i];
                if (start >= 0 && ((uint64_t)start >= clusters ||
                                   count > clusters - (uint64_t)start))
                    return -1;
            }
            if (collect && unnamed && type == 0x80)
                *runs += local;
            else if (collect && type == 0x80)
                *runs += local;
        } else {
            return -1;
        }
        off += len;
    }
    return 0;
}

static int read_mft_record(const uint8_t *img, size_t n, uint64_t vol,
                           const int64_t *lcns, const uint64_t *lens, int runs,
                           uint32_t cluster, uint32_t rec_bytes, uint64_t index,
                           uint8_t *out)
{
    uint64_t skip = (uint64_t)index * rec_bytes;
    uint32_t filled = 0;
    int i;
    memset(out, 0, rec_bytes);
    for (i = 0; i < runs && filled < rec_bytes; i++) {
        uint64_t bytes = lens[i] * cluster;
        if (skip >= bytes) {
            skip -= bytes;
            continue;
        }
        if (lcns[i] < 0)
            return -1;
        while (filled < rec_bytes && skip < bytes) {
            uint64_t pos = vol + (uint64_t)lcns[i] * cluster + skip;
            uint32_t take = rec_bytes - filled;
            if (take > bytes - skip)
                take = (uint32_t)(bytes - skip);
            if (pos + take > n)
                return -1;
            memcpy(out + filled, img + pos, take);
            filled += take;
            skip += take;
        }
        skip = 0;
    }
    return filled == rec_bytes ? 0 : -1;
}

static int check_image(const uint8_t *img, size_t n, int quiet_ok)
{
    uint64_t vol = 0;
    uint32_t bps, spc, cluster, rec_bytes;
    uint64_t sectors, clusters, mft_lcn, mirr_lcn;
    int8_t cpr;
    uint8_t *rec;
    uint8_t *raw0;
    int64_t mft_l[MAX_RUNS], alloc_l[MAX_RUNS];
    uint64_t mft_n[MAX_RUNS], alloc_n[MAX_RUNS];
    int mft_runs = 0;
    int alloc_runs = 0;
    int i;
    int rc = -1;

    if (n < 512)
        return fail("image shorter than a sector");
    if (memcmp(img + 3, "NTFS    ", 8) != 0) {
        int part;
        if (img[0x1FE] != 0x55 || img[0x1FF] != 0xAA)
            return fail("no NTFS boot sector and no MBR");
        for (part = 0; part < 4; part++) {
            const uint8_t *ent = img + 0x1BE + part * 16;
            uint32_t start;
            if (ent[4] != 0x07)
                continue;
            start = r32(ent + 8);
            vol = (uint64_t)start * 512;
            break;
        }
        if (vol == 0)
            return fail("MBR has no type 0x07 partition");
    }
    if (vol + 512 > n)
        return fail("partition starts past the image");
    if (memcmp(img + vol + 3, "NTFS    ", 8) != 0)
        return fail("OEM id is not NTFS");
    if (img[vol + 0x1FE] != 0x55 || img[vol + 0x1FF] != 0xAA)
        return fail("boot signature");
    bps = r16(img + vol + 0x0B);
    spc = img[vol + 0x0D];
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0 || spc == 0 || (spc & (spc - 1)) != 0)
        return fail("cluster size");
    cluster = bps * spc;
    if (cluster > 65536)
        return fail("cluster size");
    sectors = r64(img + vol + 0x28);
    if (sectors == 0 || vol + sectors * bps > n)
        return fail("sector count past the image");
    clusters = sectors / spc;
    mft_lcn = r64(img + vol + 0x30);
    mirr_lcn = r64(img + vol + 0x38);
    if (mft_lcn >= clusters || mirr_lcn >= clusters)
        return fail("MFT LCN past the volume");
    cpr = (int8_t)img[vol + 0x40];
    if (cpr > 0)
        rec_bytes = (uint32_t)cpr * cluster;
    else if (cpr < 0)
        rec_bytes = 1u << (uint32_t)(-cpr);
    else
        return fail("clusters per file record");
    if (rec_bytes < 512 || rec_bytes > 4096 || (rec_bytes % bps) != 0)
        return fail("file record size");
    if (vol + mft_lcn * cluster + rec_bytes > n)
        return fail("MFT past the image");

    rec = (uint8_t *)malloc(rec_bytes);
    raw0 = (uint8_t *)malloc(rec_bytes);
    if (rec == NULL || raw0 == NULL)
        goto out;
    memcpy(raw0, img + vol + mft_lcn * cluster, rec_bytes);
    memcpy(rec, raw0, rec_bytes);
    if (walk_record(rec, rec_bytes, bps, clusters, 1, mft_l, mft_n, &mft_runs, MAX_RUNS) != 0)
        return fail("file record");
    if (mft_runs == 0) {
        /* Resident $MFT, or a tiny synthetic image: the boot LCN is the record. */
        mft_l[0] = (int64_t)mft_lcn;
        mft_n[0] = (rec_bytes + cluster - 1) / cluster;
        mft_runs = 1;
    }
    if (vol + mirr_lcn * cluster + rec_bytes <= n &&
        memcmp(img + vol + mirr_lcn * cluster, raw0, rec_bytes) != 0)
        return fail("mft mirror disagrees");

    for (i = 0; i < 64; i++) {
        int before;
        if (read_mft_record(img, n, vol, mft_l, mft_n, mft_runs, cluster, rec_bytes, (uint64_t)i, rec) != 0)
            break;
        if (memcmp(rec, "FILE", 4) != 0)
            continue;
        if ((r16(rec + 0x16) & 1) == 0)
            continue;
        before = alloc_runs;
        if (walk_record(rec, rec_bytes, bps, clusters, 1, alloc_l, alloc_n, &alloc_runs, MAX_RUNS) != 0)
            return fail("file record");
        (void)before;
    }

    if (read_mft_record(img, n, vol, mft_l, mft_n, mft_runs, cluster, rec_bytes, 6, rec) == 0 &&
        memcmp(rec, "FILE", 4) == 0) {
        int64_t bl[MAX_RUNS];
        uint64_t bn[MAX_RUNS];
        int br = 0;
        uint8_t *bitmap;
        uint64_t need = (clusters + 7) / 8;
        uint64_t got = 0;
        if (walk_record(rec, rec_bytes, bps, clusters, 1, bl, bn, &br, MAX_RUNS) != 0)
            return fail("bitmap record");
        bitmap = (uint8_t *)calloc(1, (size_t)need + 1);
        if (bitmap == NULL)
            goto out;
        for (i = 0; i < br && got < need; i++) {
            uint64_t c;
            if (bl[i] < 0)
                continue;
            for (c = 0; c < bn[i] && got < need; c++) {
                uint64_t pos = vol + ((uint64_t)bl[i] + c) * cluster;
                uint32_t take = cluster;
                if (got + take > need)
                    take = (uint32_t)(need - got);
                if (pos + take > n) {
                    free(bitmap);
                    return fail("bitmap run past the image");
                }
                memcpy(bitmap + got, img + pos, take);
                got += take;
            }
        }
        for (i = 0; i < alloc_runs; i++) {
            uint64_t c;
            if (alloc_l[i] < 0)
                continue;
            for (c = 0; c < alloc_n[i]; c++) {
                uint64_t bit = (uint64_t)alloc_l[i] + c;
                if (bit / 8 >= got) {
                    free(bitmap);
                    return fail("allocated cluster past the bitmap");
                }
                if (((bitmap[bit / 8] >> (bit % 8)) & 1) == 0) {
                    free(bitmap);
                    return fail("allocated cluster is free in the bitmap");
                }
            }
        }
        free(bitmap);
    }

    if (!quiet_ok)
        printf("CHECK ok\n");
    rc = 0;
out:
    free(rec);
    free(raw0);
    return rc;
}

static int self_test(void)
{
    uint8_t *img;
    g_quiet = 1;
    img = (uint8_t *)calloc(1, 64 * 1024);
    uint8_t *rec = (uint8_t *)calloc(1, 1024);
    uint32_t vol = 8 * 512;
    int rc = 0;

    if (img == NULL || rec == NULL)
        return 1;
    img[0x1FE] = 0x55;
    img[0x1FF] = 0xAA;
    img[0x1BE + 4] = 0x07;
    img[0x1BE + 8] = 8;
    memcpy(img + vol + 3, "NTFS    ", 8);
    img[vol + 0x0B] = 0x00;
    img[vol + 0x0C] = 0x02;
    img[vol + 0x0D] = 1;
    img[vol + 0x28] = 120;
    img[vol + 0x30] = 16;
    img[vol + 0x38] = 32;
    img[vol + 0x40] = (uint8_t)(int8_t)-10;
    img[vol + 0x1FE] = 0x55;
    img[vol + 0x1FF] = 0xAA;

    memcpy(rec, "FILE", 4);
    rec[4] = 0x30;
    rec[6] = 3;
    rec[0x14] = 0x38;
    rec[0x16] = 1;
    rec[0x18] = 0x68;
    rec[0x1C] = 0x00;
    rec[0x1D] = 0x04;
    rec[0x30] = 0x01;
    rec[510] = 0x01;
    rec[1022] = 0x01;
    rec[0x38] = 0x80;
    rec[0x3C] = 0x28;
    rec[0x48] = 4;
    rec[0x4C] = 0x18;
    rec[0x60] = 0xff;
    rec[0x61] = 0xff;
    rec[0x62] = 0xff;
    rec[0x63] = 0xff;
    memcpy(img + vol + 16 * 512, rec, 1024);
    memcpy(img + vol + 32 * 512, rec, 1024);
    if (check_image(img, 64 * 1024, 1) != 0)
        rc = 1;

    memset(img, 0, 512);
    if (check_image(img, 512, 1) == 0)
        rc = 1;

    free(rec);
    free(img);
    return rc;
}

int main(int argc, char **argv)
{
    FILE *fp;
    uint8_t *img;
    long len;
    int rc;

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    if (argc != 2) {
        fprintf(stderr, "usage: ntfs_check <image> | --self-test\n");
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        fail("cannot open image");
        return 1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return fail("seek");
    }
    len = ftell(fp);
    if (len < 512 || len > 512L * 1024L * 1024L) {
        fclose(fp);
        return fail("image size");
    }
    rewind(fp);
    img = (uint8_t *)malloc((size_t)len);
    if (img == NULL || fread(img, 1, (size_t)len, fp) != (size_t)len) {
        free(img);
        fclose(fp);
        return fail("read");
    }
    fclose(fp);
    rc = check_image(img, (size_t)len, 0);
    free(img);
    return rc == 0 ? 0 : 1;
}
