<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-sparse.md
PURPOSE:     Sparse files FSCTL behavior and in-tree attribute flags
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS sparse files

## Public / in-tree facts

[MS-FSA: FSCTL_SET_SPARSE](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/b5b3dd4c-b585-49d7-960e-fbfbf7a85af0) defines setting or clearing sparse on a data stream, status rules, and `FILE_ATTRIBUTE_SPARSE_FILE` when any stream is sparse. Input uses `FILE_SET_SPARSE_BUFFER` ([MS-FSCC](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/) section referenced from that FSA page).

In-tree:

- Attribute flag `NTFS_ATTR_SPARSE` (`0x8000`) and file type `NTFS_FILE_TYPE_SPARSE` (`0x200`) in `drivers/filesystems/ntfs/ntfs.h`.
- `FILE_SET_SPARSE_BUFFER` in `sdk/include/xdk/iotypes.h`.
- `NtfsSetSparse` in `drivers/filesystems/ntfs/ntfs3x.c` sets or clears those flags on the data attribute.
- Run lists use `NTFS_SPARSE_LBO` (`(LONGLONG)-1`) for holes (`ntfs.h`).
- `drivers/filesystems/ntfs/blockdev.c` satisfies sparse reads with zeros; **writes to sparse runs return `STATUS_NOT_IMPLEMENTED`**.

## Compatibility outcomes

- `FSCTL_SET_SPARSE` must follow MS-FSA status rules for supported opens.
- Reads over holes return zeros.
- Writes that allocate into holes must succeed once implemented; today they are `STATUS_NOT_IMPLEMENTED`.
- Catalog: `Sparse`.

## Probe gates

- On-disk encoding of a sparse run beyond the in-tree `NTFS_SPARSE_LBO` sentinel and attribute flag: capture before inventing packed run bytes Windows writes for holes.
- Validated write-into-hole behavior should be confirmed with an API probe (status + readable zeros/data), citing the guest log marker—not a guessed allocator policy.

## Non-goals

- Copying Microsoft’s internal sparse allocator heuristics when a different structure still matches ReadFile/WriteFile and the on-disk NTFS 3.1 results Windows accepts.
