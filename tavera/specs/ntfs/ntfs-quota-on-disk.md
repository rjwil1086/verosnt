<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-quota-on-disk.md
PURPOSE:     Documented NTFS quota query/set and the missing $Q index value
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS quota on-disk value

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists `$Extend\$Quota` as quota information.

[MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) names `$Q` as the quota index stream (`$Extend\$Quota:$Q:$INDEX_ALLOCATION`).

[MS-FSCC 2.4.41](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/acdc0738-ba3c-47a1-b11a-72e22d831c57) defines `FILE_QUOTA_INFORMATION`. On set, `ChangeTime` and `QuotaUsed` are ignored. A threshold or limit of `-1` means none is set. A limit of `-2` deletes the entry.

[MS-FSA: Server Requests Querying Quota Information](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/b81634b1-ce07-4770-ab21-af87f9f53515) gives the status rules for that query. [MS-FSA Appendix A](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4e3695bd-7574-4f24-a223-b4679c065b63) notes 198 and 200 name the stream `$Extend\$Quota:$Q:$INDEX_ALLOCATION`.

[fsutil quota](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-quota) documents `track` and `modify` on an NTFS volume path.

[MS-FSCC FileFsControlInformation](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/e5a70738-7ee4-46d9-a5f7-6644daa49a51) documents `FILE_VC_QUOTA_TRACK` / `FILE_VC_QUOTA_ENFORCE` and notes that a client attempt to set those flags is ignored.

In-tree:

- `FILE_QUOTA_INFORMATION` is declared in `sdk/include/ndk/iotypes.h` (query/set buffer for the FSCTL path).
- `NtfsSetQuota` in `drivers/filesystems/ntfs/ntfs3x.c` returns `STATUS_NOT_IMPLEMENTED`. `FSCTL_SET_QUOTA` in `drivers/filesystems/ntfs/fsctl.c` calls it.

None of the pages above define the bytes stored in the `$Q` index entry.

## Compatibility outcomes

- Query/set buffers follow `FILE_QUOTA_INFORMATION` and the MS-FSA status rules once the on-disk value is known.
- Enabling tracking and writing a per-user threshold/limit on Windows 10 22H2 succeeds via `fsutil quota track` / `fsutil quota modify` (see observed behavior). A raw `FSCTL_SET_QUOTA` on the volume handle still returned `ERROR_INVALID_FUNCTION` in probe-v1 after those commands.

## Observed behavior (Windows 10 22H2, probe-v1)

Probe: [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/). Serial `output/ntfs/probe-v1/guest-serial.log` contains `Quota fsutil track ok`, `Quota fsutil modify ok`, `Quota ok`, and `NTFS-RAW-PROBE-DONE ok`. Manifest `output/ntfs/probe-v1/captures/manifest.txt` ends with `NTFS-RAW-DUMP-END`.

Sequence that grew `$Q` on disk:

1. `fsutil.exe quota track T:` (Learn fsutil quota).
2. `fsutil.exe quota modify T: 4096 8192 <GetUserNameW>` (threshold 4096, limit 8192, current user).

Resident `$Q` index root sizes observed across probe-v1 images (same host dump path):

| After | Resident `$Q` index root bytes | Capture |
|-------|--------------------------------|---------|
| Format only (earlier image) | 208 | prior dump |
| `fsutil quota track` only (earlier image) | 288 | prior dump |
| `track` + `modify` (final image) | **384** | `captures/quota_q_index_root.bin` |

Also captured: `$Quota` MFT record `captures/mft_record_24_quota.bin` (record 24).

`DeviceIoControl(..., FSCTL_SET_QUOTA=0x00098080, ...)` on `\\.\T:` after the fsutil steps still returned Win32 `ERROR_INVALID_FUNCTION` (1) in the same serial log (`Quota FSCTL_SET_QUOTA optional e=1`). That does not undo the on-disk `$Q` growth from fsutil.

### Differential (probe-v2)

[`tavera/probes/ntfs/probe-v2/`](../../probes/ntfs/probe-v2/) variants (serial `NTFS-DIFF-PROBE-DONE ok` under `output/ntfs/probe-v2/A/` and `.../B/`):

| Variant | `fsutil quota modify` threshold / limit | Capture |
|---------|----------------------------------------|---------|
| A | 4096 / 8192 | `output/ntfs/probe-v2/A/captures/quota_q_index_root.bin` (384 bytes) |
| B | 8192 / 16384 | `output/ntfs/probe-v2/B/captures/quota_q_index_root.bin` (384 bytes) |

Host report `output/ntfs/probe-v2/diff-report.txt` (`NTFS-DIFF-REPORT` … `NTFS-DIFF-REPORT-END`) shows nine differing runs in that resident `$Q` index-root blob. Matching the known inputs:

- Little-endian **int64 at offset 316** equals the modify **threshold** (A=`4096`, B=`8192`).
- Little-endian **int64 at offset 324** equals the modify **limit** (A=`8192`, B=`16384`).

Other differing offsets (e.g. 68, 133, 140, 221, 228, 301, 308) changed between boots but were **not** fixed to the threshold/limit inputs; they are not named here.

No `NtfsSetQuota` implementation in this probe session.

### Implementer slice and suite results **(Rev.1)**

**(Rev.1)** The working tree after the implementer pass wires `NtfsSetQuota` through Rust `ntfs_quota_write_threshold_limit` in `drivers/filesystems/ntfs/src/lib.rs`, writing LE int64 at offsets **316** / **324** when the resident `$Q` root length is at least 332. The older sentence above that `NtfsSetQuota` returns `STATUS_NOT_IMPLEMENTED` remains as the pre-implementer in-tree note; do not treat it as current code.

**(Rev.1)** Limits of that slice (still open for later specs / `NTFS-QUOTA-003`): no SID/owner matching (every set overwrites the same absolute offsets); `QuotaLimit == -2` returns `STATUS_NOT_IMPLEMENTED`; non-resident `$Q` returns `STATUS_NOT_IMPLEMENTED`; short root returns `STATUS_BUFFER_TOO_SMALL`.

**(Rev.1)** Compliance suite (moved to [`tavera/test-suites/ntfs/`](../../test-suites/ntfs/)):

| Guest | `QuotaAccounting` row | Marker / log |
|-------|----------------------|--------------|
| Windows 10 22H2 | **green** after suite agent uses `fsutil quota track` / `modify` (not raw `FSCTL_SET_QUOTA` on a file) | `tavera/test-suites/ntfs/out/windows-baseline-result.log` |
| Tavera live CD | **red** `fsutil track` | `tavera/test-suites/ntfs/out/share-compliance/result.log`; snapshot `out/ntfs31.txt` reports 42 of 45 |

**(Rev.1)** So Windows proves the suite’s fsutil path; Tavera still fails enabling quota tracking. Implementers must make `fsutil quota track` succeed on a Tavera NTFS volume before relying on the 316/324 overwrite path for catalog green. Growing `$Q` from format size and `-2` delete remain `NTFS-QUOTA-003`.

### Implementer track + modify status **(Rev.2)**

**(Rev.2)** After the suite-reds implementer pass: Tavera `fsutil quota track` is **green** (`FileFsControlInformation` / `FILE_VC_QUOTA_TRACK` on the VCB + in-tree `fsutil quota`). Suite `QuotaAccounting` is still **red** on `fsutil modify` (`out/share-compliance/result.log`, 43 of 45). Cause: resident `$Q` root is shorter than 332, so `ntfs_quota_write_threshold_limit` returns `STATUS_BUFFER_TOO_SMALL`. The 316/324 overwrite path is correct only when the root is already large enough (probe-v2).

**(Rev.2)** Sizes already recorded above (format 208 / track 288 / modify 384) are **not** enough to implement growth: only the 384-byte post-modify blobs are retained under `output/ntfs/probe-v1/` and `probe-v2/`. Spec agent will not invent padding or entry layout. **Next:** probe-v3 dumps `$Q` at format, after `track`, after `modify` (and optionally after limit `-2`) so growth and delete encoding can be cited. Until then, leave `NTFS-QUOTA-003` open and do not start an implementer debug loop on `fsutil modify`.

### Differential (probe-v3) **(Rev.3)**

**(Rev.3)** Probe: [`tavera/probes/ntfs/probe-v3/`](../../probes/ntfs/probe-v3/). Stages F / T / M / D on Windows 10 22H2; host dump + `output/ntfs/probe-v3/growth-report.txt` (`NTFS-QUOTA-GROWTH-REPORT` … `NTFS-QUOTA-GROWTH-REPORT-END`). Serial each stage ends `NTFS-QUOTA-GROWTH-DONE ok`.

| Stage | Guest steps | Resident `$Q` bytes | LE int64 @316 | LE int64 @324 | Capture |
|-------|-------------|---------------------|---------------|---------------|---------|
| F | format only | **208** | (absent) | (absent) | `F/captures/quota_q_index_root.bin` |
| T | + `fsutil quota track` | **384** | **-1** | **-1** | `T/captures/quota_q_index_root.bin` |
| M | + `fsutil quota modify … 4096 8192 <user>` | **384** | **4096** | **8192** | `M/captures/quota_q_index_root.bin` |
| D | + `FSCTL_SET_QUOTA` QuotaLimit=-2 | **384** | 4096 | 8192 | `D/captures/…` (delete **did not** apply; serial `e=1`) |

**(Rev.3)** Correction to the earlier “track → 288” size note: on this probe-v3 guest, **track alone** grows the root to **384** (same length as after modify). Modify does **not** grow further; it only overwrites the threshold/limit fields (and a few ChangeTime-like DWORD runs).

**(Rev.3)** Index walk of the resident `$Q` value (same entry header shape as in-tree `MkViewRootN` / view-index entries in `sdk/lib/fslib/ntfslib/files.c`):

| Stage | Entries (key ULONG → data length) | End marker |
|-------|-----------------------------------|------------|
| F | key **1** → 48; key **0x100** → 64 | @192 |
| T/M | key **1** → 48; key **0x100** → 64; key **0x101** → 60; key **0x102** → 76 | @368 |

**(Rev.3)** Index node header at value offsets 0x14 / 0x18 (IndexLength / AllocatedSize): format `0xC0` → after track `0x170`.

**(Rev.3)** Quota **control-entry data** layout inside each `$Q` index entry’s data (matches the format-time `Qce` writer in `sdk/lib/fslib/ntfslib/files.c` and MS-FSCC “none” = -1):

| Data off | Field (this capture) |
|----------|----------------------|
| +0x00 | ULONG version (= 2 after track/modify entries) |
| +0x04 | ULONG flags |
| +0x08 | ULONGLONG bytes used |
| +0x10 | ChangeTime |
| +0x18 | QuotaThreshold (LE int64); **-1** after track; **4096** after modify on key **0x102** |
| +0x20 | QuotaLimit (LE int64); **-1** after track; **8192** after modify on key **0x102** |
| +0x28 | Exceeded time |
| +0x30 | SID (variable; entry data length grows with SID) |

**(Rev.3)** Absolute offsets **316** / **324** (probe-v2) are data+0x18 / data+0x20 of the key-**0x102** entry when that entry starts at root offset **272** (post-track Windows layout). Prefer matching by **owner-id key + relative data offsets** when inserting entries; the absolute pair remains valid for the 384-byte post-track blob.

**(Rev.3)** Stage D: `FSCTL_SET_QUOTA` with QuotaLimit **-2** returned Win32 **ERROR_INVALID_FUNCTION (1)** (same optional failure as probe-v1). **Delete-entry on-disk encoding is still unknown** — leave that half of `NTFS-QUOTA-003` open.

### Implementer recipe for suite `QuotaAccounting` **(Rev.3)**

**(Rev.3)** Real path (no padding invent):

1. **`fsutil quota track` / `FILE_VC_QUOTA_TRACK`:** already accepted on the VCB. Also grow resident `$Extend\$Quota:$Q` from the format-time two-entry root to a four-entry root like stage T: keep keys **1** and **0x100**; append keys **0x101** and **0x102** with control-entry data (threshold=limit=**-1**), bump IndexLength/AllocatedSize to **0x170**, move the end marker. SID bytes must be the live volume’s owner SIDs (do **not** copy SID bytes from the probe capture).
2. **`fsutil quota modify` / set quota:** locate the `$Q` entry for the target owner (on Windows after track, key **0x102** held the interactive user); write LE int64 threshold/limit at data **+0x18** / **+0x20** (same fields as probe-v2’s 316/324 on that layout). Existing Rust helper may keep writing absolute 316/324 **only** when the root is already the post-track 384-byte shape.
3. **Do not** implement QuotaLimit **-2** delete until a capture shows the on-disk change (`NTFS-QUOTA-003` delete half remains open).

### Differential (probe-v4) QuotaLimit −2 **(Rev.4)**

**(Rev.4)** Probe: [`tavera/probes/ntfs/probe-v4/`](../../probes/ntfs/probe-v4/). Stages **M** (track+modify 4096/8192) and **Q** (same then `NtSetQuotaInformationFile` with `QuotaLimit=-2`, threshold −1). Serial ends `NTFS-QUOTA-DELETE-DONE ok`. Host report `output/ntfs/probe-v4/delete-report.txt` (`NTFS-QUOTA-DELETE-REPORT` … `END`).

| Stage | NtSet STATUS | Resident `$Q` | Keys | data+0x18 / +0x20 on last owner entry |
|-------|--------------|---------------|------|----------------------------------------|
| M | (not called) | 384 | 1, 0x100, 0x101, **0x102** | **4096 / 8192** |
| Q | **0x00000000** (serial `NtSetStatus`) | 384 | 1, 0x100, 0x101, **0x103** | **-1 / -1** |

**(Rev.4)** Captures: `M/captures/quota_q_index_root.bin`, `Q/captures/quota_q_index_root.bin`. The SID bytes in the last entry’s data (+0x30) are the **same** before and after. Index entry count stays four; Windows does **not** remove the leaf in this observation. Owner id key **0x102 → 0x103**; threshold/limit cleared to MS-FSCC “none” (**-1**). `FSCTL_SET_QUOTA` remains the failing path (probe-v3); use **`NtSetQuotaInformationFile`** ([Learn](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetquotainformationfile)).

**(Rev.4)** Implementer recipe for −2: on `QuotaLimit == -2`, apply the live SID’s `$Q` control entry so data **+0x18/+0x20** become **-1** (and match owner-id renumbering if the volume already uses sequential owner ids). Do not invent a missing-entry encoding beyond this capture. Suite `QuotaDelete` (later) can assert NtSet success + post-state limits **-1/-1** (or query) without parsing `$Q` on the host.

### Suite row QuotaDelete **(Rev.5)**

**(Rev.5)** Catalog row `QuotaDelete` is in [`tavera/test-suites/ntfs/features.ps1`](../../test-suites/ntfs/features.ps1). Agent path ([`ntfs_agent.c`](../../test-suites/ntfs/agent/ntfs_agent.c)): `fsutil quota track` + `modify 4096 8192 <user>`, then `NtSetQuotaInformationFile` with `QuotaLimit=-2`, then `NtQueryQuotaInformationFile` and require the live SID’s threshold/limit both **-1**. Cites probe-v4 / Rev.4 above; does not dump `$Q`.

### Suite QuotaDelete SID choice **(Rev.6)**

**(Rev.6)** Windows baseline after full Compliance (`out/share-windows/result.log`): `NtSetQuotaInformationFile` with `QuotaLimit=-2` for the **interactive user** (who owns suite-created files) returned **`STATUS_CANNOT_DELETE` (0xC0000121)**. Probe-v4 succeeded because the volume had no user files before −2. Suite agent now seeds thr/lim for **LOCAL SERVICE** (fallback Guest), then −2 that SID and queries −1/−1. Still cites probe-v4 on-disk clear-to−1 behavior; does not invent `$Q` layout.

### Suite QuotaDelete post-state **(Rev.7)**

**(Rev.7)** After seeding LOCAL SERVICE and a successful −2, `NtQueryQuotaInformationFile` did not return that SID (`QuotaDelete red SID missing after delete`). That matches MS-FSCC “delete the entry” more closely than probe-v4’s keep-with−1 path for the interactive SID. Suite green if −2 succeeds and, when the SID is still listed, thr/lim are both **−1**; a missing SID after −2 is also green.

### Implementer recipe for suite `QuotaDelete` **(Rev.8)**

**(Rev.8)** Consolidated handoff for an implementer session (no new probe). Cites Rev.4–7 and [`tavera/probes/ntfs/probe-v4/`](../../probes/ntfs/probe-v4/). Work item: **NTFS-QUOTA-004**.

- **API:** Handle `QuotaLimit == -2` on the volume quota set path that backs `NtSetQuotaInformationFile` ([Learn](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetquotainformationfile); [MS-FSCC 2.4.41](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/acdc0738-ba3c-47a1-b11a-72e22d831c57)). Do **not** rely on `FSCTL_SET_QUOTA` (probe-v3 stage D returned Win32 error 1).
- **On-disk (probe-v4):** For the target SID’s `$Q` control entry, set data **+0x18 / +0x20** to **−1 / −1**; keep the SID leaf; owner key may renumber (**0x102 → 0x103**). Captures under `output/ntfs/probe-v4/` and the Rev.4 table above.
- **Status quirk (suite):** Interactive SID with suite-owned files may get **`STATUS_CANNOT_DELETE` (0xC0000121)**; the suite seeds/−2 **LOCAL SERVICE** instead. Returning that status when delete cannot proceed is allowed. Do **not** invent a second leaf-removal encoding unless a capture cites it.
- **Query post-state (suite):** After successful −2, suite green if the SID is gone **or** thr/lim are both **−1** ([`ntfs_agent.c`](../../test-suites/ntfs/agent/ntfs_agent.c) `ProbeQuotaDelete`).
- **Supersedes Rev.3 bullet 3:** −2 is no longer blocked on a missing capture; **NTFS-QUOTA-003** stays resolved. Remaining catalog work is **NTFS-QUOTA-004**.

## Non-goals

- Guessing `$Q` value layout from memory or forbidden symbol dumps.
- Requiring Microsoft’s quota UI beyond the documented fsutil / FSCTL contract.

## Revision log

- **Rev.1** (2026-11-18): Documented implementer 316/324 overwrite limits; suite relocation; Windows vs Tavera `QuotaAccounting` results.
- **Rev.2** (2026-11-18): Track green / modify red; probe-v3 required for `$Q` growth; no invent.
- **Rev.3** (2026-11-18): probe-v3 F/T/M/D sizes, entry keys, control-entry relative layout, implementer track-growth recipe; -2 still open.
- **Rev.4** (2026-11-18): probe-v4 NtSet −2 success; on-disk clears thr/lim to −1, key 0x102→0x103; entry kept.
- **Rev.5** (2026-11-18): suite catalog `QuotaDelete` wired to NtSet −2 + query −1/−1.
- **Rev.6** (2026-11-18): suite targets LOCAL SERVICE for −2 after interactive SID got STATUS_CANNOT_DELETE.
- **Rev.7** (2026-11-18): missing SID after −2 accepted as green (delete); −1/−1 if still present.
- **Rev.8** (2026-11-18): consolidated implementer QuotaDelete recipe; handoff **NTFS-QUOTA-004**.
- **Rev.9** (2026-11-18): set-quota access / local CTL function; **NTFS-QUOTA-005**.

### Set-quota access and local CTL **(Rev.9)**

**(Rev.9)** Local macro in `drivers/filesystems/ntfs/fsctl.c`: `FSCTL_SET_QUOTA` = `CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_BUFFERED, FILE_WRITE_DATA)`. `NtfsSetQuota` has no privilege check beyond what that CTL access mask implies.

**(Rev.9)** Learn (read 2026-11-18): [Managing Privileges in a File System](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/privileges) — **SeManageVolumePrivilege** for volume-level FSCTL policy; check with `SeSinglePrivilegeCheck` / `SePrivilegeCheck`. [FILE_QUOTA_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_quota_information) — FILE_WRITE_DATA on the volume required to set quota via `IRP_MJ_SET_QUOTA`.

**(Rev.9)** Acceptance (**NTFS-QUOTA-005**): require the caller to hold **and enable** SeManageVolumePrivilege before updating `$Quota` through this FSCTL path; choose an **unused** CTL function number in this driver’s CTL space (do not invent a Microsoft FSCTL number to match).
