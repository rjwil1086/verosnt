<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-win10-compat.md
PURPOSE:     Index of clean-room specs for a Windows 10-compatible NTFS driver
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# Windows 10–compatible NTFS driver — spec index

North star: enough clean-room specs that a later session can implement and verify an NTFS driver whose volumes and APIs behave as Windows 10 22H2 expects, including Windows mounting a Tevera OS–formatted volume cleanly.

Allowed inputs only: public Learn / openspecs pages, paths already in this tree, and guest logs copied into `tavera/specs/ntfs/sources/` (from `output/`) with their markers. Spec citations keep those URLs and `sources/` paths. Missing on-disk layouts become **probe gates**, not guessed constants. Raw-format probes and implementation stay in separate sessions ([`.cursor/rules/clean-room-probe-separation.mdc`](../../../.cursor/rules/clean-room-probe-separation.mdc)).

Suite catalog rows live in `unittests/ntfs/features.ps1` (buildenv). Names below match that catalog so later tests plug into the same inventory. This index does not edit that file.

**(Rev.1)** The compliance suite was moved to [`tavera/test-suites/ntfs/`](../../test-suites/ntfs/) (`features.ps1`, `Invoke-NtfsSuite.ps1`, `Compare-NtfsBytes.ps1`, README). `buildenv/unittests/ntfs` is a pointer stub only. Catalog names are unchanged.

Raw-format probe sources live under [`tavera/probes/ntfs/`](../../probes/ntfs/) as versioned folders (`probe-v1`, `probe-v2`, … per [`.cursor/rules/tavera-probes.mdc`](../../../.cursor/rules/tavera-probes.mdc)). Captures go under `output/ntfs/<probe-id>/` (gitignored). Specs must name the probe version that produced each capture-backed fact. Do not edit `drivers/filesystems/ntfs` in a probe session.

## Status legend

- **Sourced** — Learn and/or in-tree declarations are enough to implement or verify that slice without inventing layout.
- **Probe-gated** — a future capture-cited extension of the named spec is required before that on-disk path may be implemented.
- **Acceptance** — guest-visible pass criteria for Milestone 45.

## Inventory

| Need | Spec | Status | Catalog rows |
|------|------|--------|--------------|
| Spec map / Win10 goal | `ntfs-win10-compat.md` (this file) | Sourced | — |
| Boot, version 3.1, reserved system files | [`ntfs-volume-system-files.md`](ntfs-volume-system-files.md) | Sourced (boot/EBPB/SI 72 / reserved names); probe gates noted for AttrDef table bytes beyond in-tree table | BootSector, Version3_1, VolumeLabel, ClusterSize, Mft, LogFile, AttrDef, UpCase, Bitmap, BootFile, BadClus, Extend |
| `$MFT` / `$MFTMirr` | [`ntfs-mft-mirror.md`](ntfs-mft-mirror.md) | Sourced | Mft, MftMirror |
| `$Secure` / security | [`ntfs-secure.md`](ntfs-secure.md) | Sourced for streams/API; probe gate for any `$SDS` entry framing not already in `ntfslib` | Secure, SecurityDescriptor |
| Object IDs | [`ntfs-object-id.md`](ntfs-object-id.md) | Sourced for FSCTL buffers; probe gate for `$O` index entry value bytes if not in-tree | ObjId, ObjectIdApi |
| Reparse | [`ntfs-reparse.md`](ntfs-reparse.md) | Sourced for `REPARSE_DATA_BUFFER` / FSCTL; probe gate for `$R` index key/value if not in-tree | Reparse, ReparsePoint |
| Sparse | [`ntfs-sparse.md`](ntfs-sparse.md) | Sourced for FSCTL / attribute flag; probe gate for run-list hole encoding beyond in-tree `NTFS_SPARSE_LBO` | Sparse |
| Compression | [`ntfs-compression.md`](ntfs-compression.md) | Sourced for FSCTL + LZNT1 as documented default; probe gate for on-disk unit framing beyond in-tree driver | Compression |
| USN journal | [`ntfs-usn.md`](ntfs-usn.md) | Sourced for streams / `USN_RECORD` in-tree; probe gate if journal header beyond `$MAX`/`$J` names is required | UsnJrnl, UsnJournal |
| Read / write / remount | [`ntfs-read-write-remount.md`](ntfs-read-write-remount.md) | Sourced (API behavior) | ResidentData, NonResidentData, EndOfFile, AllocationSize, Create, Delete, Rename, Remount, … |
| Quotas (`$Q` value) | [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) | **Capture + diff** (probe-v2: LE int64 @316 threshold, @324 limit in `$Q` root) | Quota, QuotaAccounting, **QuotaDelete** (Rev.6) |
| Encryption (on-disk body) | [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md) | **Capture + diff** (probe-v6: 672-byte `$EFS`; fixed 16-byte header; Microsoft field names none) | Encryption, **EfsBody** |
| Log replay (`$LogFile` records) | [`ntfs-log-replay.md`](ntfs-log-replay.md) | **Capture + diff** (probe-v7 single-op recipe; record field names still **none**) | LogFile (presence); **LogReplay** / **LogOps** black-box dirty remount (Rev.10) |
| Log restart dirty-mount (#2) **(Rev.14)** | [`ntfs-log-restart-mount.md`](ntfs-log-restart-mount.md) | **Implementer recipe** (probe-v11 on probe-v7 dumps; empty slots → clear dirty; never apply) | (no catalog row; mount policy) |
| Tevera → Windows clean mount | [`ntfs-windows-clean-mount.md`](ntfs-windows-clean-mount.md) | **Acceptance** (test harness is a milestone follow-on) | WindowsCleanMount (to be added to catalog when the test is written) |
| Locks / page file / TxF / chkdsk **(Rev.11)** | [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md) | Locks **in-tree**; page/TxF/`NtfsChkdsk` **deferred** (GitHub #3 partial) | (no dedicated catalog row; lock via FsRtl) |
| Locks / page file / TxF / chkdsk **(Rev.12)** | [`ntfs-locks-pagefile-txf-chkdsk.md`](ntfs-locks-pagefile-txf-chkdsk.md) Rev.1 | Locks **in-tree**; **chkdsk de-scoped** (**NTFS-CHKDSK-001**); page/TxF still deferred (#3) | (no dedicated catalog row; lock via FsRtl; dirty via LogReplay) |
| Page file / TxF **(Rev.13)** | [`ntfs-pagefile.md`](ntfs-pagefile.md), [`ntfs-txf.md`](ntfs-txf.md) | **Implementer recipes** from probe-v9/v10 (`NTFS-PAGE-PROBE-DONE ok` / `NTFS-TXF-PROBE-DONE ok`) | (optional future catalog rows) |

## Milestone acceptance (not this plan’s code)

Milestone 45 is done only when every `features.ps1` row is green in `unittests/ntfs/out/ntfs31.json`, including a future `WindowsCleanMount` row, and probe-gated paths are either implemented from capture-cited specs or remain explicitly blocked without inventing layout.

**(Rev.1)** Acceptance snapshots now land under `tavera/test-suites/ntfs/out/ntfs31.json` (same schema). The old buildenv `out/` path is stale.

### Suite run 2026-11-18 **(Rev.1)**

**(Rev.1)** Windows 10 22H2 baseline (`Compare-NtfsBytes.ps1 -WindowsOnly`): **43 green / 2 red** in `out/windows-baseline-result.log`. Remaining Windows reds: `Remount` (dismount), `UsnJrnl` (path open after create). Suite agent fixes landed for QuotaAccounting (fsutil), Encryption (final path), ReparsePoint (CreateSymbolicLinkW), UsnJournal (volume FSCTL).

**(Rev.1)** Tavera `-Phase compliance`: **42 of 45 (93.3%)** in `out/ntfs31.txt`. Tavera-only reds vs Windows green:

| Row | Tavera detail | Spec / open item |
|-----|---------------|------------------|
| `Version3_1` | extended volume data missing | open `NTFS-VER-001` |
| `QuotaAccounting` | fsutil track | `ntfs-quota-on-disk.md` Rev.1; `NTFS-QUOTA-003` / track enablement |
| `Encryption` | FILE_SUPPORTS_ENCRYPTION clear | `ntfs-encrypted-file-body.md` Rev.1; `NTFS-EFS-002` still for `$EFS` bytes |

### Suite run 2026-11-18 (Windows 100%) **(Rev.2)**

**(Rev.2)** Windows baseline re-run (`Compare-NtfsBytes.ps1 -WindowsOnly -PreferWhpx`): **45 green / 0 red** in `out/windows-baseline-result.log` (`NTFS-AGENT-DONE`). Harness fixes in `tavera/test-suites/ntfs/agent/ntfs_agent.c`: `Remount` leaves the test volume cwd, retries `FSCTL_LOCK_VOLUME`, allows `FSCTL_DISMOUNT_VOLUME` without a lock ([FSCTL_LOCK_VOLUME](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume) / [FSCTL_DISMOUNT_VOLUME](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_dismount_volume)); `UsnJrnl` uses `FSCTL_QUERY_USN_JOURNAL` after a successful `FSCTL_CREATE_USN_JOURNAL` instead of opening `$Extend\$UsnJrnl`. `-PreferWhpx` is suite-only and falls back to TCG.

**(Rev.2)** Tavera score above is unchanged in this revision; implementer still owns the three Tavera-only reds.

### Suite run 2026-11-18 (implementer pass) **(Rev.3)**

**(Rev.3)** Tavera `-Phase compliance` after implementer slice: **43 of 45 (95.6%)** in `out/ntfs31.txt` / `out/share-compliance/result.log`.

| Row | Status | Detail | Open item |
|-----|--------|--------|-----------|
| `Version3_1` | **green** | Extended volume data filled | `NTFS-VER-001` **resolved** |
| `QuotaAccounting` | **red** | `fsutil modify` (track now green) | `NTFS-QUOTA-003` still open — needs probe-v3 growth/`-2` |
| `Encryption` | **red** | `EncryptFile finalpath` (flag clear gone) | `NTFS-EFS-003` **resolved**; catalog green blocked on `NTFS-EFS-002` |

**(Rev.3)** No further implementer work on the two remaining reds from existing cites alone. Next productive step is a **probe session**, not a driver debug loop.

### probe-v3 quota growth **(Rev.4)**

**(Rev.4)** [`tavera/probes/ntfs/probe-v3/`](../../probes/ntfs/probe-v3/) answered `$Q` **growth** for `QuotaAccounting`: track alone 208→384 with -1/-1 at 316/324; modify writes threshold/limit only. Spec recipe in [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.3. **QuotaLimit -2** delete still open (`NTFS-QUOTA-003`). `Encryption` still blocked on `NTFS-EFS-002`. Implementer can clear `fsutil modify` from that recipe without a debug loop; do not invent `-2`.

### probe-v4 / probe-v5 **(Rev.5)**

**(Rev.5)** [`tavera/probes/ntfs/probe-v4/`](../../probes/ntfs/probe-v4/): `NtSetQuotaInformationFile` QuotaLimit=-2 succeeds; `$Q` clears thr/lim to -1 (key 0x102→0x103). **NTFS-QUOTA-003 resolved.** [`tavera/probes/ntfs/probe-v5/`](../../probes/ntfs/probe-v5/): dirty remount + known-file readback **ok** on Windows — suite `LogReplay` black-box is ready; **NTFS-LOG-001** field layout still open. **Do not add suite rows in this probe session** (deferred).

### Suite rows QuotaDelete / LogReplay **(Rev.6)**

**(Rev.6)** Harness session added catalog rows `QuotaDelete` and `LogReplay` in [`tavera/test-suites/ntfs/features.ps1`](../../test-suites/ntfs/features.ps1) and agent marks in [`ntfs_agent.c`](../../test-suites/ntfs/agent/ntfs_agent.c). Specs: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.5; [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.2. Windows baseline must stay 100% on the expanded catalog before blaming Tavera.

### Windows baseline 47/47 **(Rev.7)**

**(Rev.7)** `Compare-NtfsBytes.ps1 -WindowsOnly -PreferWhpx`: **47 green / 0 red** in `out/share-windows/result.log` (`NTFS-AGENT-DONE`). `QuotaDelete` seeds/−2 LOCAL SERVICE (interactive SID hit `STATUS_CANNOT_DELETE` with suite files; missing SID after −2 accepted). `LogReplay` dirty remount + readback green. Specs: [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.6–7.

### QuotaDelete implementer handoff **(Rev.8)**

**(Rev.8)** Suite `QuotaDelete` is green on Windows. Tavera catalog green is gated on open item **NTFS-QUOTA-004** and the consolidated recipe in [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) Rev.8 (clear thr/lim to −1 per probe-v4; no further probe for that path). Do not edit `ntfs.sys` from a probe/harness session.

### EFS probe-v6 + EfsBody **(Rev.9)**

**(Rev.9)** [`tavera/probes/ntfs/probe-v6/`](../../probes/ntfs/probe-v6/): `$EFS` always 672 with fixed 16-byte header; **NTFS-EFS-002 resolved** (structural; Microsoft field names **none**). Suite catalog adds **`EfsBody`**. Tavera `Encryption` / `EfsBody` gated on **NTFS-EFS-004**. Spec: [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md) Rev.3–4.

### Log probe-v7 + LogOps **(Rev.10)**

**(Rev.10)** [`tavera/probes/ntfs/probe-v7/`](../../probes/ntfs/probe-v7/): single-op `$LogFile` stages F/C/W/N/X/T; structural recipe cited; Microsoft field names **none**. Suite catalog adds **`LogOps`** (create+write+rename+dirty remount+readback). **NTFS-LOG-001** stays open for named layout; **NTFS-LOG-002** for Win↔Tavera differentials after recipe apply. Spec: [`ntfs-log-replay.md`](ntfs-log-replay.md) Rev.3–4.

## Probe sessions still required

Done so far:

- [`probe-v1`](../../probes/ntfs/probe-v1/) — format, `$LogFile` dump, fsutil quota → `$Q`, EncryptFile → `$DATA`/`$EFS`.
- [`probe-v2`](../../probes/ntfs/probe-v2/) — A/B differential: quota threshold/limit at `$Q` root offsets 316/324; `$EFS` 16-byte same-boot prefix; log traffic volume diffs without named record fields.

Still open (see [`open-items.md`](open-items.md)):

1. `$LogFile` restart / log-record field layout → `ntfs-log-replay.md` (`NTFS-LOG-001`); apply recipe + Win↔Tavera diffs → **NTFS-LOG-002**.
2. **(Rev.9)** `$EFS` structural recipe cited (probe-v6); **NTFS-EFS-002 resolved**. Implementer **NTFS-EFS-004** for suite `Encryption` / `EfsBody`.
3. **(Rev.5)** `$Q` growth + −2 cited (probe-v3/v4); **NTFS-QUOTA-003 resolved**. `NTFS-VER-001` and `NTFS-EFS-003` resolved. **(Rev.10)** **NTFS-LOG-001** field names still none (probe-v7 recipe cited). **NTFS-QUOTA-004** for suite `QuotaDelete` on Tavera.
4. Any probe gate called out inside a “Sourced” topic spec when an implementer hits a missing offset.

Each probe session stores captures under `output/ntfs/<probe-id>/`, cites **that probe path** and the capture in the spec, and does not edit `ntfs.sys`. A later change that needs a different capture adds `probe-v3` (etc.) and leaves older versions in place unless an unused probe is retired per `tavera-probes.mdc`. Implementation is a later session that consumes only the updated specs and markers; gaps go in [`open-items.md`](open-items.md).

## Revision log

- **Rev.1** (2026-11-18): Suite moved to `tavera/test-suites/ntfs`; Windows/Tavera compliance scores; Tavera-only reds listed.
- **Rev.2** (2026-11-18): Windows baseline 45/45 after Remount/UsnJrnl harness fixes; `-PreferWhpx` note.
- **Rev.3** (2026-11-18): Implementer pass → 43/45; resolve VER-001 / EFS-003; remaining reds need probe-v3 / EFS-002.
- **Rev.4** (2026-11-18): probe-v3 `$Q` growth recipe for QuotaAccounting; -2 still open.
- **Rev.5** (2026-11-18): probe-v4 −2 / probe-v5 dirty remount; QUOTA-003 resolved; suite rows still deferred.
- **Rev.6** (2026-11-18): suite `QuotaDelete` + `LogReplay` catalog rows added from probe cites.
- **Rev.7** (2026-11-18): Windows baseline 47/47 with new rows green.
- **Rev.8** (2026-11-18): QuotaDelete implementer gated on **NTFS-QUOTA-004** + quota Rev.8.
- **Rev.9** (2026-11-18): probe-v6 EFS recipe; EFS-002 resolved; suite `EfsBody`; EFS-004 open.
- **Rev.10** (2026-11-18): probe-v7 log-iso recipe; field names none; suite `LogOps`; LOG-002 open.
- **Rev.11** (2026-11-18): #3 partial — locks in-tree; page/TxF/chkdsk deferred (`ntfs-locks-pagefile-txf-chkdsk.md`).
- **Rev.12** (2026-11-18): #3 chkdsk slice closed / mislabeled (`ntfs-locks-pagefile-txf-chkdsk.md` Rev.1; **NTFS-CHKDSK-001**); page/TxF remain.
- **Rev.13** (2026-11-18): probe-v9 pagefile + probe-v10 TxF; **NTFS-PAGE-001** / **NTFS-TXF-001** resolved for implementer.
- **Rev.14** (2026-11-18): probe-v11 restart-page dirty-mount for GitHub #2; **NTFS-LOG-003** resolved (`ntfs-log-restart-mount.md`).
