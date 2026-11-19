# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/test-suites/ntfs/features.ps1
# PURPOSE:     Fixed NTFS 3.1 catalog. Names stay put while ntfs.sys changes.
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
#              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
#              Ryan Wilson <ryan@taveraos.com>

function Get-NtfsFeatureCatalog {
    @(
        @{ Name = 'BootSector'; Group = 'Volume' }
        @{ Name = 'Version3_1'; Group = 'Volume' }
        @{ Name = 'VolumeLabel'; Group = 'Volume' }
        @{ Name = 'ClusterSize'; Group = 'Volume' }
        @{ Name = 'Mft'; Group = 'Volume' }
        @{ Name = 'MftMirror'; Group = 'Volume' }
        @{ Name = 'LogFile'; Group = 'Volume' }
        @{ Name = 'AttrDef'; Group = 'Volume' }
        @{ Name = 'UpCase'; Group = 'Volume' }
        @{ Name = 'Bitmap'; Group = 'Volume' }
        @{ Name = 'BootFile'; Group = 'Volume' }
        @{ Name = 'BadClus'; Group = 'Volume' }
        @{ Name = 'Secure'; Group = 'Volume' }
        @{ Name = 'Extend'; Group = 'Volume' }
        @{ Name = 'ObjId'; Group = 'Volume' }
        @{ Name = 'Quota'; Group = 'Volume' }
        @{ Name = 'Reparse'; Group = 'Volume' }
        @{ Name = 'UsnJrnl'; Group = 'Volume' }
        @{ Name = 'FileRecordFixup'; Group = 'Records' }
        @{ Name = 'StandardInformation72'; Group = 'Records' }
        @{ Name = 'ResidentData'; Group = 'Records' }
        @{ Name = 'NonResidentData'; Group = 'Records' }
        @{ Name = 'AttributeList'; Group = 'Records' }
        @{ Name = 'Timestamps'; Group = 'Records' }
        @{ Name = 'EndOfFile'; Group = 'Records' }
        @{ Name = 'AllocationSize'; Group = 'Records' }
        @{ Name = 'Create'; Group = 'Records' }
        @{ Name = 'Delete'; Group = 'Records' }
        @{ Name = 'Rename'; Group = 'Records' }
        @{ Name = 'CaseInsensitive'; Group = 'Records' }
        @{ Name = 'ShortName'; Group = 'Records' }
        @{ Name = 'HardLink'; Group = 'Records' }
        @{ Name = 'AlternateStream'; Group = 'Records' }
        @{ Name = 'DirectoryIndex'; Group = 'Records' }
        @{ Name = 'DirectoryIndexSplit'; Group = 'Records' }
        @{ Name = 'BitmapAccounting'; Group = 'Records' }
        @{ Name = 'Remount'; Group = 'Records' }
        @{ Name = 'LogReplay'; Group = 'Records' }
        @{ Name = 'LogOps'; Group = 'Records' }
        @{ Name = 'SecurityDescriptor'; Group = 'Ntfs3' }
        @{ Name = 'ObjectIdApi'; Group = 'Ntfs3' }
        @{ Name = 'QuotaAccounting'; Group = 'Ntfs3' }
        @{ Name = 'QuotaDelete'; Group = 'Ntfs3' }
        @{ Name = 'ReparsePoint'; Group = 'Ntfs3' }
        @{ Name = 'Sparse'; Group = 'Ntfs3' }
        @{ Name = 'Compression'; Group = 'Ntfs3' }
        @{ Name = 'Encryption'; Group = 'Ntfs3' }
        @{ Name = 'EfsBody'; Group = 'Ntfs3' }
        @{ Name = 'UsnJournal'; Group = 'Ntfs3' }
        @{ Name = 'ConcurrentWriters'; Group = 'Load' }
        @{ Name = 'IoErrorVisible'; Group = 'Load' }
        @{ Name = 'PowerLoss'; Group = 'Load' }
        @{ Name = 'StaticAnalysis'; Group = 'Host' }
        @{ Name = 'HostSanitizers'; Group = 'Host' }
        @{ Name = 'Fuzz'; Group = 'Host' }
    )
}
