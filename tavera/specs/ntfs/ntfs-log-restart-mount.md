<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-log-restart-mount.md
PURPOSE:     Dirty-mount policy using observed $LogFile restart-page tags (GitHub #2)
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS log restart page — dirty mount (no record apply)

## Header and provenance

Component: GitHub #2 — mount a dirty volume whose `$LogFile` has Windows-written pages; recognize restart pages; empty client restart slots → clear dirty / RW; non-empty → stay RO; **never apply** outstanding log records.  
Version: 1. Author: Ryan Wilson. Date: 2026-11-18.  
Only allowed sources below were used. Probe/spec session only — does not edit `drivers/filesystems/ntfs`.

## Sources

| Source | Date read |
|--------|-----------|
| [MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) — `$LogFile` | 2026-11-18 |
| [How NTFS Works](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)) — recovery uses the log; this spec does **not** implement redo/undo apply | 2026-11-18 |
| [Fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty) | 2026-11-18 |
| In-tree `NTFS_VOLUME_DIRTY` / `VCB_VOLUME_READ_ONLY` (`drivers/filesystems/ntfs/ntfs.h`); no-op `NtfsReplayLog` (`ntfs3x.c`) | 2026-11-18 |
| In-tree multi-sector fixup pattern used by host dump tools (`tavera/probes/ntfs/probe-v7/host/ntfs_raw_dump.c`) | 2026-11-18 |
| Captures `output/ntfs/probe-v7/<stage>/captures/logfile_data.bin` + `T\guest-serial.log` (`NTFS-LOG-ISO-DONE ok`) from [`tavera/probes/ntfs/probe-v7`](../../probes/ntfs/probe-v7) | 2026-11-18 |
| Report `output/ntfs/probe-v11/restart-report.txt` (`NTFS-LOG-RESTART-REPORT` … `END`) from [`tavera/probes/ntfs/probe-v11`](../../probes/ntfs/probe-v11) — probe method | 2026-11-18 |

## Purpose and scope

In scope: **safe dirty-mount policy** for #2 so the implementer can leave RO or clear dirty **without** parsing or applying `RCRD` bodies.

Out of scope: inventing Microsoft restart/log-record field names (**NTFS-LOG-001** stays open for that); redo/undo apply of committed/uncommitted transactions; matching every byte of `$LogFile` on write (**NTFS-LOG-002**).

## Interfaces (probe-local labels)

All offsets below are **probe-local** reads from the first restart-tagged page of `logfile_data.bin`. They are **not** Microsoft names.

| Label | Observation (all probe-v7 stages F/C/W/N/X/T) |
|-------|-----------------------------------------------|
| `PAGE_TAG_RSTR` | Bytes `52 53 54 52` at page-aligned offsets **0** and **4096** (exactly two such pages in the first 1 MiB dump) |
| `PAGE_TAG_RCRD` | Bytes `52 43 52 44` at page-aligned offsets starting at **8192** |
| `PAGE_SIZE_U32_AT_10` | LE `u32` at **+0x10** (and again at **+0x14**) = **`0x1000`** (4096) |
| `U16_AT_4` / `U16_AT_6` | `0x001E` / `0x0009` — consistent with the in-tree multi-sector header / fixup cadence already used on 512-byte sectors (9 = 8 sectors + USA) |
| `U64_AT_8` | Always **0** |
| `SLOT_U64_AT_60` / `SLOT_U64_AT_68` | Always **0** (including dirty-remount stage **T**) |
| `UTF16_AT_90` | UTF-16LE **`NTFS`** (8 bytes) |
| Changing counters (not required for #2 policy) | LE `u64` at **+0x30**, LE `u32` at **+0x58**, LE `u16` at **+0x1E** change across stages |

Page size for scanning = value of `PAGE_SIZE_U32_AT_10` (4096), not a guessed constant.

## Behavior (implementer acceptance for #2)

On mount, when the volume dirty bit is set (`NTFS_VOLUME_DIRTY` / [fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty)):

1. **Recognize** a Windows-written restart page: first `$LogFile` data page (and the page at +4096) begins with `PAGE_TAG_RSTR` and `PAGE_SIZE_U32_AT_10 == 0x1000`.
2. Read **`SLOT_U64_AT_60`** and **`SLOT_U64_AT_68`** (LE) from the first `RSTR` page.
3. If **both slots are 0**: clear the dirty bit, mount **read-write**, and **do not** parse or apply any `RCRD` page bodies. (Matches probe-v7 stage **T**: dirty set → remount NTFS → readback ok while both slots stay 0.)
4. If **either slot is non-zero**: keep **`VCB_VOLUME_READ_ONLY`** (reads ok, writes not), and **still do not** apply `RCRD` bodies. Halfway apply would corrupt the volume (issue #2).
5. `NtfsReplayLog` must **not** become a record applicator in this slice.

## Observed behavior

From `output/ntfs/probe-v11/restart-report.txt` (`NTFS-LOG-RESTART-REPORT` … `END`), inputs = probe-v7 dumps:

| Fact | Evidence |
|------|----------|
| `RSTR` at 0 and 4096; `RCRD` from 8192 | OBSERVED lines + per-stage TAG\* |
| Page size field 0x1000 | `U32_AT_10` all stages |
| Slots +0x60 / +0x68 always 0 | `U64_AT_60` / `U64_AT_68` all stages; `NONZERO_SAMPLE_AT_60 none_in_probe_v7` |
| Dirty remount RW success with slots 0 | `GUEST_T` DIRTY_SET / REMOUNT / READBACK / DONE ok |

## Compatibility notes

- Suite `LogReplay` / `LogOps` remain black-box dirty remount tests; they do not require this restart recognition.
- Full `$LogFile` record layout naming remains **NTFS-LOG-001**.
- Non-zero `SLOT_U64_AT_60/68` was **not** observed in probe-v7; the RO branch is required by #2 and is the safe default when a later capture shows non-zero.

## Test plan

1. Host: run [`tavera/probes/ntfs/probe-v11/Invoke-NtfsLogRestartProbe.ps1`](../../probes/ntfs/probe-v11/Invoke-NtfsLogRestartProbe.ps1); require `NTFS-LOG-RESTART-REPORT-END` and `SLOT_U64_AT_60_AND_68` = 0.
2. Implementer: on Tavera, with dirty set and slots 0 after a Windows- or self-written log that carries `RSTR` pages as above — remount RW and clear dirty without applying records.
3. If a future probe produces non-zero slots, expect RO until cleared by a documented path; still no apply.

## Open questions

- A captured non-zero `SLOT_U64_AT_60/68` sample (crash / mid-transaction) — optional follow-on probe; RO policy already stated.
- Microsoft names for these slots — still **none** (**NTFS-LOG-001**).

## Review log

- 2026-11-18: Spec agent — probe-v11 analysis of probe-v7 dumps + Learn/in-tree; no driver edit.

## Revision log

- **Rev.0** (2026-11-18): Initial #2 dirty-mount recipe from restart-report.
