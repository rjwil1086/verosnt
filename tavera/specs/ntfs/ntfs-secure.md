<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-secure.md
PURPOSE:     $Secure streams and security-id based descriptors for NTFS 3.1
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS `$Secure` and security descriptors

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists `$Secure` as security descriptors used by the volume.

[MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) names `$SDH` and `$SII` as `$Secure` index allocation streams.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) lists `$SECURITY_DESCRIPTOR` and `$STANDARD_INFORMATION` (including quota-related fields in the attribute description on that page).

In-tree:

- Record 9 is `$Secure` (`NTFS_FILE_SECURE` / `METAFILE_SECURE`).
- Collation `COLLATION_NTOFS_ULONG` for `$SII`, `COLLATION_NTOFS_SECURITY_HASH` for `$SDH` (`ntfslib.h` / `ntfs.h`).
- `STANDARD_INFORMATION.SecurityId` at offset `0x34` (72-byte SI).
- Shared format security id `NTFS_SECURITY_ID` (`0x100`); `NTFS_SDS_MIRROR` (`0x40000`) places a duplicate `$SDS` set 256 KiB after the primary (`ntfslib.h`, used again in `ntfs3x.c`).
- `$Secure` / SDS lookup and append live in `drivers/filesystems/ntfs/ntfs3x.c`.

## Compatibility outcomes

- Files carry a security id in `$STANDARD_INFORMATION` that resolves through `$Secure` indexes/streams.
- Catalog: `Secure`, `SecurityDescriptor`.

## Probe gates

- Any `$SDS` entry header field (hash, id, offset, length) not already written by `sdk/lib/fslib/ntfslib/files.c` or declared beside that code must be capture-cited before new constants land.
- Do not invent `$SDH` / `$SII` key layouts beyond what the in-tree indexes already build.

## Non-goals

- Pasting Windows ACL UI policy.
- Using Microsoft PDB layouts for `$SDS`.
