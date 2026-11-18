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
