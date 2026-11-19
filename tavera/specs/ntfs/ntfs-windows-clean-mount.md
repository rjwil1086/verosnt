<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-windows-clean-mount.md
PURPOSE:     Acceptance behavior for Windows mounting a Tevera OS-formatted NTFS volume
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# Tevera OS → Windows clean mount

## Public / in-tree facts

[NTFS overview](https://learn.microsoft.com/en-us/windows-server/storage/file-server/ntfs-overview) says NTFS uses its transaction log to restore consistency after a system failure.

[chkdsk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) checks file system and metadata; without `/f` it reports status and does not fix.

[Fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty) queries or sets the volume dirty bit; when set, autochk checks the volume at the next restart.

[How NTFS Works](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)) lists `$MftMirr` as a duplicate of the first four MFT records and `$Volume` as holding volume information including the dirty flag.

In-tree formatter (`sdk/lib/fslib/ntfslib/`):

- Version 3.1 (`NTFS_MAJOR_VERSION` / `NTFS_MINOR_VERSION` in `ntfslib.h`).
- `$VOLUME_INFORMATION` written with flags 0 — dirty bit left clear (`files.c`).
- `$MFTMirr` = first four system records; `$LogFile` filled with `0xFF` (`files.c`).
- Boot sector EBPB carries `$MFT` / `$MFTMirr` LCNs (`BOOT_SECTOR` / `EXTENDED_BIOS_PARAMETERS_BLOCK` in `ntfslib.h`).

## Compatibility outcomes (acceptance)

A volume that Tevera OS just formatted (fresh raw disk, `FormatEx` / `ntfslib` path) must satisfy all of the following when attached to Windows 10 22H2 x64:

1. Windows mounts it and `GetVolumeInformation` reports the file system name `NTFS`.
2. `fsutil dirty query <volume>` reports the volume is **not** dirty.
3. `chkdsk <volume>` **without** `/f` reports no problems that require a fix.
4. A future suite/probe guest log records a fixed end marker (name chosen when the harness is written; until then use `NTFS-WINDOWS-MOUNT-OK` as the planned marker) after the three checks above succeed.

Catalog row to add when the harness exists: `WindowsCleanMount` (see [`ntfs-win10-compat.md`](ntfs-win10-compat.md)).

## Probe / test gate (API-behavior, not raw layout)

A future session may write the GPL harness and Windows probe that performs the steps above and marks `WindowsCleanMount` green/red. That is API-behavior observation (mount text, dirty query, chkdsk text). It must not dump sectors into this spec or invent `$LogFile` layouts.

If Windows refuses to mount or marks the volume dirty / chkdsk-dirty, fix the formatter or driver from Learn + in-tree + that guest log, or open a **raw-format probe session** if a missing on-disk fact is required—do not invent the fact here.

## Non-goals

- Sector dumps or `$LogFile` record decode in this acceptance spec.
- Requiring WHPX; capture boots stay on TCG per project rules when a Windows disk is involved.
- Using the installed `virtual-machines/veros-ntfs/disk.qcow2` as the format target for this check (Milestone 45 text keeps that disk out of the fresh-volume path).

### Kept-disk host mount observation **(Rev.1)**

**(Rev.1)** Separate from the fresh-format acceptance above: host retest of the **kept** disk `virtual-machines/veros-ntfs/disk.qcow2` (converted to non-sparse fixed VHDX). Observation log `output/ntfs/issue-22-host-mount/host-mount-obs.txt` (`ISSUE-22-HOST-MOUNT-OBS` … `ISSUE-22-HOST-MOUNT-OBS-END`, 2026-11-18):

- `Mount-DiskImage` succeeded; IFS partition ~16 GiB.
- Root `Get-ChildItem` failed (`DIR_FAIL`).
- `chkdsk` without `/f` ([Learn](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk)): corrupt attribute-record messages; `Errors found. CHKDSK cannot continue in read-only mode.`

**(Rev.1)** Open item **NTFS-MOUNT-001**. Do not invent attribute layouts from this text; a raw-format probe is required before naming failing on-disk fields.

### Probe-v8 cite **(Rev.2)**

**(Rev.2)** Raw-format probe [`tavera/probes/ntfs/probe-v8/`](../../probes/ntfs/probe-v8/) cites `$UsnJrnl:$J` **AllocatedSize** vs mapping-pair cluster sum mismatch. Implementer spec: [`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md). **NTFS-MOUNT-001** remains open until host retest is clean (and any remaining 0x7C7/0x7D3 chkdsk names are probed).

**(Rev.3)** Host retest after setting `$J` `AllocatedSize` = run-sum × cluster on a kept-disk **copy** still `DIR_FAIL` + `chkdsk` errors ([`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md) Rev.1). **NTFS-MOUNT-001** / #22 stay open.

**(Rev.4)** Deep probe [`tavera/probes/ntfs/probe-v12/`](../../probes/ntfs/probe-v12/) (`output/ntfs/probe-v12/mount-deep-report.txt`, `NTFS-MOUNT-DEEP-REPORT` … `END`): `$J` has `NTFS_ATTR_SPARSE` with zero hole runs and SI sparse bit clear; **0x7C7** / **0x7D3** are in-use orphans vs parent **0x6EA** `$I30` (survivor **0x7E5**), with **0x7C7** also failing `$Bitmap` / LCN overlap. Spec Rev.2: [`ntfs-host-mount-corruption.md`](ntfs-host-mount-corruption.md). **NTFS-MOUNT-001** / #22 stay open pending implementer + host retest.

## Revision log

- **Rev.1** (2026-11-18): kept-disk Mount-DiskImage / chkdsk observation; **NTFS-MOUNT-001**.
- **Rev.2** (2026-11-18): probe-v8 → host-mount-corruption spec; `$J` alloc/run FAIL.
- **Rev.3** (2026-11-18): AllocatedSize=run_sum host retest insufficient.
- **Rev.4** (2026-11-18): probe-v12 sparse/`$J` + orphan 0x7C7/0x7D3; #22 stays open.
