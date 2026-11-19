<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-encrypted-file-body.md
PURPOSE:     Documented encrypted-file API behavior and the missing on-disk body
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS encrypted file body

## Public / in-tree facts

[Handling Encrypted Files and Directories](https://learn.microsoft.com/en-us/windows/win32/fileio/handling-encrypted-files-and-directories) says `EncryptFile` encrypts every data stream in the file, and that a later read through the normal file APIs returns plaintext for a caller who can decrypt it.

[Backup and Restore of Encrypted Files](https://learn.microsoft.com/en-us/windows/win32/fileio/backup-and-restore-of-encrypted-files) says `ReadEncryptedFileRaw` returns the file's data still encrypted.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) says `$LOGGED_UTILITY_STREAM` is used by the encrypting file system.

[MS-EFSR: EFSRPC Raw Data Format](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-efsr/e67520ea-0cb6-4349-bbe5-427a644d9492) says the details of that backup format are implementation-dependent and that a client should not parse it. The page does not say those bytes are the on-disk file body.

[MS-GPEF: Overview](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gpef/4d4687f9-132c-4501-9996-684b40b14735) says a symmetric key encrypts the file data. It does not give the framing of that data on disk.

In-tree:

- Attribute type `AttributeLoggedUtilityStream = 0x100` in `drivers/filesystems/ntfs/ntfs.h` and `sdk/lib/fslib/ntfslib/ntfslib.h`.
- File attribute flag `NTFS_FILE_TYPE_ENCRYPTED` (`0x4000`) in `drivers/filesystems/ntfs/ntfs.h`.
- `NtfsReadEncrypted`, `NtfsWriteEncrypted`, and `NtfsSetEncryption` in `drivers/filesystems/ntfs/ntfs3x.c` return `STATUS_NOT_IMPLEMENTED`. `FSCTL_SET_ENCRYPTION` in `drivers/filesystems/ntfs/fsctl.c` calls `NtfsSetEncryption`.

## Compatibility outcomes

- Callers with decrypt rights who use normal read APIs must see plaintext once encryption is implemented.
- `ReadEncryptedFileRaw` must still be able to obtain ciphertext for backup, per Learn.
- Until the probe gate closes, encryption I/O stays `STATUS_NOT_IMPLEMENTED`. Catalog row `Encryption` cannot go green on real encrypt/decrypt without capture-cited framing.

## Probe gate (raw-format)

A future **probe session** must:

1. Encrypt a file on Windows 10 22H2 with documented APIs (`EncryptFile` / related) via [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/).
2. Capture the on-disk data attribute / `$LOGGED_UTILITY_STREAM` (or other streams the capture shows) under `output/ntfs/probe-v1/`, then copy into `tavera/specs/ntfs/sources/probes/probe-v1/` (and the private mirror) for citation.
3. Extend **this** spec with framing facts cited to captures and probe steps.

No encrypted read/write implementation in that session. A later implementation session uses only the updated spec.

## Non-goals

- Treating MS-EFSR backup blobs as proven on-disk layout without a capture that shows they match.
- Inventing EFS key or FEK framing from recall.
