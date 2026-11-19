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

In-tree:

- `FILE_QUOTA_INFORMATION` is declared in `sdk/include/ndk/iotypes.h` (query/set buffer for the FSCTL path).
- `NtfsSetQuota` in `drivers/filesystems/ntfs/ntfs3x.c` returns `STATUS_NOT_IMPLEMENTED`. `FSCTL_SET_QUOTA` in `drivers/filesystems/ntfs/fsctl.c` calls it.

None of the pages above define the bytes stored in the `$Q` index entry.

## Compatibility outcomes

- Query/set buffers follow `FILE_QUOTA_INFORMATION` and the MS-FSA status rules once the on-disk value is known.
- Until the probe gate is closed, `FSCTL_SET_QUOTA` may remain `STATUS_NOT_IMPLEMENTED`. Suite rows `Quota` / `QuotaAccounting` cannot go green on a real set path without the capture-cited layout.

## Probe gate (raw-format)

A future **probe session** must:

1. Set and query quotas on Windows 10 22H2 through documented FSCTLs / APIs via [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/).
2. Capture `$Extend\$Quota:$Q` index entry bytes under `output/ntfs/probe-v1/`, then copy into `tavera/specs/ntfs/sources/probes/probe-v1/` (and the private mirror) for citation.
3. Extend **this** spec with each new field size/offset cited to a capture and probe step.

No `NtfsSetQuota` implementation in that session. A later implementation session consumes only the updated spec.

## Non-goals

- Guessing `$Q` value layout from memory or forbidden symbol dumps.
- Requiring Microsoft’s quota UI or policy stack beyond the on-disk + FSCTL contract.
