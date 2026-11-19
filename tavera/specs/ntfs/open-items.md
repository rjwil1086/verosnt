<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/open-items.md
PURPOSE:     Spec and implementer handoff queue for NTFS
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS open items

Handoff file for implementer ↔ spec agent. Do not delete entries. See `.cursor/rules/tavera-specs-open-items.mdc`.

## Format

```text
### ID
- Status: open | resolved
- Opened: YYYY-MM-DD
- Question: …
- Blocks: …
- Resolved: YYYY-MM-DD (when resolved)
- Resolution: cite source or probe; link to spec change
```

## Items

### NTFS-QUOTA-001
- Status: resolved
- Opened: 2026-11-18
- Question: After `NtSetVolumeInformationFile` with `FileFsControlInformation` / `FILE_VC_QUOTA_TRACK` on `\\.\T:`, `FSCTL_SET_QUOTA` returned `ERROR_INVALID_FUNCTION`. What documented sequence writes a per-user `$Q` value the host dump can capture?
- Blocks: Closing the on-disk `$Q` capture gate in `ntfs-quota-on-disk.md`.
- Resolved: 2026-11-18
- Resolution: Use [fsutil quota](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-quota) `track` then `modify <vol> 4096 8192 <user>` via [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/). Serial `output/ntfs/probe-v1/guest-serial.log` shows `fsutil modify ok` and `NTFS-RAW-PROBE-DONE ok`. Resident `$Q` root is 384 bytes in `output/ntfs/probe-v1/captures/quota_q_index_root.bin` (manifest `NTFS-RAW-DUMP-END`). Spec updated: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md). Field layout inside that blob remains for a later cite-only pass. `FSCTL_SET_QUOTA` still returned `e=1` after fsutil (logged as optional).

### NTFS-EFS-001
- Status: resolved
- Opened: 2026-11-18
- Question: `EncryptFileW` on a `DefineDosDevice` path returned `ERROR_PATH_NOT_FOUND` even when `GetFileAttributesW` succeeded. How does probe-v1 obtain an EFS on-disk body / `$LOGGED_UTILITY_STREAM`?
- Blocks: Closing the encrypted-file-body gate in `ntfs-encrypted-file-body.md`.
- Resolved: 2026-11-18
- Resolution: Confirm `FILE_SUPPORTS_ENCRYPTION`, write under `T:\efs\secret.txt`, call `EncryptFileW` on `GetFinalPathNameByHandleW` (not the DosDevice path alone). Serial: `Encrypt ok finalpath`, `NTFS-RAW-PROBE-DONE ok`. Captures: `secret_data.bin` (512 B ciphertext, non-resident), `secret_logged_utility.bin` (672 B, `$EFS` / type `0x100`), `mft_record_39_secret.bin`. Spec updated: [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md). FEK framing inside those blobs is not named yet.

### NTFS-QUOTA-002
- Status: resolved
- Opened: 2026-11-18
- Question: Where in the resident `$Q` index-root bytes do the fsutil modify threshold and limit appear?
- Blocks: Implementer writing a matching on-disk quota value.
- Resolved: 2026-11-18
- Resolution: probe-v2 A (4096/8192) vs B (8192/16384). In `quota_q_index_root.bin`, LE int64 @ **316** = threshold, LE int64 @ **324** = limit. Cited in [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md); `output/ntfs/probe-v2/diff-report.txt`.

### NTFS-EFS-002
- Status: resolved
- Opened: 2026-11-18
- Question: What are the fields inside the 16-byte `$EFS` prefix that is identical for two files encrypted in one session, and inside the regions that differ from offset 16 / 416?
- Blocks: Full on-disk EFS framing for `NtfsSetEncryption` / encrypted read-write.
- Notes: probe-v2 same-boot `secretA_logged_utility.bin` vs `secretB_logged_utility.bin` in `output/ntfs/probe-v2/A/captures/` and `diff-report.txt`.
- Notes (probe-v6, 2026-11-18): [`tavera/probes/ntfs/probe-v6/`](../../probes/ntfs/probe-v6/) Z/S/L/P/R + `efs-report.txt`. `$EFS` always **672**; bytes **0..15** fixed (`A0 02 …` / LE size 672 + u32 2); first diff at **16**; `$DATA` length = plaintext length. **Microsoft field names: none.** Structural recipe in [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md) Rev.3.
- Resolved: 2026-11-18
- Resolution: Structural on-disk recipe cited (size/header/regions). Implementer catalog work continues under **NTFS-EFS-004** (crypto payload at 16+ must not be invented).
- Notes (implementer, 2026-11-18): Structural framing shipped in `NtfsSetEncryption` / `AddLoggedUtilityStream`: non-resident `$EFS` **672**, fixed 16-byte header, offset 16+ zero (FEK not invented), `$DATA` length unchanged, plaintext read/write passthrough. Catalog rows verified under **NTFS-EFS-004**.

### NTFS-EFS-004
- Status: resolved
- Opened: 2026-11-18
- Question: Implement `NtfsSetEncryption` / encrypt+readback so suite `Encryption` and `EfsBody` match Windows 10 22H2 using [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md) Rev.3–4.
- Blocks: Catalog `Encryption` and `EfsBody` green on Tavera.
- Notes: Windows baseline must include `EfsBody` green. Structural `$EFS` 672 + header cited by probe-v6; per-file bytes from offset 16 still capture-only — do not invent FEK framing. Verify with `-Phase compliance`.
- Notes (implementer, 2026-11-18): `FSCTL_SET_ENCRYPTION` → non-resident `$EFS` + `NTFS_FILE_TYPE_ENCRYPTED` on SI/FCB; `NtfsReadEncrypted`/`NtfsWriteEncrypted` plaintext `$DATA` passthrough. Compliance: `Encryption` **green**, `EfsBody` **green** (`out/share-compliance/result.log`, 47 of 48). Still red: `LogReplay` / `fsutil dirty set` (**NTFS-LOG-001**). Spec agent may mark this item resolved.
- Resolved: 2026-11-18
- Resolution: Catalog path closed. Implementer structural `$EFS` + passthrough; suite `Encryption` / `EfsBody` green (`out/share-compliance/result.log`, later 49/49). Opening a Windows-wrapped `$EFS` remains GitHub #1 — not this item. Spec: [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md).

### NTFS-LOG-001
- Status: open
- Opened: 2026-11-18
- Question: What restart-page / log-record field layout is in `logfile_data.bin` after create-only vs create-rename-delete traffic?
- Blocks: Real `NtfsReplayLog` / log-record write.
- Notes: probe-v2 A vs B `logfile_data.bin` (1 MiB each); 27939 diff runs in `diff-report.txt`. No field names assigned yet.
- Notes (probe-v5, 2026-11-18): [`tavera/probes/ntfs/probe-v5/`](../../probes/ntfs/probe-v5/) C/W/D/X dumps + `log-report.txt`. Dirty remount **readback ok** on Windows (black-box suite `LogReplay` is now definable). Still **thousands** of `$LogFile` byte diffs; **no field names**. Item stays **open** for restart/log-record layout. Spec: [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.1.
- Notes (suite, 2026-11-18): Catalog `LogReplay` added ([`features.ps1`](../../test-suites/ntfs/features.ps1) / agent dirty remount + readback). Black-box only; item stays **open** for field layout. Spec: [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.2.
- Notes (probe-v7, 2026-11-18): [`tavera/probes/ntfs/probe-v7/`](../../probes/ntfs/probe-v7/) F/C/W/N/X/T + `log-iso-report.txt`. Structural recipe (1 MiB dumps; ~7.8k–9.2k DIFFRUNS per op; 512-byte fixup cadence). **Field names: none.** Dirty remount + rename black-box ready for suite `LogOps`. Spec: [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.3. Item stays **open** for named restart/log-record fields. Implementer apply+diff handoff: **NTFS-LOG-002**.
- Notes (suite, 2026-11-18): Catalog `LogOps` added ([`features.ps1`](../../test-suites/ntfs/features.ps1) / agent create+write+rename+dirty remount+readback under new name). Black-box only. Spec: [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.4. Windows baseline **49 green / 0 red** (`out/share-windows/result.log`, `NTFS-AGENT-DONE`; includes `LogOps`).

### NTFS-LOG-002
- Status: resolved
- Opened: 2026-11-18
- Question: After applying the probe-v7 structural `$LogFile` recipe in [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.3, which remaining uncited bytes / status gaps close by comparing the same create/write/rename/delete/dirty-remount ops’ `$LogFile` dumps and guest-visible statuses on Windows 10 22H2 vs Tavera?
- Blocks: Completing a minimal `NtfsReplayLog` / `NtfsLogFileRecord` beyond the recipe without inventing field names (**NTFS-LOG-001** still owns named layout).
- Notes: Probe session documented the differential follow-on; do not invent Microsoft field names. Suite `LogReplay` / `LogOps` stay black-box until LOG-001 closes.
- Notes (implementer, 2026-11-18): Black-box catalog path: `FSCTL_MARK_VOLUME_DIRTY` / `FSCTL_IS_VOLUME_DIRTY` on `$VOLUME_INFORMATION`; after dirty mount, no-op `NtfsReplayLog` then `NtfsClearVolumeDirty`. Did **not** invent `$LogFile` field names or record bodies (**NTFS-LOG-001** still open). Compliance: `LogReplay` **green**, `LogOps` **green** (`out/share-compliance/result.log`, 49 of 49). Spec agent may mark the black-box half resolved; Win↔Tavera `$LogFile` dump diffs for named layout remain under **NTFS-LOG-001**.
- Resolved: 2026-11-18
- Resolution: Black-box catalog half closed (`LogReplay` / `LogOps` green; dirty FSCTL + no-op replay / clear). Named `$LogFile` field layout and Win↔Tavera dump diffs stay under **NTFS-LOG-001**. Spec: [`ntfs-log-replay.md`](ntfs-log-replay.md); restart-mount recipe under **NTFS-LOG-003** / [`ntfs-log-restart-mount.md`](ntfs-log-restart-mount.md).

### NTFS-LOG-003
- Status: resolved
- Opened: 2026-11-18
- Resolved: 2026-11-18
- Question: What probe-local restart-page tags/slots does the implementer use for GitHub #2 (dirty mount: empty slots → clear dirty/RW; non-empty → RO; never apply records)?
- Blocks: Implementer refusal of #2 without a cited recipe (field names still none under **NTFS-LOG-001**).
- Resolution: [`ntfs-log-restart-mount.md`](ntfs-log-restart-mount.md) from [`tavera/probes/ntfs/probe-v11`](../../probes/ntfs/probe-v11) / `output/ntfs/probe-v11/restart-report.txt` (`NTFS-LOG-RESTART-REPORT` … `END`) on probe-v7 dumps. `RSTR`/`RCRD` tags; page size `u32@+0x10=0x1000`; slots `u64@+0x60` and `u64@+0x68` always 0 in captured stages including dirty remount T. Do **not** apply `RCRD` bodies.
- Notes (implementer, 2026-11-18): `NtfsReplayLog` recognizes probe-local `RSTR` + `PAGE_SIZE_U32_AT_10`; empty `SLOT_U64_AT_60/68` → clear dirty/RW; non-empty → stay RO (`STATUS_MEDIA_WRITE_PROTECTED` to skip clear). Still **never** applies `RCRD`. Microsoft names remain under **NTFS-LOG-001**.

### NTFS-QUOTA-003
- Status: resolved
- Opened: 2026-11-18
- Question: How does the resident `$Q` index root grow from the format-time size (and from the post-`track` size) to at least 332 bytes so offsets 316/324 exist, and how is a `QuotaLimit` of `-2` (delete entry per MS-FSCC 2.4.41) encoded on disk?
- Blocks: `NtfsSetQuota` when `$Q` root length is below 332 bytes; delete-entry path for limit `-2`; suite `QuotaAccounting` after track (currently red on `fsutil modify`).
- Notes: Implementer writes threshold/limit only at the probe-cited offsets 316/324 when the root is already large enough (`ntfs-quota-on-disk.md`, NTFS-QUOTA-002). Other differing `$Q` offsets remain unnamed. Suite 2026-11-18: Windows `QuotaAccounting` green via fsutil; Tavera red on `fsutil track` first — enable tracking before growth/`-2` matter for catalog green. Implementer 2026-11-18: `FileFsControlInformation` query/set stores `FILE_VC_QUOTA_TRACK` on the VCB (MS-FSCC + fsutil Learn); `$Q` growth bytes and `-2` delete encoding still not invented. Compliance re-run 2026-11-18: `fsutil track` green after in-tree `fsutil quota` + agent `GetSystemDirectoryW` path; still red on `fsutil modify` (short `$Q` / growth and `-2` remain open). Spec agent 2026-11-18: retained captures are only post-`modify` 384-byte roots (probe-v1/v2). Format-only (208) and track-only (288) sizes are recorded in [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) but those blobs are not kept under `output/`. **No implementer path without probe-v3** dumping `$Q` at format, after `track`, after `modify` (and optionally after limit `-2`). Do not pad or invent entry bytes to reach 332/384.
- Notes (probe-v3, 2026-11-18): **Growth half answered** in [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.3 from [`tavera/probes/ntfs/probe-v3/`](../../probes/ntfs/probe-v3/). Format **208** → track **384** (keys 1, 0x100, 0x101, 0x102; threshold/limit **-1** at data+0x18/+0x20 of key 0x102 = absolute 316/324). Modify stays 384 and writes 4096/8192 there. Implementer may grow on track + write relative fields (recipe in Rev.3). **Delete half still open:** stage D `FSCTL_SET_QUOTA` QuotaLimit=-2 returned `e=1`; no on-disk delete observed. Item stays **open** until `-2` encoding is captured or explicitly de-scoped.
- Notes (implementer, 2026-11-18): `NtfsGrowQuotaIndexForTrack` on `FILE_VC_QUOTA_TRACK` grows resident `$Q` via Rust `ntfs_quota_grow_for_track` (live `SeLocalSystemSid` + caller token SID; packing from `files.c` MkViewRootN/Qce). `NtfsSetQuota` writes relative data+0x18/+0x20 (key 0x102 / SID) with absolute 316/324 fallback. **`-2` delete still not implemented.** Compliance re-run: `QuotaAccounting` **green** (`out/share-compliance/result.log`, 44 of 45). Remaining suite red is `Encryption` / `EncryptFile finalpath` (`NTFS-EFS-002`). Item stays **open** for the `-2` delete half only.
- Resolved: 2026-11-18
- Resolution: Growth via probe-v3 ([`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.3); −2 via probe-v4 ([`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.4 / [`tavera/probes/ntfs/probe-v4/`](../../probes/ntfs/probe-v4/)): `NtSetQuotaInformationFile` STATUS_SUCCESS; on-disk keeps the SID leaf, clears thr/lim to **-1**, owner key **0x102→0x103**. Suite `QuotaDelete` deferred to a later harness session.
- Notes (suite, 2026-11-18): Catalog `QuotaDelete` added ([`features.ps1`](../../test-suites/ntfs/features.ps1) / agent NtSet −2 + query −1/−1). Spec: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.5.
- Notes (spec, 2026-11-18): Suite harness + Windows 47/47 done. Implementer −2 work moved to **NTFS-QUOTA-004** / [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.8.

### NTFS-QUOTA-004
- Status: resolved
- Opened: 2026-11-18
- Question: Implement `QuotaLimit == -2` per [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.4 + Rev.8 so Tavera matches Windows 10 22H2 for suite `QuotaDelete`.
- Blocks: Catalog `QuotaDelete` green on Tavera (`tavera/test-suites/ntfs/`).
- Notes: Windows baseline **47 green / 0 red** (`out/share-windows/result.log`, `NTFS-AGENT-DONE`). Clear-to−1 cited by probe-v4; stub today in `NtfsSetQuota` returns `STATUS_NOT_IMPLEMENTED`. No new probe required for the clear-to−1 path. Do not invent full leaf-removal bytes beyond cited behavior. Verify with `-Phase compliance` after the implementer change.
- Notes (implementer, 2026-11-18): Wired `NtSetQuotaInformationFile` / `NtQueryQuotaInformationFile` through `IRP_MJ_SET_QUOTA` / `IRP_MJ_QUERY_QUOTA` (`ntoskrnl/io/iomgr/iofunc.c`; NTFS register + dispatch). `NtfsSetQuota`: SID upsert for seed; `QuotaLimit == -2` clears thr/lim to −1 (optional key 0x102→0x103). `NtfsQueryQuota` emits `FILE_QUOTA_INFORMATION` from resident `$Q`. Compliance: `QuotaDelete` **green** (`out/share-compliance/result.log`, 45 of 47). Still red: `Encryption` / `EncryptFile finalpath` (**NTFS-EFS-002**), `LogReplay` / `fsutil dirty set` (**NTFS-LOG-001**). Spec agent may mark this item resolved.
- Resolved: 2026-11-18
- Resolution: Implementer clear-to−1 / key bump per probe-v4; suite `QuotaDelete` green (`out/share-compliance/result.log`). Spec: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.4 + Rev.8.

### NTFS-VER-001
- Status: resolved
- Opened: 2026-11-18
- Question: What extended volume-data fields does the suite’s `Version3_1` probe require, and how should Tavera fill them so the row matches Windows 10 22H2?
- Blocks: Catalog `Version3_1` green on Tavera.
- Notes: `tavera/test-suites/ntfs/out/share-compliance/result.log` → `Version3_1 red extended volume data missing`. Windows baseline green. Do not invent layout; cite Learn / in-tree / new probe. Implementer 2026-11-18: `GetNfsVolumeData` no longer rejects METHOD_BUFFERED when `UserBuffer` is NULL; fills in-tree `NTFS_EXTENDED_VOLUME_DATA` (Major/Minor from mount) per Learn + suite `ProbeVersion`. Compliance re-run 2026-11-18: row green after also filling a partial extended tail when the caller buffer is only the MinGW 8-byte prefix (`ByteCount`/`Major`/`Minor`). Spec agent may mark resolved.
- Resolved: 2026-11-18
- Resolution: Implementer filled in-tree `NTFS_EXTENDED_VOLUME_DATA` via `FSCTL_GET_NTFS_VOLUME_DATA` ([Learn](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-ntfs_extended_volume_data); `sdk/include/xdk/iotypes.h`). Suite `Version3_1` green on Tavera (`out/share-compliance/result.log`, 43 of 45 snapshot). Spec: [`ntfs-win10-compat.md`](ntfs-win10-compat.md) Rev.3.

### NTFS-EFS-003
- Status: resolved
- Opened: 2026-11-18
- Question: When should GetVolumeInformation report `FILE_SUPPORTS_ENCRYPTION` on a Tavera NTFS volume?
- Blocks: Suite `Encryption` green; meaningful `EncryptFileW` / `NtfsSetEncryption` work.
- Notes: Windows baseline green; Tavera `Encryption red FILE_SUPPORTS_ENCRYPTION clear` (`share-compliance/result.log`). Distinct from `NTFS-EFS-002` (`$EFS` byte fields). Implementer 2026-11-18: sets `FILE_SUPPORTS_ENCRYPTION` when `NtfsInfo.MajorVersion >= 3` (MS-FSA Appendix A) plus `FILE_VOLUME_QUOTAS`; `NtfsSetEncryption` / `$EFS` body remain blocked on `NTFS-EFS-002`. Compliance re-run 2026-11-18: flag clear is gone; suite row still red on `EncryptFile finalpath` (blocked by `NTFS-EFS-002`). Spec agent may resolve this flag-only item.
- Resolved: 2026-11-18
- Resolution: Flag-only item closed. Report `FILE_SUPPORTS_ENCRYPTION` when NTFS major ≥ 3 ([MS-FSA Appendix A](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4e3695bd-7574-4f24-a223-b4679c065b63)); implementer did so in `volinfo.c`. Suite no longer reports `FILE_SUPPORTS_ENCRYPTION clear`; remaining `Encryption` red is `EncryptFile finalpath`, which stays blocked on **NTFS-EFS-002**. Spec: [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md) Rev.2.

### NTFS-MOUNT-001
- Status: open
- Opened: 2026-11-18
- Question: Why does host `Mount-DiskImage` of the kept Tavera NTFS disk still fail guest-visible root listing / `chkdsk` without `/f`, and which in-tree writer must change once a raw-format probe cites the failing structures?
- Blocks: GitHub symptom #22; clean host mount of `virtual-machines/veros-ntfs/disk.qcow2`.
- Notes: Observation `output/ntfs/issue-22-host-mount/host-mount-obs.txt` (`ISSUE-22-HOST-MOUNT-OBS` … `END`): `MOUNT_DISKIMAGE_OK`; `DIR_FAIL`; `chkdsk` without `/f` → corrupt attribute records and `Errors found. CHKDSK cannot continue in read-only mode.` Spec: [`ntfs-windows-clean-mount.md`](ntfs-windows-clean-mount.md) Rev.1. Do **not** invent `$Secure`/`$SDS`/attribute layouts; if an unnamed on-disk fact is required, open a raw-format probe session.
- Notes (probe-v8, 2026-11-18): [`tavera/probes/ntfs/probe-v8/`](../../probes/ntfs/probe-v8/) + `output/ntfs/probe-v8/mount-frs-report.txt` (`NTFS-MOUNT-FRS-REPORT` … `END`). FRS **0x17** `$UsnJrnl:$J`: **FAIL alloc_vs_runs** (`alloc_clusters=97`, `run_sum=98`, `AllocatedSize=397312`, cluster 4096). FRS **0x7C7** / **0x7D3** (`data.safe.bin`): bounds + alloc/run **pass** — no second invented defect. Implementer recipe: [`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md). After `$J` fix, retest host mount/chkdsk; if 0x7C7/0x7D3 still named, further probe.
- Notes (implementer, 2026-11-18): `AddRun` in-place growth and `NtfsExtendAllocation` keep `AllocatedSize` aligned with run-sum × cluster before `UpdateFileRecord` ([`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md)). Did **not** invent 0x7C7/0x7D3 defects. Host Mount-DiskImage / chkdsk retest still required before closing this item / GitHub #22.
- Notes (host retest, 2026-11-18): Patched a **copy** of the kept disk so `$J` `AllocatedSize` = `run_sum × cluster` (**401408**; probe-v8 no longer `FAIL alloc_vs_runs`). Host obs `output/ntfs/issue-22-host-mount/host-mount-obs-retest-final.txt` (`ISSUE-22-HOST-MOUNT-OBS` … `END`): `MOUNT_DISKIMAGE_OK`; **`DIR_FAIL`** (letter and volume GUID); `chkdsk` without `/f` still fails — message changed to **allocated size incorrect** on `$J` FRS 0x17, and still **corrupt** on **0x7C7** / **0x7D3**. Writer fix alone does not rewrite the kept image; AllocatedSize=run_sum is **not** sufficient for host clean. Keep open; next raw probe must explain chkdsk’s `$J` allocated check and 0x7C7/0x7D3 (do not invent).
- Notes (probe-v12, 2026-11-18): [`tavera/probes/ntfs/probe-v12/`](../../probes/ntfs/probe-v12/) + `output/ntfs/probe-v12/mount-deep-report.txt` (`NTFS-MOUNT-DEEP-REPORT` … `END`). `$J`: **FAIL** `J_sparse_flag_without_holes` (`NTFS_ATTR_SPARSE`, `hole_clusters=0`), **FAIL** ATTR sparse vs SI `NTFS_FILE_TYPE_SPARSE` clear, **FAIL** alloc 97 vs non-hole 98. **0x7C7** / **0x7D3**: **FAIL** orphan (parent **0x6EA** `$I30` indexes only **0x7E5** for `data.safe.bin`); **0x7C7** also **FAIL** `$Bitmap` free claimed LCNs + LCN overlap with **0x7E5**. Recipe Rev.2: [`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md). Keep open until host retest after writers satisfy those FAILs.
- Notes (implementer, 2026-11-18): Format `$J` without `ATTR_IS_SPARSE` (`ntfslib/files.c`). `NtfsApplySparseAllocatedRules`: `AllocatedSize` = non-hole MCB sum × cluster; clear ATTR sparse when `hole_clusters=0` **and** no VCN gaps; clear SI `0x200`. `NtfsSetSparse` writes SI bit with ATTR. Orphan **0x7C7**/**0x7D3** replace path **not** done. Host Mount-DiskImage / chkdsk retest still required.
- Notes (implementer, 2026-11-18): Suite hang after `UsnJrnl` was Mm retry on write into unnamed MCB gaps without `NTFS_ATTR_SPARSE` (MFT 38): `NtfsMaterializeSparseRange` now always runs before map; fills unnamed gaps; named hole-without-sparse (format `$Bad`) stays `STATUS_ACCESS_DENIED`. False `STATUS_NOT_IMPLEMENTED` from breaking the MCB walk early was removed. Harness: agent emits `NTFS-AGENT-DONE` on serial + `DONE.ok`; `Invoke-NtfsSuite` treats serial DONE / S5 / `DONE.ok` as finished when vvfat misses `result.log` DONE. Orphans still open; host retest still required.
- Notes (implementer, 2026-11-18): Orphan replace **rule 7**: `NtfsDeleteFileRecord` treats index `STATUS_OBJECT_NAME_NOT_FOUND` as already-unlinked and still frees `$Bitmap` + `FreeMftEntry`; clears `FRH_IN_USE` on the in-memory record before `FreeClusters` so a later `UpdateFileRecord` cannot resurrect an in-use orphan; skips empty non-resident streams (`HighestVCN == -1`). Create/directory paths roll back with `NtfsDeleteFileRecord` if `$I30` add fails after `AddNewMftEntry`. Writer gap closed per Rev.2 / probe-v12; host Mount-DiskImage / `chkdsk` retest still required before closing this item / #22.
- Notes (suite, 2026-11-18): After orphan-replace + `$J` sparse/alloc writers: `-Phase compliance` **49 green / 0 red** (`tavera/test-suites/ntfs/out/share-compliance/result.log`, `NTFS-AGENT-DONE`; snapshot `out/ntfs31.txt` **49 of 49 / 100%**; serial `out/serial-compliance-194540.log` also `NTFS-AGENT-DONE`). Catalog path clean. Item stays **open** for host Mount-DiskImage / `chkdsk` retest on the kept disk (#22) — suite green does not close that.

### NTFS-COMP-001
- Status: resolved
- Opened: 2026-11-18
- Question: How should `NtfsUnitClusters` in `drivers/filesystems/ntfs/ntfs3x.c` reject a disk `CompressionUnit` that would overflow the shift or `UnitClusters * BytesPerCluster` before `NtfsReadCompressed` / write paths use it?
- Blocks: Memory safety on crafted non-resident compressed attributes (GitHub symptom #4).
- Notes: In-tree write path sets `CompressionUnit` to **4** or **0** (`ntfs3x.c`). `NtfsUnitClusters` today does `1u << CompressionUnit` when non-zero with no range check. Acceptance: overflow/zero-divisor safety only — not a Microsoft compression-unit table. Spec: [`ntfs-compression.md`](ntfs-compression.md) Rev.1.
- Notes (implementer, 2026-11-18): `NtfsUnitClusters` accepts only **0** or **4**; other values fail compressed read/write with `STATUS_FILE_CORRUPT_ERROR` before shift/multiply. Spec agent may mark resolved.
- Resolved: 2026-11-18
- Resolution: Implementer accepts only CompressionUnit **0** or **4**; other values → `STATUS_FILE_CORRUPT_ERROR` before shift/multiply. Spec: [`ntfs-compression.md`](ntfs-compression.md) Rev.1.

### NTFS-IDX-001
- Status: resolved
- Opened: 2026-11-18
- Question: How should `NtfsInsertRootEntry` in `drivers/filesystems/ntfs/ntfs3x.c` clamp index-root header sizes and the entry chain to the resident attribute / file-record bounds already typed in this tree before `RtlMoveMemory`?
- Blocks: Memory safety on crafted `$INDEX_ROOT` (GitHub symptom #5).
- Notes: Cite in-tree `INDEX_ROOT_ATTRIBUTE` / resident fields only; do not invent new Windows field names. Spec note under [`ntfs-reparse.md`](ntfs-reparse.md) / [`ntfs-object-id.md`](ntfs-object-id.md) / quota index paths as Rev.1 hardening.
- Notes (implementer, 2026-11-18): Resident value + header `AllocatedSize`/`TotalSizeOfEntries`/`FirstEntryOffset` + entry insert clamped to in-tree bounds before `RtlMoveMemory`. Spec agent may mark resolved.
- Resolved: 2026-11-18
- Resolution: Implementer clamps resident index-root header sizes and entry insert to in-tree bounds before `RtlMoveMemory`. Hardening cites under reparse/object-id/quota index Rev.1 notes.

### NTFS-ATTR-001
- Status: resolved
- Opened: 2026-11-18
- Question: How should `NtfsGetReparsePoint`, `NtfsQueryObjectId`, and `NtfsGetReparseTag` in `drivers/filesystems/ntfs/ntfs3x.c` require a resident attribute and a value range inside the attribute record before copying `Resident.ValueOffset` / `Resident.ValueLength`?
- Blocks: Memory safety on crafted attributes (GitHub symptom #6).
- Notes: Spec: [`ntfs-reparse.md`](ntfs-reparse.md) Rev.1; [`ntfs-object-id.md`](ntfs-object-id.md) Rev.1. No new Windows field names.
- Notes (implementer, 2026-11-18): `NtfsResidentValueInRecord` (same clamp as `NtfsFileRecordIsConsistent`) before reparse/object-id copies. Spec agent may mark resolved.
- Resolved: 2026-11-18
- Resolution: Implementer `NtfsResidentValueInRecord` clamp before reparse/object-id copies. Spec: [`ntfs-reparse.md`](ntfs-reparse.md) Rev.1; [`ntfs-object-id.md`](ntfs-object-id.md) Rev.1.

### NTFS-QUOTA-005
- Status: resolved
- Opened: 2026-11-18
- Question: How should the local `FSCTL_SET_QUOTA` path in `drivers/filesystems/ntfs/fsctl.c` require an enabled volume-management privilege before updating `$Quota`, and use an unused CTL function number in this driver’s CTL space?
- Blocks: Weak access on set-quota (GitHub symptom #8).
- Notes: Local macro today: `CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_BUFFERED, FILE_WRITE_DATA)` in `fsctl.c`. Learn (read 2026-11-18): [Managing Privileges in a File System](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/privileges) documents **SeManageVolumePrivilege** for volume-level FSCTL policy via `SeSinglePrivilegeCheck` / `SePrivilegeCheck`. Learn: [FILE_QUOTA_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_quota_information) states FILE_WRITE_DATA on the volume is required to set quota via `IRP_MJ_SET_QUOTA`. Do **not** invent a Microsoft FSCTL function number; pick an unused function number in this driver’s CTL space. Spec: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.9.
- Notes (implementer, 2026-11-18): Local CTL function **0x900** + `SeSinglePrivilegeCheck(SE_MANAGE_VOLUME_PRIVILEGE)` before `NtfsSetQuota`; `fsutil/quota.c` matched. Spec agent may mark resolved.
- Notes (implementer, 2026-11-18): Suite `QuotaAccounting` red on `fsutil modify` after the FSCTL privilege gate. `fsutil quota modify` now uses `NtSetQuotaInformationFile` + `SeIncreaseQuotaPrivilege` (same path as suite `QuotaDelete` / [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.4). Local FSCTL **0x900** keeps the SeManageVolume check for direct callers. Compliance re-run: `QuotaAccounting` **green** (49/49 marks; shutdown/DONE handoff tracked under **NTFS-MOUNT-001**).
- Resolved: 2026-11-18
- Resolution: Local CTL **0x900** + `SeManageVolumePrivilege` for direct FSCTL; catalog `fsutil quota modify` via `NtSetQuotaInformationFile` + `SeIncreaseQuotaPrivilege`. Suite `QuotaAccounting` green. Spec: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.9.

### NTFS-LOCK-001
- Status: resolved
- Opened: 2026-11-18
- Resolved: 2026-11-18
- Question: Are byte-range locks present for GitHub symptom #3, and what acceptance applies?
- Blocks: Partial close of #3 lock slice.
- Resolution: In-tree `NtfsLockControl` → `FsRtlProcessFileLock` ([`lockctrl.c`](../../drivers/filesystems/ntfs/lockctrl.c)); FCB `FileLock`; read/write/fastio/cleanup wired. Spec: [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md). Learn: [FsRtlProcessFileLock](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-_fsrtl_advanced_fcb_header-fsrtlprocessfilelock).

### NTFS-PAGE-001
- Status: resolved
- Opened: 2026-11-18
- Resolved: 2026-11-18
- Question: What create + I/O behavior is required when `SL_OPEN_PAGING_FILE` is set so a paging file works on Tavera NTFS?
- Blocks: Remaining page-file slice of GitHub #3.
- Notes: Learn documents the flag ([IRP_MJ_CREATE](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/ifs/irp-mj-create)). In-tree `create.c` has the flag read **commented out**; `rw.c` remarks say page files are not implemented. Do **not** invent a full paging-file path until a guest log names it and a recipe is cited. Spec: [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md).
- Resolution: Probe [`tavera/probes/ntfs/probe-v9`](../../probes/ntfs/probe-v9) / `output/ntfs/probe-v9/guest-serial.log` (`NTFS-PAGE-PROBE-DONE ok`): `NtCreatePagingFile` → `0x00000000`; ordinary `CreateFileW` → `e=32`. Implementer recipe: [`ntfs-pagefile.md`](ntfs-pagefile.md).
- Notes (implementer, 2026-11-18): Wired `SL_OPEN_PAGING_FILE` → `FCB_IS_PAGE_FILE` / `FSRTL_FLAG2_IS_PAGING_FILE`; ordinary open of a paging file returns `STATUS_SHARING_VIOLATION` ([`ntfs-pagefile.md`](ntfs-pagefile.md)).

### NTFS-TXF-001
- Status: resolved
- Opened: 2026-11-18
- Resolved: 2026-11-18
- Question: What TxF / transactional NTFS surface must Tavera implement when a guest log names it?
- Blocks: Transactions slice of GitHub #3.
- Notes: No in-tree TxF path; `rw.c` remarks say transactions are not implemented. **Deferred** — no allowed-source recipe in this session. Spec: [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md).
- Resolution: Probe [`tavera/probes/ntfs/probe-v10`](../../probes/ntfs/probe-v10) / `output/ntfs/probe-v10/guest-serial.log` (`NTFS-TXF-PROBE-DONE ok`): commit visible; rollback / uncommitted create → plain open `e=2`. Implementer recipe: [`ntfs-txf.md`](ntfs-txf.md). Do **not** invent `$RmMetadata` layouts from the API log.
- Notes (implementer, 2026-11-18): Process-local staging in `ktmw32` + `CreateFileTransactedW` ([`ntfs-txf.md`](ntfs-txf.md)): commit `MoveFile` to final name; rollback deletes staging; uncommitted final path absent. **No** `$RmMetadata` / `$Txf*` field invention.

### NTFS-CHKDSK-001
- Status: resolved
- Opened: 2026-11-18
- Resolved: 2026-11-18
- Question: Does Tavera need an in-driver `NtfsChkdsk` (or equivalent), and what is its contract when a guest log names it?
- Blocks: `NtfsChkdsk` slice of GitHub #3.
- Notes: No `NtfsChkdsk` symbol under `drivers/filesystems/ntfs`. Host `chkdsk` readability is **NTFS-MOUNT-001** / [`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md), not a substitute. **Deferred** until a guest log names in-driver repair. Spec: [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md).
- Resolution: **Out of scope / mislabeled.** Learn check path is user-mode [chkdsk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk), [fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty) → [autochk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/autochk) / [chkntfs](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkntfs); in-tree surface is dirty FSCTLs (+ `NtfsVerifyVolume` stub), not an `NtfsChkdsk` export. Host readability stays **NTFS-MOUNT-001**. Spec Rev.1: [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md). No implementer pass.
