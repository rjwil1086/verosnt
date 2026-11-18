<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-compression.md
PURPOSE:     Per-file NTFS compression FSCTL and LZNT1 default
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS compression

## Public / in-tree facts

[FSCTL_SET_COMPRESSION](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_compression) sets per-file or per-directory compression state. Learn states the only implemented algorithm is LZNT1, used as the default compression method. Directories are not compressed by the call; the call sets the default for files created in the directory. `FSCTL_GET_COMPRESSION` retrieves compression state; `GetFileAttributes` reports the compressed attribute.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) lists data and related attributes; compression is expressed on attribute flags in the on-disk attribute record (in-tree mask below).

In-tree:

- `NTFS_ATTR_COMPRESSION_MASK` (`0x00FF`) and `NTFS_FILE_TYPE_COMPRESSED` (`0x800`) in `drivers/filesystems/ntfs/ntfs.h`.
- `NtfsSetCompression` / get path and LZNT1 compress/decompress via `RtlCompressBuffer` / `RtlDecompressBuffer` in `drivers/filesystems/ntfs/ntfs3x.c` (uses `COMPRESSION_FORMAT_LZNT1` / `COMPRESSION_FORMAT_NONE`).
- Non-resident header includes `CompressionUnit` (`ATTR_RECORD` / `NTFS_ATTR_RECORD` in `ntfslib.h` / `ntfs.h`).

## Compatibility outcomes

- `FSCTL_SET_COMPRESSION` with LZNT1 must make subsequent reads return plaintext equal to the uncompressed data.
- Catalog: `Compression`.

## Probe gates

- On-disk compressed unit framing (exact stored unit size and packing) beyond what the in-tree driver already reads/writes: if a mismatch with Windows appears, capture units under `output/<tool>/` before changing constants.
- Do not invent a second compression algorithm; Learn documents LZNT1 as the only implemented method.

## Non-goals

- Matching Microsoft’s internal compression scheduling if a different structure still passes ReadFile/WriteFile and Windows mount/chkdsk on the volume.
