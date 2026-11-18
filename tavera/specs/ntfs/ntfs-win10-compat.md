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

Allowed inputs only: public Learn / openspecs pages, paths already in this tree, and guest logs under `output/` that still contain their markers. Missing on-disk layouts become **probe gates**, not guessed constants. Raw-format probes and implementation stay in separate sessions ([`.cursor/rules/clean-room-probe-separation.mdc`](../../../.cursor/rules/clean-room-probe-separation.mdc)).

Suite catalog rows live in `unittests/ntfs/features.ps1` (buildenv). Names below match that catalog so later tests plug into the same inventory. This index does not edit that file.

Raw-format probe sources live under [`tavera/probes/ntfs/`](../../probes/ntfs/) as versioned folders (`probe-v1`, `probe-v2`, … per [`.cursor/rules/tavera-probes.mdc`](../../../.cursor/rules/tavera-probes.mdc)). The current raw probe is [`probe-v1`](../../probes/ntfs/probe-v1/). Captures go under `output/ntfs/probe-v1/` (gitignored). Specs must name the probe version that produced each capture-backed fact. Do not edit `drivers/filesystems/ntfs` in a probe session.

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
| Quotas (`$Q` value) | [`ntfs-quota-on-disk.md`](ntfs-quota-on-disk.md) | **Probe-gated** | Quota, QuotaAccounting |
| Encryption (on-disk body) | [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md) | **Probe-gated** | Encryption |
| Log replay (`$LogFile` records) | [`ntfs-log-replay.md`](ntfs-log-replay.md) | **Probe-gated** | LogFile (presence today; replay correctness gated) |
| Tevera → Windows clean mount | [`ntfs-windows-clean-mount.md`](ntfs-windows-clean-mount.md) | **Acceptance** (test harness is a milestone follow-on) | WindowsCleanMount (to be added to catalog when the test is written) |

## Milestone acceptance (not this plan’s code)

Milestone 45 is done only when every `features.ps1` row is green in `unittests/ntfs/out/ntfs31.json`, including a future `WindowsCleanMount` row, and probe-gated paths are either implemented from capture-cited specs or remain explicitly blocked without inventing layout.

## Probe sessions still required

Run [`tavera/probes/ntfs/probe-v1/Invoke-NtfsRawProbe.ps1`](../../probes/ntfs/probe-v1/Invoke-NtfsRawProbe.ps1) on Windows 10 22H2, then extend:

1. `$LogFile` restart / log-record layout → `ntfs-log-replay.md`.
2. `$Extend\$Quota:$Q` index value bytes → `ntfs-quota-on-disk.md`.
3. Encrypted file on-disk body framing → `ntfs-encrypted-file-body.md`.
4. Any probe gate called out inside a “Sourced” topic spec when an implementer hits a missing offset.

Each probe session stores captures under `output/ntfs/<probe-id>/`, cites **that probe path** and the capture in the spec, and does not edit `ntfs.sys`. A later change that needs a different capture adds `probe-v2` (etc.) and leaves older versions in place unless an unused probe is retired per `tavera-probes.mdc`. Implementation is a later session that consumes only the updated specs and markers; gaps go in [`open-items.md`](open-items.md).
