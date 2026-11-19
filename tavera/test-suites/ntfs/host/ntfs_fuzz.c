/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        tavera/test-suites/ntfs/host/ntfs_fuzz.c
 * PURPOSE:     Copy a seed NTFS image and change one on-disk field.
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
 */

/*
 * Copy a seed NTFS image and change one on-disk field. The guest mounts
 * the copy. This program stays on the host.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *find_ntfs(uint8_t *b, size_t n, size_t *off)
{
    size_t i;
    if (n >= 11 && memcmp(b + 3, "NTFS    ", 8) == 0) {
        *off = 0;
        return b;
    }
    if (n < 512)
        return NULL;
    for (i = 0; i < 4; i++) {
        uint8_t *ent = b + 0x1BE + i * 16;
        uint32_t start;
        size_t vol;
        if (ent[4] != 0x07)
            continue;
        start = (uint32_t)ent[8] | ((uint32_t)ent[9] << 8) |
                ((uint32_t)ent[10] << 16) | ((uint32_t)ent[11] << 24);
        vol = (size_t)start * 512;
        if (vol + 512 <= n && memcmp(b + vol + 3, "NTFS    ", 8) == 0) {
            *off = vol;
            return b + vol;
        }
    }
    return NULL;
}

static uint8_t *find_file(uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i + 4 <= n; i += 512) {
        if (memcmp(b + i, "FILE", 4) == 0)
            return b + i;
    }
    return NULL;
}

static void mutate(uint8_t *b, size_t n, int index)
{
    size_t off = 0;
    uint8_t *boot = find_ntfs(b, n, &off);
    uint8_t *rec = find_file(b, n);
    int kind = index % 8;

    if (boot == NULL)
        boot = b;
    switch (kind) {
    case 0:
        boot[3 + (index / 8) % 8] ^= 0xff;
        break;
    case 1:
        if (off + 0x1ff < n)
            boot[0x1FE] ^= 0xff;
        break;
    case 2:
        boot[0x0B] = 0x01;
        boot[0x0C] = 0x00;
        break;
    case 3:
        if (rec != NULL && rec + 8 < b + n)
            rec[4] ^= 0xff;
        else if (n > 64)
            b[64] ^= 0xff;
        break;
    case 4:
        if (rec != NULL && rec + 0x40 < b + n) {
            rec[0x3C] = 0xf8;
            rec[0x3D] = 0xff;
            rec[0x3E] = 0xff;
            rec[0x3F] = 0x00;
        }
        break;
    case 5:
        if (rec != NULL && rec + 0x50 < b + n)
            rec[0x48] ^= 0xff;
        break;
    case 6:
        if (rec != NULL && rec + 0x60 < b + n)
            rec[0x58] = 0xff;
        break;
    default:
        boot[0x40] ^= 0xff;
        if (off + 0x50 < n)
            boot[0x48] ^= 0x01;
        break;
    }
}

static int self_test(void)
{
    uint8_t buf[4096];
    uint8_t copy[4096];
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 3, "NTFS    ", 8);
    memcpy(buf + 512, "FILE", 4);
    memcpy(copy, buf, sizeof(buf));
    mutate(copy, sizeof(copy), 0);
    if (memcmp(buf, copy, sizeof(buf)) == 0)
        return 1;
    mutate(copy, sizeof(copy), 3);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *in;
    FILE *out;
    uint8_t *buf;
    long len;
    int index;

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    if (argc != 4) {
        fprintf(stderr, "usage: ntfs_fuzz <seed> <dest> <index>\n");
        return 2;
    }
    index = atoi(argv[3]);
    in = fopen(argv[1], "rb");
    if (in == NULL)
        return 1;
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return 1;
    }
    len = ftell(in);
    if (len < 512 || len > 512L * 1024L * 1024L) {
        fclose(in);
        return 1;
    }
    rewind(in);
    buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, in) != (size_t)len) {
        free(buf);
        fclose(in);
        return 1;
    }
    fclose(in);
    mutate(buf, (size_t)len, index);
    out = fopen(argv[2], "wb");
    if (out == NULL || fwrite(buf, 1, (size_t)len, out) != (size_t)len) {
        if (out)
            fclose(out);
        free(buf);
        return 1;
    }
    fclose(out);
    free(buf);
    return 0;
}
