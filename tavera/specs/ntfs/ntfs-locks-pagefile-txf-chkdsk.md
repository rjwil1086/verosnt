<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-locks-pagefile-txf-chkdsk.md
PURPOSE:     Byte-range locks (in-tree) and deferred page-file / TxF / chkdsk ops
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS locks, page files, transactions, and chkdsk

## Header and provenance

Component: four operations named by GitHub symptom #3 (page files, byte-range locks, transactions, `NtfsChkdsk`).  
Version: 1. Author: Ryan Wilson. Date: 2026-11-18.  
Only allowed sources below were used. This spec records a **partial** close: locks are in-tree; the other three stay deferred without invented recipes.

## Sources

| Source | Date read |
|--------|-----------|
| [FsRtlProcessFileLock](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-_fsrtl_advanced_fcb_header-fsrtlprocessfilelock) | 2026-11-18 |
| [FsRtlCheckLockForReadAccess](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-_fsrtl_advanced_fcb_header-fsrtlchecklockforreadaccess) / related FsRtl lock helpers (same DDI family; used in-tree) | 2026-11-18 |
| [IRP_MJ_CREATE (FS and FS Filters)](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/ifs/irp-mj-create) — `SL_OPEN_PAGING_FILE` means the create is for a paging file | 2026-11-18 |
| [chkdsk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) | 2026-11-18 |
| [fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty) **(Rev.1)** | 2026-11-18 |
| [autochk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/autochk) **(Rev.1)** | 2026-11-18 |
| [chkntfs](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkntfs) **(Rev.1)** | 2026-11-18 |
| `drivers/filesystems/ntfs/fsctl.c` — `FSCTL_MARK_VOLUME_DIRTY` / `FSCTL_IS_VOLUME_DIRTY`; `NtfsVerifyVolume` stub **(Rev.1)** | 2026-11-18 |
| `drivers/filesystems/ntfs/lockctrl.c` — `NtfsLockControl` → `FsRtlProcessFileLock` | 2026-11-18 |
| `drivers/filesystems/ntfs/fcb.c` — `FsRtlInitializeFileLock` / `FsRtlUninitializeFileLock` | 2026-11-18 |
| `drivers/filesystems/ntfs/rw.c` — `FsRtlCheckLockForReadAccess` / `FsRtlCheckLockForWriteAccess` | 2026-11-18 |
| `drivers/filesystems/ntfs/fastio.c` — fast lock check helpers | 2026-11-18 |
| `drivers/filesystems/ntfs/cleanup.c` — `FsRtlFastUnlockAll` | 2026-11-18 |
| `drivers/filesystems/ntfs/dispatch.c` / `ntfs.c` — `IRP_MJ_LOCK_CONTROL` | 2026-11-18 |
| `drivers/filesystems/ntfs/ntfs.h` — `FILE_LOCK FileLock` on FCB | 2026-11-18 |
| `drivers/filesystems/ntfs/create.c` — commented `SL_OPEN_PAGING_FILE` / `PagingFileCreate` | 2026-11-18 |
| `drivers/filesystems/ntfs/rw.c` remarks — page files / transactions not implemented | 2026-11-18 |

## Purpose and scope

Split #3 into four tracked outcomes so the implementer can close the lock slice without inventing page-file, TxF, or in-driver chkdsk behavior.

| Slice | Status in this spec |
|-------|---------------------|
| Byte-range locks | **In-tree / acceptance stated** |
| Page files | **Deferred** — flag seen in create path but unused; no full paging-file I/O recipe |
| Transactions (TxF) | **Deferred** — no allowed-source recipe; wait for a guest log |
| `NtfsChkdsk` | **Deferred** — no in-tree chkdsk entry point; host `chkdsk` is a separate tool ([Learn](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk)) |
| `NtfsChkdsk` **(Rev.1)** | **Closed / out of scope** — issue body name was mislabeled; no in-driver repair export; see Rev.1 product surface below |

## Interfaces

### Byte-range locks (implemented)

- Major function `IRP_MJ_LOCK_CONTROL` → `NtfsLockControl` (`lockctrl.c`).
- Per-FCB `FILE_LOCK` (`ntfs.h`), initialized in `fcb.c`, unlocked on cleanup.
- Lock IRP ownership: `FsRtlProcessFileLock(&Fcb->FileLock, Irp, NULL)` ([Learn](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-_fsrtl_advanced_fcb_header-fsrtlprocessfilelock)); only regular file objects (`NtfsIsRegularFileObject`).
- Read/write paths consult `FsRtlCheckLockForReadAccess` / `FsRtlCheckLockForWriteAccess` (`rw.c`); fast I/O uses the FsRtl fast lock checks (`fastio.c`).

### Page files (not implemented)

- Learn: `SL_OPEN_PAGING_FILE` on the create stack means the file is a paging file ([IRP_MJ_CREATE](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/ifs/irp-mj-create)).
- In-tree: `create.c` has a **commented** read of that flag (`PagingFileCreate = (Stack->Flags & SL_OPEN_PAGING_FILE) ? TRUE : FALSE`); it does not drive behavior.
- In-tree: `rw.c` remarks state page files are not implemented on the write dispatch path.

### Transactions / TxF (not implemented)

- No TxF / resource-manager create, commit, or rollback path under `drivers/filesystems/ntfs` cited here.
- `rw.c` remarks list transactions as not implemented.
- Do **not** invent TxF on-disk layouts. Open item until a guest log names the missing operation and a probe/spec session cites an allowed source.

### `NtfsChkdsk` (not implemented)

- No `NtfsChkdsk` (or equivalent in-driver repair entry) under `drivers/filesystems/ntfs`.
- Host/volume consistency for Windows attaching a Tavera volume is tracked separately ([`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md) / **NTFS-MOUNT-001**); that is not an in-driver `NtfsChkdsk` substitute.

### Product surface for volume check (not `NtfsChkdsk`) **(Rev.1)**

**(Rev.1)** GitHub #3’s fourth bullet named `` `NtfsChkdsk` `` as if it were an in-driver operation beside locks / page files / TxF. That name does not match the documented Windows check path or anything callable under `drivers/filesystems/ntfs`.

**(Rev.1)** Documented check / dirty path (Learn only):

1. User-mode [chkdsk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) examines metadata; `/f` (and related fix flags) require a lock or a scheduled next-boot run.
2. [fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty) sets or queries the volume dirty bit; when set, [autochk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/autochk) checks the volume at the next restart. [chkntfs](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkntfs) can also schedule automatic checking.
3. In-tree driver surface for that story: `FSCTL_MARK_VOLUME_DIRTY` / `FSCTL_IS_VOLUME_DIRTY` in `fsctl.c` (also cited under log-replay / **NTFS-LOG-002**). `NtfsVerifyVolume` is the media-verify stub (`STATUS_WRONG_VOLUME`), not chkdsk.

**(Rev.1)** **Out of scope for #3:** inventing an in-driver `NtfsChkdsk` export, FSCTL, or repair IRP. **In scope elsewhere:** host/`chkdsk` readability of a Tavera volume (**NTFS-MOUNT-001** / [`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md)); dirty remount black-box already covered by suite LogReplay/LogOps.

**(Rev.1)** **NTFS-CHKDSK-001** resolved as de-scope. No implementer pass for this slice.

## Behavior

### Locks — required behavior

1. `IRP_MJ_LOCK_CONTROL` on a regular file FCB is handed to `FsRtlProcessFileLock` on that FCB’s `FileLock`.
2. Conflicting byte-range locks block or fail reads/writes per FsRtl checks already wired in `rw.c` / `fastio.c`.
3. Cleanup releases locks via `FsRtlFastUnlockAll`.

### Page files / TxF / `NtfsChkdsk`

No acceptance recipe in this revision. Implementer must **not** invent paging-file cache bypass, TxF metadata, or an in-driver chkdsk. When a guest serial/suite log names one of these, open or extend the matching open-item and cite a Learn URL or capture before coding.

**(Rev.1)** The `NtfsChkdsk` clause above is superseded for product scope: that slice is **closed** (see Product surface). Page files and TxF remain deferred with no invented recipe.

## Observed behavior

- Suite / compliance guests already exercise ordinary create/read/write/rename/delete/remount without requiring TxF or in-driver chkdsk (symptom #3 body).
- Locks: in-tree wiring as listed under Sources (no separate differential log required for this documentation pass).
- Page-file / TxF / `NtfsChkdsk`: absence confirmed by in-tree search (`create.c` commented flag; `rw.c` remarks; no `NtfsChkdsk` symbol).

**(Rev.1)** No guest log has named an in-driver `NtfsChkdsk` call. Learn documents `chkdsk` / `autochk` / dirty bit as the check path. In-tree search still finds no `NtfsChkdsk` symbol.

## Compatibility notes

- Closing the **lock** slice of #3 does **not** close page files, TxF, or `NtfsChkdsk`.
- Milestone text that says each missing operation is one pass when a log names it still applies to the three deferred slices.

**(Rev.1)** Closing the **chkdsk** slice of #3 (de-scope) does **not** close page files or TxF, and does **not** close host mount **NTFS-MOUNT-001**.

## Test plan

**Locks (partial #3):**

1. Guest or suite: lock a byte range on a regular file (`LockFile` / equivalent), confirm a conflicting read/write fails or waits as FsRtl dictates; unlock; confirm access returns.
2. Confirm volume / non-regular objects still get `STATUS_INVALID_PARAMETER` / `STATUS_INVALID_DEVICE_REQUEST` from `NtfsLockControl` as coded.

**Deferred slices:** no test plan until an open-item gains a cited recipe.

**(Rev.1) Chkdsk slice (closed):** no in-driver test. Host readability remains under **NTFS-MOUNT-001** test plan. Dirty mark/query remains under LogReplay/LogOps / **NTFS-LOG-002**.

## Open questions

- Full paging-file create + non-cached I/O path when `SL_OPEN_PAGING_FILE` is set — blocked (**NTFS-PAGE-001**).
- TxF surface — blocked (**NTFS-TXF-001**).
- In-driver `NtfsChkdsk` — blocked (**NTFS-CHKDSK-001**); distinct from host mount **NTFS-MOUNT-001**.

**(Rev.1)** In-driver `NtfsChkdsk` — **resolved / out of scope** (**NTFS-CHKDSK-001**). Host `chkdsk` cleanliness remains **NTFS-MOUNT-001**.

## Review log

- 2026-11-18: Spec agent — locks from in-tree + Learn FsRtl/create; page/TxF/chkdsk deferred without invention (implementer partial #3 notes).
- 2026-11-18 **(Rev.1)**: Spec agent — chkdsk slice de-scoped from Learn dirty/`chkdsk`/`autochk` + in-tree FSCTLs; **NTFS-CHKDSK-001** resolved; page/TxF still open.

## Revision log

- **Rev.0** (2026-11-18): Initial partial #3 spec; locks acceptance; three deferred open-items.
- **Rev.1** (2026-11-18): Close mislabeled `NtfsChkdsk` slice; document Learn check path; no implementer API.
