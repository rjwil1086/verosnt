/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/test-suites/ntfs/host/ntfs_bytes.c
 * PURPOSE:     Print comparable NTFS file-record fields
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
 */

/*
 * Print one line per user file on a raw NTFS image. Timestamps, cluster
 * numbers, and security ids are left out so a Windows image and a VerOS
 * image of the same creates can be compared field by field.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RUNS 64
#define MAX_REC 2048
#define MAX_NAMES 8

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

static int decode_hole(const uint8_t *p, const uint8_t *end)
{
    while (p < end && *p != 0) {
        uint8_t head = *p++;
        int llen = head & 0x0f;
        int olen = head >> 4;
        if (llen == 0 || llen > 8 || olen > 8 || p + llen + olen > end)
            return -1;
        p += llen;
        if (olen == 0)
            return 1;
        p += olen;
    }
    return 0;
}

static void ascii_name(char *dst, int cap, const uint8_t *src, int chars)
{
    int i;
    int n = 0;
    for (i = 0; i < chars && n + 1 < cap; i++) {
        uint16_t ch = r16(src + i * 2);
        dst[n++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    dst[n] = 0;
}

static const char *name_type(int type)
{
    if (type == 0)
        return "POSIX";
    if (type == 1)
        return "WIN32";
    if (type == 2)
        return "DOS";
    if (type == 3)
        return "WIN32DOS";
    return "OTHER";
}

struct fname {
    int type;
    uint64_t parent;
    uint16_t pseq;
    char text[200];
};

struct recinfo {
    int used;
    uint16_t seq;
    uint64_t parent;
    uint16_t pseq;
    char leaf[200];
    char names[700];
    char rest[240];
};

static int read_record(const uint8_t *img, size_t n, uint64_t vol,
                       const int64_t *lcns, const uint64_t *lens, int runs,
                       uint32_t cluster, uint32_t rec_bytes, uint64_t index,
                       uint8_t *out)
{
    uint64_t skip = index * rec_bytes;
    uint32_t filled = 0;
    int i;

    memset(out, 0, rec_bytes);
    for (i = 0; i < runs && filled < rec_bytes; i++) {
        uint64_t bytes = lens[i] * (uint64_t)cluster;
        if (bytes == 0)
            return -1;
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

static int collect_mft_runs(uint8_t *rec, uint32_t rec_bytes, uint32_t sector,
                            int64_t *lcns, uint64_t *lens, int cap, int *runs)
{
    uint32_t off;
    uint32_t used;

    *runs = 0;
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
        if (type == 0xffffffff)
            return *runs > 0 ? 0 : -1;
        if (len < 24 || off + len > used)
            return -1;
        if (rec[off + 8] == 1 && type == 0x80 && rec[off + 9] == 0) {
            uint16_t map = r16(rec + off + 0x20);
            const uint8_t *p;
            const uint8_t *end;
            int64_t lcn = 0;
            if (map >= len)
                return -1;
            p = rec + off + map;
            end = rec + off + len;
            while (p < end && *p != 0 && *runs < cap) {
                uint8_t head = *p++;
                int llen = head & 0x0f;
                int olen = head >> 4;
                uint64_t runlen = 0;
                int i;
                if (llen == 0 || llen > 8 || olen > 8 || p + llen + olen > end)
                    return -1;
                for (i = 0; i < llen; i++)
                    runlen |= (uint64_t)(*p++) << (8 * i);
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
                lcns[*runs] = lcn;
                lens[*runs] = runlen;
                (*runs)++;
            }
            return *runs > 0 ? 0 : -1;
        }
        off += len;
    }
    return -1;
}

static void describe(uint8_t *rec, uint32_t rec_bytes, struct recinfo *info)
{
    uint32_t off = r16(rec + 0x14);
    uint32_t used = r32(rec + 0x18);
    struct fname names[MAX_NAMES];
    int nnames = 0;
    int si = 0;
    int list = 0;
    int obj = 0;
    int efs = 0;
    int hole = 0;
    int data = -1;
    int cu = 0;
    unsigned reparse = 0;
    unsigned fileattr = 0;
    int i;
    int pick = -1;
    char piece[240];
    int pos = 0;

    memset(names, 0, sizeof(names));
    while (off + 8 <= used && off + 8 <= rec_bytes) {
        uint32_t type = r32(rec + off);
        uint32_t len = r32(rec + off + 4);
        uint8_t nonres;
        uint8_t nlen;
        if (type == 0xffffffff)
            break;
        if (len < 24 || off + len > used)
            break;
        nonres = rec[off + 8];
        nlen = rec[off + 9];
        if (type == 0x10 && nonres == 0) {
            uint32_t vlen = r32(rec + off + 0x10);
            uint16_t voff = r16(rec + off + 0x14);
            si = (int)vlen;
            if ((uint32_t)voff + 36 <= len)
                fileattr = r32(rec + off + voff + 0x20);
        } else if (type == 0x20) {
            list = 1;
        } else if (type == 0x40) {
            obj = 1;
        } else if (type == 0xC0 && nonres == 0) {
            uint16_t voff = r16(rec + off + 0x14);
            if ((uint32_t)voff + 4 <= len)
                reparse = r32(rec + off + voff);
        } else if (type == 0x100 && nlen == 4) {
            uint16_t noff = r16(rec + off + 10);
            if ((uint32_t)noff + 8 <= len &&
                r16(rec + off + noff) == '$' &&
                r16(rec + off + noff + 2) == 'E' &&
                r16(rec + off + noff + 4) == 'F' &&
                r16(rec + off + noff + 6) == 'S')
                efs = 1;
        } else if (type == 0x30 && nonres == 0 && nnames < MAX_NAMES) {
            uint32_t vlen = r32(rec + off + 0x10);
            uint16_t voff = r16(rec + off + 0x14);
            const uint8_t *val = rec + off + voff;
            if (vlen >= 0x42 && (uint32_t)voff + vlen <= len) {
                int chars = val[0x40];
                names[nnames].type = val[0x41];
                names[nnames].parent = r64(val) & 0x0000ffffffffffffULL;
                names[nnames].pseq = (uint16_t)(r64(val) >> 48);
                if ((uint32_t)chars * 2 + 0x42 <= vlen)
                    ascii_name(names[nnames].text, 200, val + 0x42, chars);
                nnames++;
            }
        } else if (type == 0x80 && nlen == 0) {
            data = nonres ? 1 : 0;
            if (nonres == 1) {
                uint16_t map = r16(rec + off + 0x20);
                int saw;
                cu = r16(rec + off + 0x22);
                if (map < len) {
                    saw = decode_hole(rec + off + map, rec + off + len);
                    if (saw > 0)
                        hole = 1;
                }
            }
        }
        off += len;
    }

    for (i = 0; i < nnames; i++) {
        int better = 0;
        if (pick < 0)
            better = 1;
        else if (names[i].type == 3 || names[i].type == 1) {
            if (names[pick].type != 3 && names[pick].type != 1)
                better = 1;
        }
        if (better)
            pick = i;
    }
    if (pick < 0)
        return;
    if (names[pick].text[0] == '$')
        return;
    info->used = 1;
    info->seq = r16(rec + 0x10);
    info->parent = names[pick].parent;
    info->pseq = names[pick].pseq;
    memcpy(info->leaf, names[pick].text, sizeof(info->leaf));
    info->names[0] = 0;
    for (i = 0; i < nnames; i++) {
        int j;
        for (j = i + 1; j < nnames; j++) {
            int swap = 0;
            if (names[j].type < names[i].type)
                swap = 1;
            else if (names[j].type == names[i].type &&
                     strcmp(names[j].text, names[i].text) < 0)
                swap = 1;
            if (swap) {
                struct fname tmp = names[i];
                names[i] = names[j];
                names[j] = tmp;
            }
        }
    }
    for (i = 0; i < nnames; i++) {
        int wrote = sprintf(piece, "%s%s:%llu.%u:%s",
                            i ? ";" : "",
                            name_type(names[i].type),
                            (unsigned long long)names[i].parent,
                            names[i].pseq,
                            names[i].text);
        if (wrote < 0 || pos + wrote >= (int)sizeof(info->names))
            break;
        memcpy(info->names + pos, piece, (size_t)wrote + 1);
        pos += wrote;
    }
    sprintf(info->rest,
            "si=%d|dir=%d|comp=%d|enc=%d|sparse=%d|reparse=%u|obj=%d|efs=%d|list=%d|data=%s|cu=%d|hole=%d|names=%s",
            si,
            (r16(rec + 0x16) & 2) ? 1 : 0,
            (fileattr & 0x800) ? 1 : 0,
            (fileattr & 0x4000) ? 1 : 0,
            (fileattr & 0x200) ? 1 : 0,
            reparse,
            obj,
            efs,
            list,
            data < 0 ? "none" : (data ? "NR" : "R"),
            cu,
            hole,
            info->names);
}

int main(int argc, char **argv)
{
    FILE *fp;
    uint8_t *img;
    size_t n;
    size_t got;
    uint64_t vol = 0;
    uint32_t bps, spc, cluster, rec_bytes;
    uint64_t sectors, clusters, mft_lcn;
    int8_t cpr;
    uint8_t *rec;
    int64_t lcns[MAX_RUNS];
    uint64_t lens[MAX_RUNS];
    int runs = 0;
    struct recinfo *rows;
    int i;
    int major = 0;
    int minor = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: ntfs_bytes image\n");
        return 2;
    }
    fprintf(stderr, "reading %s\n", argv[1]);
    fflush(stderr);
    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 1;
    }
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
    if (n < 512 || memcmp(img + 3, "NTFS    ", 8) != 0) {
        int part;
        if (n < 512 || img[0x1FE] != 0x55 || img[0x1FF] != 0xAA) {
            fprintf(stderr, "not an NTFS image\n");
            free(img);
            return 1;
        }
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
    bps = r16(img + vol + 0x0B);
    spc = img[vol + 0x0D];
    cluster = bps * spc;
    sectors = r64(img + vol + 0x28);
    clusters = spc ? sectors / spc : 0;
    mft_lcn = r64(img + vol + 0x30);
    cpr = (int8_t)img[vol + 0x40];
    if (cpr > 0)
        rec_bytes = (uint32_t)cpr * cluster;
    else if (cpr < 0)
        rec_bytes = 1u << (uint32_t)(-cpr);
    else
        rec_bytes = 0;
    if (bps < 512 || spc == 0 || rec_bytes < 512 || rec_bytes > 4096 ||
        mft_lcn >= clusters || vol + mft_lcn * cluster + rec_bytes > n) {
        fprintf(stderr, "boot sector fields\n");
        free(img);
        return 1;
    }
    rec = (uint8_t *)malloc(rec_bytes);
    rows = (struct recinfo *)calloc(MAX_REC, sizeof(*rows));
    if (rec == NULL || rows == NULL) {
        free(img);
        free(rec);
        free(rows);
        return 1;
    }
    memcpy(rec, img + vol + mft_lcn * cluster, rec_bytes);
    if (collect_mft_runs(rec, rec_bytes, bps, lcns, lens, MAX_RUNS, &runs) != 0) {
        fprintf(stderr, "MFT runs\n");
        free(img);
        free(rec);
        free(rows);
        return 1;
    }
    for (i = 0; i < MAX_REC; i++) {
        uint32_t aoff;
        uint32_t used;
        if (read_record(img, n, vol, lcns, lens, runs, cluster, rec_bytes, (uint64_t)i, rec) != 0)
            break;
        if (memcmp(rec, "FILE", 4) != 0)
            continue;
        if (apply_fixup(rec, rec_bytes, bps) != 0)
            continue;
        if ((r16(rec + 0x16) & 1) == 0)
            continue;
        if (i == 3) {
            aoff = r16(rec + 0x14);
            used = r32(rec + 0x18);
            while (aoff + 8 <= used && aoff + 8 <= rec_bytes) {
                uint32_t type = r32(rec + aoff);
                uint32_t len = r32(rec + aoff + 4);
                if (type == 0xffffffff || len < 24 || aoff + len > used)
                    break;
                if (type == 0x70 && rec[aoff + 8] == 0) {
                    uint16_t voff = r16(rec + aoff + 0x14);
                    if ((uint32_t)voff + 10 <= len) {
                        major = rec[aoff + voff + 8];
                        minor = rec[aoff + voff + 9];
                    }
                }
                aoff += len;
            }
        }
        if (i >= 16 && i < MAX_REC)
            describe(rec, rec_bytes, &rows[i]);
    }
    printf("VOL ver=%d.%d cluster=%u\n", major, minor, cluster);
    for (i = 16; i < MAX_REC; i++) {
        char path[512];
        int guard = 0;
        uint64_t cur;
        if (!rows[i].used)
            continue;
        path[0] = 0;
        cur = (uint64_t)i;
        while (cur != 5 && cur < MAX_REC && rows[cur].used && guard < 12) {
            char next[512];
            if (path[0] == 0)
                sprintf(next, "%s", rows[cur].leaf);
            else
                sprintf(next, "%s\\%s", rows[cur].leaf, path);
            if (strlen(next) >= sizeof(path))
                break;
            memcpy(path, next, strlen(next) + 1);
            if (rows[cur].parent == cur)
                break;
            cur = rows[cur].parent;
            guard++;
        }
        if (path[0] == 0)
            continue;
        printf("FILE %s|%s\n", path, rows[i].rest);
    }
    free(img);
    free(rec);
    free(rows);
    return 0;
}
