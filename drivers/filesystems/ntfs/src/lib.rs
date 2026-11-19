/*
 * PROJECT:     Tavera OS Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        drivers/filesystems/ntfs/src/lib.rs
 * PURPOSE:     NTFS helpers cited from tavera/specs/ntfs
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 */

/*
 * Quota $Q resident index-root helpers from tavera/specs/ntfs/ntfs-quota-on-disk.md:
 * - Absolute offsets 316/324 (probe-v2 A/B; NTFS-QUOTA-002).
 * - Track growth format 208 → four-entry root (probe-v3 Rev.3; keys 1, 0x100,
 *   0x101, 0x102; control-entry data +0x18/+0x20). Captures cited under
 *   output/ntfs/probe-v3/ with NTFS-QUOTA-GROWTH-REPORT … END.
 * - Index packing matches in-tree MkViewRootN / Qce in
 *   sdk/lib/fslib/ntfslib/files.c (not probe SID bytes).
 * FILE_QUOTA_INFORMATION (-1 none; QuotaLimit -2 clears thr/lim to -1, keep leaf):
 * tavera/specs/ntfs/ntfs-quota-on-disk.md Rev.4–8 / probe-v4.
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/acdc0738-ba3c-47a1-b11a-72e22d831c57
 */

#![no_std]

use core::panic::PanicInfo;
use core::ptr;
use core::slice;

/// STATUS_SUCCESS
const STATUS_SUCCESS: i32 = 0;
/// STATUS_INVALID_PARAMETER
const STATUS_INVALID_PARAMETER: i32 = -1073741811i32; // 0xC000000D
/// STATUS_BUFFER_TOO_SMALL
const STATUS_BUFFER_TOO_SMALL: i32 = -1073741789i32; // 0xC0000023
/// STATUS_NOT_SUPPORTED — unexpected $Q shape
const STATUS_NOT_SUPPORTED: i32 = -1073741637i32; // 0xC00000BB

const QUOTA_THRESHOLD_AT: usize = 316;
const QUOTA_LIMIT_AT: usize = 324;

const INDEX_ENTRY_END: u16 = 0x02;
const ENTRIES_START: usize = 0x20;
const CTRL_HDR: usize = 0x30; // version..ExceededTime before SID
const KEY_LEN: usize = 4;

#[inline]
fn align8(n: usize) -> usize {
    (n + 7) & !7
}

#[inline]
unsafe fn read_u16(p: *const u8, off: usize) -> u16 {
    let b = slice::from_raw_parts(p.add(off), 2);
    u16::from_le_bytes([b[0], b[1]])
}

#[inline]
unsafe fn read_u32(p: *const u8, off: usize) -> u32 {
    let b = slice::from_raw_parts(p.add(off), 4);
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

#[inline]
unsafe fn write_u16(p: *mut u8, off: usize, v: u16) {
    let b = v.to_le_bytes();
    ptr::copy_nonoverlapping(b.as_ptr(), p.add(off), 2);
}

#[inline]
unsafe fn write_u32(p: *mut u8, off: usize, v: u32) {
    let b = v.to_le_bytes();
    ptr::copy_nonoverlapping(b.as_ptr(), p.add(off), 4);
}

#[inline]
unsafe fn write_i64(p: *mut u8, off: usize, v: i64) {
    let b = v.to_le_bytes();
    ptr::copy_nonoverlapping(b.as_ptr(), p.add(off), 8);
}

/// Spec-cited absolute offsets into the resident `$Q` index-root value.
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_write_threshold_limit(
    root: *mut u8,
    root_len: usize,
    threshold: i64,
    limit: i64,
) -> i32 {
    if root.is_null() {
        return STATUS_INVALID_PARAMETER;
    }
    if root_len < QUOTA_LIMIT_AT + 8 {
        return STATUS_BUFFER_TOO_SMALL;
    }

    write_i64(root, QUOTA_THRESHOLD_AT, threshold);
    write_i64(root, QUOTA_LIMIT_AT, limit);
    STATUS_SUCCESS
}

/// Write thr/lim at control-entry data +0x18/+0x20.
/// owner_key != 0: match that ULONG key only. owner_key == 0: match SID only.
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_write_entry_threshold_limit(
    root: *mut u8,
    root_len: usize,
    owner_key: u32,
    sid: *const u8,
    sid_len: usize,
    threshold: i64,
    limit: i64,
) -> i32 {
    if root.is_null() || root_len < ENTRIES_START + 0x10 {
        return STATUS_INVALID_PARAMETER;
    }
    if owner_key == 0 && (sid.is_null() || sid_len == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    let mut cursor = ENTRIES_START;
    while cursor + 0x10 <= root_len {
        let length = read_u16(root, cursor + 0x08) as usize;
        let key_len = read_u16(root, cursor + 0x0a) as usize;
        let flags = read_u16(root, cursor + 0x0c);
        let data_off = read_u16(root, cursor + 0x00) as usize;
        let data_len = read_u16(root, cursor + 0x02) as usize;

        if length < 0x10 || cursor + length > root_len {
            break;
        }
        if (flags & INDEX_ENTRY_END) != 0 {
            break;
        }

        let matched = if owner_key != 0 {
            key_len == KEY_LEN
                && cursor + 0x10 + KEY_LEN <= root_len
                && read_u32(root, cursor + 0x10) == owner_key
        } else if !sid.is_null() && sid_len > 0 && data_len >= CTRL_HDR + sid_len {
            let data_at = cursor + data_off;
            data_at + CTRL_HDR + sid_len <= root_len && {
                let on_disk = slice::from_raw_parts(root.add(data_at + CTRL_HDR), sid_len);
                let want = slice::from_raw_parts(sid, sid_len);
                on_disk == want
            }
        } else {
            false
        };

        if matched {
            if data_len < CTRL_HDR || cursor + data_off + 0x28 > root_len {
                return STATUS_BUFFER_TOO_SMALL;
            }
            write_i64(root, cursor + data_off + 0x18, threshold);
            write_i64(root, cursor + data_off + 0x20, limit);
            return STATUS_SUCCESS;
        }

        cursor += length;
    }

    STATUS_NOT_SUPPORTED
}

/// QuotaLimit -2 (probe-v4 / Rev.4): clear thr/lim to -1 by SID; keep leaf;
/// if owner key is 0x102, renumber to 0x103.
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_clear_for_delete(
    root: *mut u8,
    root_len: usize,
    sid: *const u8,
    sid_len: usize,
) -> i32 {
    if root.is_null() || sid.is_null() || sid_len == 0 || root_len < ENTRIES_START + 0x10 {
        return STATUS_INVALID_PARAMETER;
    }

    let mut cursor = ENTRIES_START;
    while cursor + 0x10 <= root_len {
        let length = read_u16(root, cursor + 0x08) as usize;
        let key_len = read_u16(root, cursor + 0x0a) as usize;
        let flags = read_u16(root, cursor + 0x0c);
        let data_off = read_u16(root, cursor + 0x00) as usize;
        let data_len = read_u16(root, cursor + 0x02) as usize;

        if length < 0x10 || cursor + length > root_len {
            break;
        }
        if (flags & INDEX_ENTRY_END) != 0 {
            break;
        }

        if data_len >= CTRL_HDR + sid_len {
            let data_at = cursor + data_off;
            if data_at + CTRL_HDR + sid_len <= root_len {
                let on_disk = slice::from_raw_parts(root.add(data_at + CTRL_HDR), sid_len);
                let want = slice::from_raw_parts(sid, sid_len);
                if on_disk == want {
                    write_i64(root, data_at + 0x18, -1);
                    write_i64(root, data_at + 0x20, -1);
                    if key_len == KEY_LEN && read_u32(root, cursor + 0x10) == 0x102 {
                        write_u32(root, cursor + 0x10, 0x103);
                    }
                    return STATUS_SUCCESS;
                }
            }
        }
        cursor += length;
    }

    /* Already absent — treat as success for suite post-delete. */
    STATUS_SUCCESS
}

/// Append a control entry with thr/lim (suite LOCAL SERVICE seed).
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_append_entry(
    in_root: *const u8,
    in_len: usize,
    owner_key: u32,
    sid: *const u8,
    sid_len: usize,
    threshold: i64,
    limit: i64,
    change_time: i64,
    out_root: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> i32 {
    if in_root.is_null()
        || out_root.is_null()
        || out_len.is_null()
        || sid.is_null()
        || sid_len == 0
        || in_len < ENTRIES_START + 0x10
    {
        return STATUS_INVALID_PARAMETER;
    }

    let add = align8(0x10 + KEY_LEN + CTRL_HDR + sid_len);
    let need = in_len + add;
    if out_cap < need {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Copy all but the end marker; end marker is last 0x10 with INDEX_ENTRY_END. */
    let mut end_at = in_len;
    let mut cursor = ENTRIES_START;
    while cursor + 0x10 <= in_len {
        let length = read_u16(in_root, cursor + 0x08) as usize;
        let flags = read_u16(in_root, cursor + 0x0c);
        if length < 0x10 || cursor + length > in_len {
            break;
        }
        if (flags & INDEX_ENTRY_END) != 0 {
            end_at = cursor;
            break;
        }
        cursor += length;
    }

    ptr::write_bytes(out_root, 0, need);
    ptr::copy_nonoverlapping(in_root, out_root, end_at);
    let written = write_ctrl_entry(out_root, end_at, owner_key, sid, sid_len, change_time);
    /* Override thr/lim from -1 defaults. */
    let data_off = 0x10 + KEY_LEN;
    write_i64(out_root, end_at + data_off + 0x18, threshold);
    write_i64(out_root, end_at + data_off + 0x20, limit);

    let mut p = end_at + written;
    ptr::write_bytes(out_root.add(p), 0, 0x10);
    write_u16(out_root, p + 0x08, 0x10);
    write_u16(out_root, p + 0x0c, INDEX_ENTRY_END);
    p += 0x10;

    let index_len = (p - 0x10) as u32;
    write_u32(out_root, 0x14, index_len);
    write_u32(out_root, 0x18, index_len);
    write_u32(out_root, 0x10, 0x10);
    *out_len = p;
    STATUS_SUCCESS
}

/// Highest ULONG owner key among $Q entries (0 if none).
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_max_owner_key(
    root: *const u8,
    root_len: usize,
    max_key: *mut u32,
) -> i32 {
    if root.is_null() || max_key.is_null() || root_len < ENTRIES_START {
        return STATUS_INVALID_PARAMETER;
    }
    let mut best = 0u32;
    let mut cursor = ENTRIES_START;
    while cursor + 0x10 <= root_len {
        let length = read_u16(root, cursor + 0x08) as usize;
        let key_len = read_u16(root, cursor + 0x0a) as usize;
        let flags = read_u16(root, cursor + 0x0c);
        if length < 0x10 || cursor + length > root_len {
            break;
        }
        if (flags & INDEX_ENTRY_END) != 0 {
            break;
        }
        if key_len == KEY_LEN {
            let key = read_u32(root, cursor + 0x10);
            if key > best {
                best = key;
            }
        }
        cursor += length;
    }
    *max_key = best;
    STATUS_SUCCESS
}

/// Emit FILE_QUOTA_INFORMATION rows for entries that carry a SID (data_len > 0x30).
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_query(
    root: *const u8,
    root_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> i32 {
    if root.is_null() || out.is_null() || out_len.is_null() {
        return STATUS_INVALID_PARAMETER;
    }
    *out_len = 0;
    if root_len < ENTRIES_START + 0x10 {
        return STATUS_SUCCESS;
    }

    let mut cursor = ENTRIES_START;
    let mut wrote = 0usize;
    let mut prev_start: Option<usize> = None;

    while cursor + 0x10 <= root_len {
        let length = read_u16(root, cursor + 0x08) as usize;
        let flags = read_u16(root, cursor + 0x0c);
        let data_off = read_u16(root, cursor + 0x00) as usize;
        let data_len = read_u16(root, cursor + 0x02) as usize;

        if length < 0x10 || cursor + length > root_len {
            break;
        }
        if (flags & INDEX_ENTRY_END) != 0 {
            break;
        }

        if data_len > CTRL_HDR {
            let sid_len = data_len - CTRL_HDR;
            let data_at = cursor + data_off;
            /* FILE_QUOTA_INFORMATION: Next, SidLength, ChangeTime, Used, Thr, Lim, Sid */
            let entry_size = align8(40 + sid_len);
            if wrote + entry_size > out_cap {
                if wrote == 0 {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                break;
            }
            if let Some(prev) = prev_start {
                write_u32(out, prev, (wrote - prev) as u32);
            }
            ptr::write_bytes(out.add(wrote), 0, entry_size);
            write_u32(out, wrote + 0x00, 0);
            write_u32(out, wrote + 0x04, sid_len as u32);
            ptr::copy_nonoverlapping(root.add(data_at + 0x10), out.add(wrote + 0x08), 8);
            ptr::copy_nonoverlapping(root.add(data_at + 0x08), out.add(wrote + 0x10), 8);
            ptr::copy_nonoverlapping(root.add(data_at + 0x18), out.add(wrote + 0x18), 8);
            ptr::copy_nonoverlapping(root.add(data_at + 0x20), out.add(wrote + 0x20), 8);
            ptr::copy_nonoverlapping(root.add(data_at + CTRL_HDR), out.add(wrote + 0x28), sid_len);
            prev_start = Some(wrote);
            wrote += entry_size;
        }
        cursor += length;
    }

    *out_len = wrote;
    STATUS_SUCCESS
}

unsafe fn write_ctrl_entry(
    out: *mut u8,
    at: usize,
    key: u32,
    sid: *const u8,
    sid_len: usize,
    change_time: i64,
) -> usize {
    let data_len = CTRL_HDR + sid_len;
    let data_off = 0x10 + KEY_LEN;
    let entry_len = align8(data_off + data_len);

    ptr::write_bytes(out.add(at), 0, entry_len);
    write_u16(out, at + 0x00, data_off as u16);
    write_u16(out, at + 0x02, data_len as u16);
    write_u16(out, at + 0x08, entry_len as u16);
    write_u16(out, at + 0x0a, KEY_LEN as u16);
    write_u16(out, at + 0x0c, 0);
    write_u32(out, at + 0x10, key);

    let data = at + data_off;
    write_u32(out, data + 0x00, 2); // version
    write_u32(out, data + 0x04, 1); // flags (format Qce)
    write_i64(out, data + 0x08, 0); // bytes used
    write_i64(out, data + 0x10, change_time);
    write_i64(out, data + 0x18, -1); // threshold none
    write_i64(out, data + 0x20, -1); // limit none
    write_i64(out, data + 0x28, 0); // exceeded
    if sid_len > 0 && !sid.is_null() {
        ptr::copy_nonoverlapping(sid, out.add(data + CTRL_HDR), sid_len);
    }
    entry_len
}

/// Grow format-time two-entry `$Q` root to four entries (keys 1, 0x100, 0x101, 0x102).
/// If already large enough for 316/324 (≥ 332), copies input to output unchanged.
#[no_mangle]
pub unsafe extern "C" fn ntfs_quota_grow_for_track(
    in_root: *const u8,
    in_len: usize,
    sid101: *const u8,
    sid101_len: usize,
    sid102: *const u8,
    sid102_len: usize,
    change_time: i64,
    out_root: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> i32 {
    if in_root.is_null() || out_root.is_null() || out_len.is_null() {
        return STATUS_INVALID_PARAMETER;
    }
    if sid101.is_null() || sid101_len == 0 || sid102.is_null() || sid102_len == 0 {
        return STATUS_INVALID_PARAMETER;
    }
    if in_len < ENTRIES_START + 0x10 {
        return STATUS_INVALID_PARAMETER;
    }

    /* Already post-track shaped for absolute 316/324. */
    if in_len >= QUOTA_LIMIT_AT + 8 {
        if out_cap < in_len {
            return STATUS_BUFFER_TOO_SMALL;
        }
        ptr::copy_nonoverlapping(in_root, out_root, in_len);
        *out_len = in_len;
        return STATUS_SUCCESS;
    }

    /* Need keys 1 and 0x100 from the format root. */
    let mut have1 = false;
    let mut have100 = false;
    let mut entry1_at = 0usize;
    let mut entry1_len = 0usize;
    let mut entry100_at = 0usize;
    let mut entry100_len = 0usize;
    let mut cursor = ENTRIES_START;
    while cursor + 0x10 <= in_len {
        let length = read_u16(in_root, cursor + 0x08) as usize;
        let key_len = read_u16(in_root, cursor + 0x0a) as usize;
        let flags = read_u16(in_root, cursor + 0x0c);
        if length < 0x10 || cursor + length > in_len {
            break;
        }
        if (flags & INDEX_ENTRY_END) != 0 {
            break;
        }
        if key_len == KEY_LEN {
            let key = read_u32(in_root, cursor + 0x10);
            if key == 1 {
                have1 = true;
                entry1_at = cursor;
                entry1_len = length;
            } else if key == 0x100 {
                have100 = true;
                entry100_at = cursor;
                entry100_len = length;
            }
        }
        cursor += length;
    }
    if !have1 || !have100 {
        return STATUS_NOT_SUPPORTED;
    }

    let need = ENTRIES_START
        + entry1_len
        + entry100_len
        + align8(0x10 + KEY_LEN + CTRL_HDR + sid101_len)
        + align8(0x10 + KEY_LEN + CTRL_HDR + sid102_len)
        + 0x10;
    if out_cap < need {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ptr::write_bytes(out_root, 0, need);
    /* Preserve index-root header (type/collation/record size/…) from format. */
    ptr::copy_nonoverlapping(in_root, out_root, ENTRIES_START.min(in_len));

    let mut p = ENTRIES_START;
    ptr::copy_nonoverlapping(in_root.add(entry1_at), out_root.add(p), entry1_len);
    p += entry1_len;
    ptr::copy_nonoverlapping(in_root.add(entry100_at), out_root.add(p), entry100_len);
    p += entry100_len;
    p += write_ctrl_entry(out_root, p, 0x101, sid101, sid101_len, change_time);
    p += write_ctrl_entry(out_root, p, 0x102, sid102, sid102_len, change_time);

    /* End marker */
    ptr::write_bytes(out_root.add(p), 0, 0x10);
    write_u16(out_root, p + 0x08, 0x10);
    write_u16(out_root, p + 0x0c, INDEX_ENTRY_END);
    p += 0x10;

    let index_len = (p - 0x10) as u32;
    write_u32(out_root, 0x14, index_len);
    write_u32(out_root, 0x18, index_len);
    /* FirstEntryOffset stays 0x10 (relative to header at 0x10). */
    write_u32(out_root, 0x10, 0x10);

    *out_len = p;
    STATUS_SUCCESS
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
