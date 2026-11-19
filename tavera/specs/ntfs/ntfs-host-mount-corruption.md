<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-host-mount-corruption.md
PURPOSE:     Observed host Mount-DiskImage/chkdsk failures and cited on-disk mismatches
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS host mount corruption (kept disk)

## Header and provenance

Component: kept Tavera NTFS volume readability on Windows host (`Mount-DiskImage` / `chkdsk`).  
Version: 1. Author: Ryan Wilson. Date: 2026-11-18.  
Only allowed sources below were used.

## Sources

| Source | Date read |
|--------|-----------|
| [chkdsk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) | 2026-11-18 |
| [MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) | 2026-11-18 |
| [MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) (`$J` / `$MAX` on `$Extend\$UsnJrnl`) | 2026-11-18 |
| `drivers/filesystems/ntfs/fsctl.c` — `NtfsFileRecordIsConsistent` | 2026-11-18 |
| `sdk/lib/fslib/ntfslib/ntfslib.h` / `drivers/filesystems/ntfs/ntfs.h` — `ATTR_RECORD` / `NTFS_ATTR_RECORD` | 2026-11-18 |
| `output/ntfs/issue-22-host-mount/host-mount-obs.txt` (`ISSUE-22-HOST-MOUNT-OBS` … `END`) | 2026-11-18 |
| [`tavera/probes/ntfs/probe-v8/`](../../probes/ntfs/probe-v8/) → `output/ntfs/probe-v8/mount-frs-report.txt` (`NTFS-MOUNT-FRS-REPORT` … `END`) | 2026-11-18 |
| `output/ntfs/issue-22-host-mount/host-mount-obs-retest-final.txt` (`ISSUE-22-HOST-MOUNT-OBS` … `END`) | 2026-11-18 |
| [`tavera/probes/ntfs/probe-v12/`](../../probes/ntfs/probe-v12/) → `output/ntfs/probe-v12/mount-deep-report.txt` (`NTFS-MOUNT-DEEP-REPORT` … `END`) | 2026-11-18 |
| `drivers/filesystems/ntfs/ntfs.h` — `NTFS_ATTR_SPARSE` (`0x8000`), `NTFS_FILE_TYPE_SPARSE` (`0x200`) | 2026-11-18 |
| [`ntfs-sparse.md`](ntfs-sparse.md) / [MS-FSA: FSCTL_SET_SPARSE](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/b5b3dd4c-b585-49d7-960e-fbfbf7a85af0) | 2026-11-18 |

## Purpose and scope

Give the implementer **capture-cited** mismatches on the kept disk that Windows host tools reject, without inventing Microsoft field names beyond types already in this tree.

In scope: FRS named by the host `chkdsk` observation; attribute header / runlist / `AllocatedSize` consistency as checked by probe-v8 using in-tree record layout.  
Out of scope: inventing `$J` journal record bodies; claiming Windows chkdsk’s private algorithm.

## Interfaces

No new public API. Writers involved (in-tree):

- USN journal `$J` growth / create in `drivers/filesystems/ntfs/ntfs3x.c` (see also [`ntfs-usn.md`](ntfs-usn.md)).
- Ordinary non-resident `$DATA` writers for user files.

Fields used in checks (already in `NTFS_ATTR_RECORD` / `ATTR_RECORD`):

- `Type`, `Length`, `IsNonResident`, `NameLength` / name
- Resident: `ValueLength`, `ValueOffset`
- Non-resident: `LowestVCN`, `HighestVCN`, `MappingPairsOffset`, `AllocatedSize`, `DataSize`, `InitializedSize`

## Behavior (acceptance for implementer)

1. For every non-resident attribute this driver writes, **`AllocatedSize` (bytes) must equal** `(sum of mapping-pair cluster lengths) * BytesPerCluster`, and must be a multiple of the cluster size.
2. **`sum of mapping-pair cluster lengths` must equal** `HighestVCN - LowestVCN + 1` when those VCNs describe a single contiguous VCN range for that attribute record.
3. Resident attributes must pass the same resident value bounds as `NtfsFileRecordIsConsistent` in `fsctl.c`.

**(Rev.2)** Additional acceptance from probe-v12 (still using only in-tree flag names):

4. **`AllocatedSize` counts non-hole clusters only:** `(sum of mapping-pair lengths with a non-zero offset / real LCN) * BytesPerCluster`. Hole runs use the in-tree sparse sentinel (`NTFS_SPARSE_LBO` / mapping-pair offset length 0).
5. Do **not** leave `NTFS_ATTR_SPARSE` (`0x8000`) set on an attribute whose mapping pairs have **zero** hole clusters. Host retest showed `AllocatedSize = run_sum × cluster` alone still yields chkdsk “allocated size incorrect” on `$J` while this flag/holes mismatch remains.
6. When `NTFS_ATTR_SPARSE` is set on any stream, `STANDARD_INFORMATION` must carry `NTFS_FILE_TYPE_SPARSE` (`0x200`), per MS-FSA sparse rules cited in [`ntfs-sparse.md`](ntfs-sparse.md). Probe-v12 saw ATTR sparse set with SI `0x200` clear on `$UsnJrnl`.
7. After a directory entry is replaced, prior file-record segments for that name must **not** remain in-use claiming the same parent/`$FILE_NAME` while `$I30` indexes a different MFT number. Claimed data LCNs must be marked in-use in `$Bitmap` and must not overlap the survivor’s data runs.

## Observed behavior

### Host tools (Windows 10 host)

From `output/ntfs/issue-22-host-mount/host-mount-obs.txt`:

- `Mount-DiskImage` of converted `veros-ntfs/disk.qcow2` succeeded (`MOUNT_DISKIMAGE_OK`).
- Root listing failed (`DIR_FAIL`).
- `chkdsk` without `/f` reported corrupt attribute type `0x80` on file record segments **0x17**, **0x7C7**, **0x7D3**, then `Errors found. CHKDSK cannot continue in read-only mode.`

### Raw dump (probe-v8)

Probe: [`tavera/probes/ntfs/probe-v8/`](../../probes/ntfs/probe-v8/). Image: kept disk raw under `output/ntfs/probe-v8/kept-disk.raw`. Report: `output/ntfs/probe-v8/mount-frs-report.txt` (`NTFS-MOUNT-FRS-REPORT` … `NTFS-MOUNT-FRS-REPORT-END`). Volume offset 1048576; `bps=512`, `cluster=4096`, `rec_bytes=1024`.

| FRS | `$FILE_NAME` | `$DATA` name | In-tree bounds (`NtfsFileRecordIsConsistent`-style) | Run sum vs VCN span | AllocatedSize vs run sum |
|-----|--------------|--------------|------------------------------------------------------|---------------------|---------------------------|
| 0x17 | `$UsnJrnl` | `$J` (non-res) | pass (`DATA_ATTR_OK`) | 98 == 98 | **FAIL** `alloc_clusters=97` `run_sum=98` (`AllocatedSize=397312`) |
| 0x17 | `$UsnJrnl` | `$Max` (res) | pass | n/a | n/a |
| 0x7C7 | `data.safe.bin` | unnamed non-res | pass | 16 == 16 | pass (`65536`) |
| 0x7D3 | `data.safe.bin` | unnamed non-res | pass | 6 == 6 | pass (`24576`) |

Captures: `output/ntfs/probe-v8/captures/mft_record_0x17.bin`, `mft_record_0x7C7.bin`, `mft_record_0x7D3.bin`.

**(Rev.0 / first draft)** Implementer **must** fix `$UsnJrnl:$J` so `AllocatedSize` matches the mapping-pair cluster sum × cluster size (and keep VCN span consistent). Cite the FAIL line in `mount-frs-report.txt`.

**(Rev.0)** FRS **0x7C7** / **0x7D3**: probe-v8 found **no** bounds or alloc/run mismatch. Host `chkdsk` still named them. Do **not** invent a second defect. After the `$J` fix, re-run host `chkdsk` / listing; if those FRSs remain named, open a follow-on raw probe (bitmap / LCN overlap / stream body) — tracked under **NTFS-MOUNT-001** notes.

## Compatibility notes

- Fresh-format clean-mount acceptance remains in [`ntfs-windows-clean-mount.md`](ntfs-windows-clean-mount.md); this spec is about the **kept** disk host path (GitHub #22).
- Catalog `UsnJrnl` / `UsnJournal` black-box FSCTL rows do not prove host `chkdsk` clean.

## Test plan

1. Rebuild / write `$J` on a volume (or repair kept disk writers).
2. Host: convert to non-sparse VHDX → `Mount-DiskImage` → list root → `chkdsk` without `/f`.
3. Re-run [`Invoke-NtfsMountFrsProbe.ps1`](../../probes/ntfs/probe-v8/Invoke-NtfsMountFrsProbe.ps1) on the image: no `FAIL alloc_vs_runs` for `$J`.
4. Expect host obs without `DIR_FAIL` / chkdsk attribute corrupt on 0x17 when `$J` is consistent.

## Open questions

- Why host `chkdsk` still lists 0x7C7 / 0x7D3 when probe-v8 alloc/run/bounds pass — unknown without a further probe.
- Whether `DIR_FAIL` clears after `$J` alone — verify on retest.

### Host retest after AllocatedSize=run_sum (kept-disk copy) **(Rev.1)**

**(Rev.1)** Observation `output/ntfs/issue-22-host-mount/host-mount-obs-retest-final.txt` (`ISSUE-22-HOST-MOUNT-OBS` … `END`, 2026-11-18):

- On a **copy** of the kept disk, `$J` `AllocatedSize` was set to **401408** (= **98** × 4096) so probe-v8 reports `alloc_clusters=98` / `run_sum=98` (no `FAIL alloc_vs_runs`).
- `Mount-DiskImage` succeeded; IFS ~16 GiB.
- Root listing still **`DIR_FAIL`** (drive letter and `\\?\Volume{…}\`).
- `chkdsk` without `/f`: **“The total allocated size in attribute record (80, $J) of file …0017 is incorrect.”** (wording changed vs prior generic “corrupt”); FRS **0x7C7** / **0x7D3** still **corrupt**.
- Conclusion: matching `AllocatedSize` to run-sum is **necessary for the probe check** but **not sufficient** for host `chkdsk` / directory mount. Do **not** invent a second `$J` or 0x7C7/0x7D3 layout; open a follow-on raw-format probe. The kept production image is unchanged until a Tavera boot with the fixed writer rewrites `$J`.

### Deep raw probe (probe-v12) **(Rev.2)**

**(Rev.2)** Probe: [`tavera/probes/ntfs/probe-v12/`](../../probes/ntfs/probe-v12/). Report: `output/ntfs/probe-v12/mount-deep-report.txt` (`NTFS-MOUNT-DEEP-REPORT` … `NTFS-MOUNT-DEEP-REPORT-END`). Same kept-disk raw as probe-v8.

**(Rev.2)** `$UsnJrnl:$J` (FRS **0x17**):

| Check | Result |
|-------|--------|
| `AllocatedSize` clusters vs non-hole run sum | **FAIL** 97 vs 98 (`AllocatedSize=397312`) |
| VCN span vs non-hole runs (no holes) | pass 98 == 98 |
| `NTFS_ATTR_SPARSE` vs hole runs | **FAIL** flag set, `hole_clusters=0` |
| ATTR sparse vs SI `NTFS_FILE_TYPE_SPARSE` | **FAIL** ATTR `0x8000`, SI attrs `0x00000006` (bit `0x200` clear) |
| `$Bitmap` bits for `$J` LCNs | pass (all claimed in-use) |

**(Rev.2)** Parent directory FRS **0x6EA** (`extension-store`): `$I30` `INDEX_ROOT` has a single `data.safe.bin` hit → MFT **0x7E5** (survivor). FRS **0x7C7** and **0x7D3** both remain in-use with `$FILE_NAME` parent **0x6EA** / `data.safe.bin` but are **not** in that index (**orphan_frs**).

**(Rev.2)** FRS **0x7C7**:

- **FAIL** orphan vs `0x6EA` `$I30`
- **FAIL** `$Bitmap`: seven claimed LCNs free (`1455512`, `1455514`, `1455515`, `1455523`–`1455526`)
- **FAIL** LCN overlap with survivor **0x7E5** (`1455516..1455522`)

**(Rev.2)** FRS **0x7D3**:

- **FAIL** orphan vs `0x6EA` `$I30`
- pass `$Bitmap` for its six LCNs; pass no LCN overlap with **0x7E5**

**(Rev.2)** Implementer slice (cite `mount-deep-report.txt` FAIL lines): keep behavior rules 1–7 above. Host `chkdsk` / `DIR_FAIL` retest after writers satisfy `$J` sparse/alloc consistency **and** stop leaving orphan in-use FRS / overlapping or free-marked data LCNs on replace. Do not invent chkdsk’s private wording algorithm.

## Open questions **(Rev.2 update)**

**(Rev.2)** Prior open question (“why 0x7C7 / 0x7D3 when alloc/run pass”) is answered by probe-v12 orphan / bitmap / overlap FAILs above. Remaining open: whether clearing `$J` `NTFS_ATTR_SPARSE` (with `AllocatedSize` aligned to non-hole runs) plus retiring orphan FRS is **sufficient** for host `DIR_FAIL` to clear — verify with a new host Mount-DiskImage / `chkdsk` retest; do not assume.

## Review log

- 2026-11-18: Spec agent — probe-v8 cite only; no invented layouts; `$J` alloc/run FAIL is the implementable slice for **NTFS-MOUNT-001**.
- 2026-11-18 **(Rev.1)**: Spec agent — host retest after AllocatedSize patch; still DIR_FAIL + chkdsk errors.
- 2026-11-18 **(Rev.2)**: Spec agent — probe-v12 deep report; sparse/`$J` + orphan/bitmap/overlap for 0x7C7/0x7D3; no invented layouts.

## Revision log

- **Rev.0** (2026-11-18): Initial probe-v8-backed recipe for `$J` AllocatedSize vs runs; 0x7C7/0x7D3 deferred.
- **Rev.1** (2026-11-18): Host retest with AllocatedSize=run_sum; not sufficient; #22 stays open.
- **Rev.2** (2026-11-18): probe-v12; `$J` sparse without holes + SI mismatch; 0x7C7/0x7D3 orphan (+ 0x7C7 bitmap/overlap).
