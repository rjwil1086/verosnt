/*
 * PROJECT:     VerOS NT Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        drivers/filesystems/ntfs/ntfs3x.c
 * PURPOSE:     NTFS 3.1 security, $Extend, USN, and compression
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 *              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
 *              Ryan Wilson <ryan@taveraos.com>
 */

/*
 * $Secure, the $Extend indexes, the USN journal, and LZNT1 are updated here.
 * Quota $Q threshold/limit at resident index-root offsets 316/324:
 * tavera/specs/ntfs/ntfs-quota-on-disk.md (NTFS-QUOTA-002 / probe-v2).
 * Track growth format→four-entry $Q (keys 1, 0x100, 0x101, 0x102):
 * tavera/specs/ntfs/ntfs-quota-on-disk.md Rev.3 / probe-v3
 * (output/ntfs/probe-v3/… NTFS-QUOTA-GROWTH-REPORT). Packing from in-tree
 * MkViewRootN/Qce in sdk/lib/fslib/ntfslib/files.c; live SIDs only.
 * QuotaLimit -2 clears thr/lim to -1 by SID (keep leaf; optional 0x102→0x103):
 * tavera/specs/ntfs/ntfs-quota-on-disk.md Rev.4–8 / probe-v4 (NTFS-QUOTA-004).
 * Real NtfsReplayLog record apply stays blocked on NTFS-LOG-001
 * (tavera/specs/ntfs/ntfs-log-replay.md). Black-box dirty remount
 * (NTFS-LOG-002 / Rev.3–4): FSCTL_MARK/IS_VOLUME_DIRTY on $VOLUME_INFORMATION.
 * Dirty-mount restart policy (NTFS-LOG-003 / GitHub #2):
 * tavera/specs/ntfs/ntfs-log-restart-mount.md — probe-local RSTR tag,
 * PAGE_SIZE_U32_AT_10, SLOT_U64_AT_60/68 from probe-v11 restart-report
 * (NTFS-LOG-RESTART-REPORT). Empty slots → clear dirty/RW; non-empty → RO;
 * never apply RCRD bodies. Offsets are the probe method, not Microsoft names.
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/fsctl-mark-volume-dirty
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/fsctl-is-volume-dirty
 * https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty
 * CompressionUnit / index-root / reparse-OID bounds:
 * tavera/specs/ntfs/ntfs-compression.md Rev.1 (NTFS-COMP-001);
 * ntfs-reparse.md Rev.1 (NTFS-IDX-001 / NTFS-ATTR-001);
 * ntfs-object-id.md Rev.1 (NTFS-ATTR-001).
 * EFS structural $EFS (NTFS-EFS-002 / EFS-004): 672-byte $LOGGED_UTILITY_STREAM
 * named $EFS; fixed 16-byte header from probe-v6; offset 16+ left zero (no
 * invented FEK). $DATA length unchanged; plaintext readback for suite.
 * tavera/specs/ntfs/ntfs-encrypted-file-body.md Rev.3–4 /
 * tavera/probes/ntfs/probe-v6/ (NTFS-EFS-PROBE-REPORT).
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-encryptfilew
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702
 * MS-FSCC FILE_QUOTA_INFORMATION:
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/acdc0738-ba3c-47a1-b11a-72e22d831c57
 * FSCTL_SET_SPARSE SI bit with ATTR (NTFS-MOUNT-001 Rev.2 / probe-v12):
 * tavera/specs/ntfs/ntfs-host-mount-corruption.md; ntfs-sparse.md;
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/b5b3dd4c-b585-49d7-960e-fbfbf7a85af0
 * IRP_MJ_SET_QUOTA / IRP_MJ_QUERY_QUOTA:
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/irp-mj-set-quota
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntqueryquotainformationfile
 */

#include "ntfs.h"

/* Rust helpers in drivers/filesystems/ntfs/src/lib.rs */
NTSTATUS
ntfs_quota_write_threshold_limit(PUCHAR Root, SIZE_T RootLength, LONGLONG Threshold, LONGLONG Limit);
NTSTATUS
ntfs_quota_write_entry_threshold_limit(PUCHAR Root,
                                       SIZE_T RootLength,
                                       ULONG OwnerKey,
                                       const UCHAR *Sid,
                                       SIZE_T SidLength,
                                       LONGLONG Threshold,
                                       LONGLONG Limit);
NTSTATUS
ntfs_quota_clear_for_delete(PUCHAR Root, SIZE_T RootLength, const UCHAR *Sid, SIZE_T SidLength);
NTSTATUS
ntfs_quota_append_entry(const UCHAR *InRoot,
                        SIZE_T InLength,
                        ULONG OwnerKey,
                        const UCHAR *Sid,
                        SIZE_T SidLength,
                        LONGLONG Threshold,
                        LONGLONG Limit,
                        LONGLONG ChangeTime,
                        PUCHAR OutRoot,
                        SIZE_T OutCapacity,
                        PSIZE_T OutLength);
NTSTATUS
ntfs_quota_max_owner_key(const UCHAR *Root, SIZE_T RootLength, PULONG MaxKey);
NTSTATUS
ntfs_quota_query(const UCHAR *Root,
                 SIZE_T RootLength,
                 PUCHAR Out,
                 SIZE_T OutCapacity,
                 PSIZE_T OutLength);
NTSTATUS
ntfs_quota_grow_for_track(const UCHAR *InRoot,
                          SIZE_T InLength,
                          const UCHAR *Sid101,
                          SIZE_T Sid101Length,
                          const UCHAR *Sid102,
                          SIZE_T Sid102Length,
                          LONGLONG ChangeTime,
                          PUCHAR OutRoot,
                          SIZE_T OutCapacity,
                          PSIZE_T OutLength);

#define NDEBUG
#include <debug.h>

/* NTFS_SDS_MIRROR in sdk/lib/fslib/ntfslib/ntfslib.h */
#define NTFS_SDS_MIRROR 0x40000

static
NTSTATUS
NtfsAddResidentAttribute(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER Record,
                         ULONG Type,
                         PCWSTR Name,
                         UCHAR NameLength,
                         const UCHAR *Value,
                         ULONG ValueLength);

static
ULONG
NtfsHashSecurity(const UCHAR *Data, ULONG Length)
{
    ULONG Hash = 0;
    ULONG Index;

    for (Index = 0; Index + 4 <= Length; Index += 4)
    {
        ULONG Dword = *(const ULONG *)(Data + Index);
        Hash = Dword + ((Hash << 3) | (Hash >> 29));
    }
    return Hash;
}

static
NTSTATUS
NtfsFindExtend(PDEVICE_EXTENSION Vcb, PCWSTR Name, PULONGLONG Index)
{
    UNICODE_STRING UpName;
    ULONG FirstEntry = 0;

    RtlInitUnicodeString(&UpName, Name);
    /* FirstEntry is required. A NULL is dereferenced while walking the index. */
    return NtfsFindMftRecord(Vcb, NTFS_FILE_EXTEND, &UpName, &FirstEntry, FALSE, TRUE, Index);
}

static
NTSTATUS
NtfsReadRecord(PDEVICE_EXTENSION Vcb, ULONGLONG Index, PFILE_RECORD_HEADER *Record)
{
    NTSTATUS Status;

    *Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (*Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Index, *Record);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *Record);
        *Record = NULL;
    }
    return Status;
}

static
VOID
NtfsFreeRecord(PDEVICE_EXTENSION Vcb, PFILE_RECORD_HEADER Record)
{
    if (Record != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
}

static
NTSTATUS
NtfsStoreAttribute(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER Record,
                   PNTFS_ATTR_CONTEXT Context)
{
    PNTFS_ATTR_RECORD Slot;

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Instance == Context->pRecord->Instance &&
            Slot->Length == Context->pRecord->Length)
        {
            RtlCopyMemory(Slot, Context->pRecord, Slot->Length);
            return UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }
    return STATUS_NOT_FOUND;
}

/* Put one view-index entry into a leaf INDX block of a large index. */
static
NTSTATUS
NtfsInsertIndexBlock(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER Record,
                     PCWSTR IndexName,
                     PINDEX_ROOT_ATTRIBUTE Root,
                     const UCHAR *Key,
                     USHORT KeyLength,
                     const UCHAR *Data,
                     USHORT DataLength)
{
    PNTFS_ATTR_CONTEXT Allocation = NULL;
    PINDEX_BUFFER Block;
    PUCHAR Entries;
    PUCHAR End;
    PUCHAR Cursor;
    ULONGLONG Size;
    ULONGLONG Offset;
    ULONG BlockSize;
    ULONG EntryLength;
    ULONG Written;
    NTSTATUS Status;

    BlockSize = Root->SizeOfEntry;
    if (BlockSize < 512 || BlockSize > 65536)
        return STATUS_NOT_IMPLEMENTED;

    Status = FindAttribute(Vcb, Record, AttributeIndexAllocation, IndexName,
                           (ULONG)wcslen(IndexName), &Allocation, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Block = ExAllocatePoolWithTag(NonPagedPool, BlockSize, TAG_NTFS);
    if (Block == NULL)
    {
        ReleaseAttributeContext(Allocation);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    EntryLength = ALIGN_UP_BY(16 + KeyLength + DataLength, 8);
    Size = AttributeDataLength(Allocation->pRecord);
    Status = STATUS_DISK_FULL;
    for (Offset = 0; Offset + BlockSize <= Size; Offset += BlockSize)
    {
        if (ReadAttribute(Vcb, Allocation, Offset, (PCHAR)Block, BlockSize) != BlockSize)
            continue;
        if (FixupUpdateSequenceArray(Vcb, &Block->Ntfs) != STATUS_SUCCESS)
            continue;
        if (Block->Ntfs.Type != NRH_INDX_TYPE)
            continue;
        if (Block->Header.Flags & 1)
            continue;
        if (Block->Header.TotalSizeOfEntries + EntryLength > Block->Header.AllocatedSize)
            continue;

        Entries = (PUCHAR)&Block->Header + Block->Header.FirstEntryOffset;
        End = (PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries;
        Cursor = Entries;
        while (Cursor + 16 <= End)
        {
            USHORT Length = *(PUSHORT)(Cursor + 8);
            USHORT ThisKey = *(PUSHORT)(Cursor + 10);
            USHORT Flags = *(PUSHORT)(Cursor + 12);
            int Compare;

            if (Length < 16 || Cursor + Length > End)
                break;
            if (Flags & NTFS_INDEX_ENTRY_END)
                break;
            Compare = memcmp(Key, Cursor + 16, min(KeyLength, ThisKey));
            if (Compare == 0 && KeyLength == ThisKey)
            {
                Status = STATUS_SUCCESS;
                goto Done;
            }
            if (Compare < 0 || (Compare == 0 && KeyLength < ThisKey))
                break;
            Cursor += Length;
        }

        RtlMoveMemory(Cursor + EntryLength, Cursor, (ULONG)(End - Cursor));
        RtlZeroMemory(Cursor, EntryLength);
        *(PUSHORT)Cursor = (USHORT)ALIGN_UP_BY(16 + KeyLength, 8);
        *(PUSHORT)(Cursor + 2) = DataLength;
        *(PUSHORT)(Cursor + 8) = (USHORT)EntryLength;
        *(PUSHORT)(Cursor + 10) = KeyLength;
        RtlCopyMemory(Cursor + 16, Key, KeyLength);
        RtlCopyMemory(Cursor + *(PUSHORT)Cursor, Data, DataLength);
        Block->Header.TotalSizeOfEntries += EntryLength;
        AddFixupArray(Vcb, &Block->Ntfs);
        Status = WriteAttribute(Vcb, Allocation, Offset, (PCHAR)Block, BlockSize, &Written, Record);
        goto Done;
    }

Done:
    ExFreePoolWithTag(Block, TAG_NTFS);
    ReleaseAttributeContext(Allocation);
    return Status;
}

/* Insert a view-index entry. A small index grows inside the file record when
 * the record still has spare bytes. A large index takes a free leaf block. */
static
NTSTATUS
NtfsInsertRootEntry(PDEVICE_EXTENSION Vcb,
                    ULONGLONG Directory,
                    PCWSTR IndexName,
                    const UCHAR *Key,
                    USHORT KeyLength,
                    const UCHAR *Data,
                    USHORT DataLength)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PNTFS_ATTR_RECORD Slot;
    PINDEX_ROOT_ATTRIBUTE Root;
    PUCHAR Entries;
    PUCHAR End;
    PUCHAR Cursor;
    ULONG EntryLength;
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, Directory, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb, Record, AttributeIndexRoot, IndexName, (ULONG)wcslen(IndexName), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Context->pRecord->IsNonResident)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_IMPLEMENTED;
    }

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0 &&
           Slot->Instance != Context->pRecord->Instance)
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    if (Slot->Type == AttributeEnd || Slot->Length == 0)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_FOUND;
    }

    /* NTFS-IDX-001: resident value must sit inside the attribute record. */
    if (Slot->Length < 24 ||
        Slot->Resident.ValueOffset > Slot->Length ||
        Slot->Resident.ValueLength > Slot->Length - Slot->Resident.ValueOffset)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Root = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)Slot + Slot->Resident.ValueOffset);
    {
        ULONG HeaderBase = FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header);
        ULONG HeaderRoom;

        if (Slot->Resident.ValueLength < HeaderBase + sizeof(INDEX_HEADER_ATTRIBUTE))
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_FILE_CORRUPT_ERROR;
        }

        HeaderRoom = Slot->Resident.ValueLength - HeaderBase;
        if (Root->Header.AllocatedSize > HeaderRoom ||
            Root->Header.TotalSizeOfEntries > Root->Header.AllocatedSize ||
            Root->Header.FirstEntryOffset > Root->Header.TotalSizeOfEntries ||
            Root->Header.FirstEntryOffset < sizeof(INDEX_HEADER_ATTRIBUTE))
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_FILE_CORRUPT_ERROR;
        }
    }

    if (Root->Header.Flags != INDEX_ROOT_SMALL)
    {
        Status = NtfsInsertIndexBlock(Vcb, Record, IndexName, Root, Key, KeyLength, Data, DataLength);
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    EntryLength = ALIGN_UP_BY(16 + KeyLength + DataLength, 8);
    if (Root->Header.TotalSizeOfEntries + EntryLength > Root->Header.AllocatedSize)
    {
        ULONG Extra = Root->Header.TotalSizeOfEntries + EntryLength - Root->Header.AllocatedSize;
        ULONG ValueEnd = Slot->Resident.ValueOffset + Slot->Resident.ValueLength;
        ULONG Slack = (Slot->Length > ValueEnd) ? Slot->Length - ValueEnd : 0;
        ULONG FromRecord = (Extra > Slack) ? ALIGN_UP_BY(Extra - Slack, 8) : 0;
        ULONG Spare;
        PUCHAR Next;
        ULONG NextOff;

        Extra = ALIGN_UP_BY(Extra, 8);
        if (Record->BytesAllocated < Record->BytesInUse)
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        Spare = Record->BytesAllocated - Record->BytesInUse;
        if (FromRecord > Spare)
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_DISK_FULL;
        }
        Next = (PUCHAR)Slot + Slot->Length;
        NextOff = (ULONG)(Next - (PUCHAR)Record);
        if (NextOff > Record->BytesInUse ||
            Slot->Length == 0 ||
            (PUCHAR)Slot < (PUCHAR)Record ||
            Next > (PUCHAR)Record + Record->BytesInUse)
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        if (FromRecord != 0)
        {
            ULONG Tail = Record->BytesInUse - NextOff;

            RtlMoveMemory(Next + FromRecord, Next, Tail);
            Slot->Length += FromRecord;
            Record->BytesInUse += FromRecord;
        }
        RtlZeroMemory((PUCHAR)Slot + Slot->Resident.ValueOffset + Slot->Resident.ValueLength, Extra);
        Slot->Resident.ValueLength += Extra;
        Root->Header.AllocatedSize += Extra;
    }

    Entries = (PUCHAR)&Root->Header + Root->Header.FirstEntryOffset;
    End = (PUCHAR)&Root->Header + Root->Header.TotalSizeOfEntries;
    if (End < Entries ||
        End > (PUCHAR)Slot + Slot->Resident.ValueOffset + Slot->Resident.ValueLength ||
        (PUCHAR)&Root->Header + Root->Header.AllocatedSize >
            (PUCHAR)Slot + Slot->Resident.ValueOffset + Slot->Resident.ValueLength)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Cursor = Entries;
    while (Cursor + 16 <= End)
    {
        USHORT Length = *(PUSHORT)(Cursor + 8);
        USHORT ThisKey = *(PUSHORT)(Cursor + 10);
        USHORT Flags = *(PUSHORT)(Cursor + 12);
        int Compare;

        if (Length < 16 || Cursor + Length > End)
            break;
        if (Flags & NTFS_INDEX_ENTRY_END)
            break;

        Compare = memcmp(Key, Cursor + 16, min(KeyLength, ThisKey));
        if (Compare == 0 && KeyLength == ThisKey)
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_SUCCESS;
        }
        if (Compare < 0 || (Compare == 0 && KeyLength < ThisKey))
            break;
        Cursor += Length;
    }

    if (Cursor < Entries ||
        Cursor > End ||
        Cursor + EntryLength > (PUCHAR)&Root->Header + Root->Header.AllocatedSize)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    RtlMoveMemory(Cursor + EntryLength, Cursor, (ULONG)(End - Cursor));
    RtlZeroMemory(Cursor, EntryLength);
    *(PUSHORT)Cursor = (USHORT)ALIGN_UP_BY(16 + KeyLength, 8);
    *(PUSHORT)(Cursor + 2) = DataLength;
    *(PUSHORT)(Cursor + 8) = (USHORT)EntryLength;
    *(PUSHORT)(Cursor + 10) = KeyLength;
    RtlCopyMemory(Cursor + 16, Key, KeyLength);
    RtlCopyMemory(Cursor + *(PUSHORT)Cursor, Data, DataLength);
    Root->Header.TotalSizeOfEntries += EntryLength;

    Status = UpdateFileRecord(Vcb, Directory, Record);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsClearVolumeDirty(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER Volume;
    PNTFS_ATTR_RECORD Slot;
    PVOLINFO_ATTRIBUTE VolInfo;
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, NTFS_FILE_VOLUME, &Volume);
    if (!NT_SUCCESS(Status))
        return Status;

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Volume + Volume->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Type == AttributeVolumeInformation && !Slot->IsNonResident)
        {
            VolInfo = (PVOLINFO_ATTRIBUTE)((PUCHAR)Slot + Slot->Resident.ValueOffset);
            VolInfo->Flags &= ~NTFS_VOLUME_DIRTY;
            break;
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }

    Status = UpdateFileRecord(Vcb, NTFS_FILE_VOLUME, Volume);
    NtfsFreeRecord(Vcb, Volume);
    if (!NT_SUCCESS(Status))
        return Status;

    Vcb->Flags &= ~VCB_VOLUME_READ_ONLY;
    Vcb->NtfsInfo.Flags &= ~NTFS_VOLUME_DIRTY;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsMarkVolumeDirty(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER Volume;
    PNTFS_ATTR_RECORD Slot;
    PVOLINFO_ATTRIBUTE VolInfo;
    NTSTATUS Status;
    BOOLEAN Found = FALSE;

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_ACCESS_DENIED;

    Status = NtfsReadRecord(Vcb, NTFS_FILE_VOLUME, &Volume);
    if (!NT_SUCCESS(Status))
        return Status;

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Volume + Volume->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Type == AttributeVolumeInformation && !Slot->IsNonResident)
        {
            VolInfo = (PVOLINFO_ATTRIBUTE)((PUCHAR)Slot + Slot->Resident.ValueOffset);
            VolInfo->Flags |= NTFS_VOLUME_DIRTY;
            Found = TRUE;
            break;
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }

    if (!Found)
    {
        NtfsFreeRecord(Vcb, Volume);
        return STATUS_NOT_FOUND;
    }

    Status = UpdateFileRecord(Vcb, NTFS_FILE_VOLUME, Volume);
    NtfsFreeRecord(Vcb, Volume);
    if (!NT_SUCCESS(Status))
        return Status;

    Vcb->NtfsInfo.Flags |= NTFS_VOLUME_DIRTY;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsReplayLog(PDEVICE_EXTENSION Vcb)
{
    /*
     * NTFS-LOG-003 / ntfs-log-restart-mount.md (probe-v11 on probe-v7 dumps).
     * Probe-local labels only — Microsoft field names remain open (NTFS-LOG-001).
     * Never parse or apply RCRD bodies (GitHub #2).
     */
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PUCHAR Page = NULL;
    ULONG PageSize;
    ULONG Got;
    ULONG Tag;
    ULONGLONG Slot60;
    ULONGLONG Slot68;
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, NTFS_FILE_LOGFILE, &Record);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }

    /* Need at least a candidate restart page; size comes from the page itself. */
    Page = ExAllocatePoolWithTag(NonPagedPool, 0x1000, TAG_NTFS);
    if (Page == NULL)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }

    Got = ReadAttribute(Vcb, Context, 0, (PCHAR)Page, 0x1000);
    if (Got < 0x68 + sizeof(ULONGLONG))
    {
        ExFreePoolWithTag(Page, TAG_NTFS);
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        /* No usable page (e.g. 0xFF fill) — black-box suite clears dirty. */
        return STATUS_SUCCESS;
    }

    RtlCopyMemory(&Tag, Page, sizeof(Tag));
    /* PAGE_TAG_RSTR = 'RSTR' little-endian bytes 52 53 54 52 */
    if (Tag != 0x52545352UL)
    {
        ExFreePoolWithTag(Page, TAG_NTFS);
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }

    RtlCopyMemory(&PageSize, Page + 0x10, sizeof(PageSize));
    if (PageSize != 0x1000)
    {
        ExFreePoolWithTag(Page, TAG_NTFS);
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        /* Unrecognized layout — stay RO rather than guess. */
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    RtlCopyMemory(&Slot60, Page + 0x60, sizeof(Slot60));
    RtlCopyMemory(&Slot68, Page + 0x68, sizeof(Slot68));
    ExFreePoolWithTag(Page, TAG_NTFS);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);

    if (Slot60 != 0 || Slot68 != 0)
    {
        /* Outstanding client restart slots — RO, never apply RCRD. */
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    /* Both slots zero — clear dirty / RW; still do not apply RCRD. */
    return STATUS_SUCCESS;
}

VOID
NtfsLogFileRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG MftIndex,
                  PFILE_RECORD_HEADER FileRecord)
{
    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(MftIndex);
    UNREFERENCED_PARAMETER(FileRecord);
}

static
NTSTATUS
NtfsLookupSecurity(PDEVICE_EXTENSION Vcb,
                   const UCHAR *Body,
                   ULONG BodyLength,
                   PULONG SecurityId)
{
    PFILE_RECORD_HEADER Secure = NULL;
    PNTFS_ATTR_CONTEXT Sds = NULL;
    PUCHAR Buffer;
    ULONGLONG Offset = 0;
    ULONGLONG Size;
    ULONG Hash;
    NTSTATUS Status;

    *SecurityId = NTFS_SECURITY_ID;
    Status = NtfsReadRecord(Vcb, NTFS_FILE_SECURE, &Secure);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Secure, AttributeData, L"$SDS", 4, &Sds, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Secure);
        return Status;
    }

    Hash = NtfsHashSecurity(Body, BodyLength);
    Size = AttributeDataLength(Sds->pRecord);
    Buffer = ExAllocatePoolWithTag(NonPagedPool, 0x10000, TAG_NTFS);
    if (Buffer == NULL)
    {
        ReleaseAttributeContext(Sds);
        NtfsFreeRecord(Vcb, Secure);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Offset + 0x18 < Size && Offset < NTFS_SDS_MIRROR)
    {
        ULONG Got = (ULONG)min(0x10000, Size - Offset);
        ULONG Cursor = 0;

        if (ReadAttribute(Vcb, Sds, Offset, (PCHAR)Buffer, Got) != Got)
            break;

        while (Cursor + 0x18 <= Got)
        {
            ULONG EntryHash = *(PULONG)(Buffer + Cursor);
            ULONG Id = *(PULONG)(Buffer + Cursor + 4);
            ULONG EntryLength = *(PULONG)(Buffer + Cursor + 0x10);

            if (EntryLength < 0x18 || EntryLength > Got - Cursor)
                break;
            if (EntryHash == Hash &&
                EntryLength - 0x14 == BodyLength &&
                RtlCompareMemory(Buffer + Cursor + 0x14, Body, BodyLength) == BodyLength)
            {
                *SecurityId = Id;
                ExFreePoolWithTag(Buffer, TAG_NTFS);
                ReleaseAttributeContext(Sds);
                NtfsFreeRecord(Vcb, Secure);
                return STATUS_SUCCESS;
            }
            Cursor = ALIGN_UP_BY(Cursor + EntryLength, 16);
        }
        Offset += Got;
    }

    ExFreePoolWithTag(Buffer, TAG_NTFS);
    ReleaseAttributeContext(Sds);
    NtfsFreeRecord(Vcb, Secure);
    return STATUS_NOT_FOUND;
}

static
NTSTATUS
NtfsAppendSecurity(PDEVICE_EXTENSION Vcb,
                   const UCHAR *Body,
                   ULONG BodyLength,
                   PULONG SecurityId)
{
    PFILE_RECORD_HEADER Secure = NULL;
    PNTFS_ATTR_CONTEXT Sds = NULL;
    PUCHAR Entry;
    ULONGLONG End;
    ULONG EntryLength;
    ULONG Id;
    ULONG Written;
    UCHAR Key[8];
    UCHAR Header[0x14];
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, NTFS_FILE_SECURE, &Secure);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Secure, AttributeData, L"$SDS", 4, &Sds, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Secure);
        return Status;
    }

    End = AttributeDataLength(Sds->pRecord);
    if (End >= NTFS_SDS_MIRROR)
        End = 0;
    /* The next id is one past the last entry we can see. 0x100 is the first. */
    Id = NTFS_SECURITY_ID;
    if (End >= 0x18)
    {
        UCHAR Tail[0x18];
        ULONGLONG Scan = 0;

        while (Scan + 0x18 <= End)
        {
            ULONG This = 0;
            if (ReadAttribute(Vcb, Sds, Scan + 0x10, (PCHAR)&This, 4) != 4 || This < 0x18)
                break;
            if (ReadAttribute(Vcb, Sds, Scan + 4, (PCHAR)Tail, 4) == 4)
                Id = max(Id, *(PULONG)Tail + 1);
            Scan = ALIGN_UP_BY(Scan + This, 16);
        }
    }

    EntryLength = ALIGN_UP_BY(0x14 + BodyLength, 16);
    Entry = ExAllocatePoolWithTag(NonPagedPool, EntryLength, TAG_NTFS);
    if (Entry == NULL)
    {
        ReleaseAttributeContext(Sds);
        NtfsFreeRecord(Vcb, Secure);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Entry, EntryLength);
    *(PULONG)Entry = NtfsHashSecurity(Body, BodyLength);
    *(PULONG)(Entry + 4) = Id;
    *(PULONGLONG)(Entry + 8) = End;
    *(PULONG)(Entry + 0x10) = 0x14 + BodyLength;
    RtlCopyMemory(Entry + 0x14, Body, BodyLength);

    Status = WriteAttribute(Vcb, Sds, End, Entry, EntryLength, &Written, Secure);
    if (NT_SUCCESS(Status) && End + NTFS_SDS_MIRROR + EntryLength <= AttributeAllocatedLength(Sds->pRecord))
        WriteAttribute(Vcb, Sds, End + NTFS_SDS_MIRROR, Entry, EntryLength, &Written, Secure);

    if (NT_SUCCESS(Status))
    {
        LARGE_INTEGER NewSize;
        NewSize.QuadPart = End + EntryLength;
        Sds->pRecord->NonResident.DataSize = NewSize.QuadPart;
        Sds->pRecord->NonResident.InitializedSize = NewSize.QuadPart;
        NtfsStoreAttribute(Vcb, Secure, Sds);

        RtlCopyMemory(Header, Entry, 0x14);
        *(PULONG)Key = *(PULONG)Entry;
        *(PULONG)(Key + 4) = Id;
        NtfsInsertRootEntry(Vcb, NTFS_FILE_SECURE, L"$SDH", Key, 8, Header, 0x14);
        *(PULONG)Key = Id;
        NtfsInsertRootEntry(Vcb, NTFS_FILE_SECURE, L"$SII", Key, 4, Header, 0x14);
        *SecurityId = Id;
    }

    ExFreePoolWithTag(Entry, TAG_NTFS);
    ReleaseAttributeContext(Sds);
    NtfsFreeRecord(Vcb, Secure);
    return Status;
}

NTSTATUS
NtfsAssignSecurity(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   ULONGLONG ParentMftIndex,
                   PSECURITY_DESCRIPTOR Explicit,
                   BOOLEAN IsDirectory)
{
    PSTANDARD_INFORMATION Info;
    ULONG InfoLength = 0;
    const UCHAR *Body = NULL;
    ULONG BodyLength = 0;
    ULONG SecurityId = NTFS_SECURITY_ID;
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR Parent = NULL;

    UNREFERENCED_PARAMETER(IsDirectory);

    Info = NtfsFindStandardInformation(Vcb, FileRecord, &InfoLength);
    if (Info == NULL || InfoLength < NTFS_STD_INFO_V31_SIZE)
    {
        NtfsCopyParentSecurityId(Vcb, FileRecord, ParentMftIndex);
        return STATUS_SUCCESS;
    }

    if (Explicit != NULL && RtlValidSecurityDescriptor(Explicit))
    {
        BodyLength = RtlLengthSecurityDescriptor(Explicit);
        Body = (const UCHAR *)Explicit;
    }
    else
    {
        NtfsCopyParentSecurityId(Vcb, FileRecord, ParentMftIndex);
        Status = STATUS_SUCCESS;
        Info = NtfsFindStandardInformation(Vcb, FileRecord, &InfoLength);
        if (Info == NULL)
            return STATUS_SUCCESS;
        /* The copied id already names a descriptor in $Secure. */
        return STATUS_SUCCESS;
    }

    Status = NtfsLookupSecurity(Vcb, Body, BodyLength, &SecurityId);
    if (!NT_SUCCESS(Status))
        Status = NtfsAppendSecurity(Vcb, Body, BodyLength, &SecurityId);
    if (!NT_SUCCESS(Status))
    {
        NtfsCopyParentSecurityId(Vcb, FileRecord, ParentMftIndex);
        return STATUS_SUCCESS;
    }

    Info->SecurityId = SecurityId;
    if (Info->OwnerId == 0)
        Info->OwnerId = 1;
    UNREFERENCED_PARAMETER(Parent);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsStampStandard(PDEVICE_EXTENSION Vcb,
                  PFILE_RECORD_HEADER Record,
                  ULONGLONG Usn,
                  ULONGLONG Charge)
{
    PSTANDARD_INFORMATION Info;
    ULONG Length = 0;

    Info = NtfsFindStandardInformation(Vcb, Record, &Length);
    if (Info == NULL || Length < NTFS_STD_INFO_V31_SIZE)
        return STATUS_SUCCESS;
    if (Usn != 0)
        Info->Usn = Usn;
    if (Charge != 0)
        Info->QuotaCharge = Charge;
    if (Info->OwnerId == 0)
        Info->OwnerId = 1;
    return STATUS_SUCCESS;
}

static
BOOLEAN
NtfsResidentValueInRecord(PNTFS_ATTR_RECORD Attr)
{
    /* Same resident clamp as NtfsFileRecordIsConsistent (NTFS-ATTR-001). */
    if (Attr->IsNonResident != 0)
        return FALSE;
    if (Attr->Length < 24)
        return FALSE;
    if (Attr->Resident.ValueLength > Attr->Length)
        return FALSE;
    if (Attr->Resident.ValueOffset > Attr->Length - Attr->Resident.ValueLength)
        return FALSE;
    return TRUE;
}

NTSTATUS
NtfsQueryObjectId(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG OutLength;
    ULONG Copy;
    NTSTATUS Status;
    UCHAR ObjectId[64];

    OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb, Record, AttributeObjectId, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        ULONGLONG Extend;
        UCHAR Key[16];
        UCHAR Data[56];

        RtlZeroMemory(ObjectId, sizeof(ObjectId));
        *(PULONGLONG)ObjectId = Fcb->MFTIndex;
        *(PULONGLONG)(ObjectId + 8) = KeQueryInterruptTime();
        Status = NtfsAddResidentAttribute(Vcb, Record, AttributeObjectId, NULL, 0, ObjectId, 64);
        if (!NT_SUCCESS(Status))
        {
            NtfsFreeRecord(Vcb, Record);
            return Status;
        }
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
        if (NT_SUCCESS(NtfsFindExtend(Vcb, L"$ObjId", &Extend)))
        {
            RtlCopyMemory(Key, ObjectId, 16);
            RtlZeroMemory(Data, sizeof(Data));
            *(PULONGLONG)Data = Fcb->MFTIndex;
            NtfsInsertRootEntry(Vcb, Extend, L"$O", Key, 16, Data, 56);
        }
    }
    else
    {
        if (!NtfsResidentValueInRecord(Context->pRecord))
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        Copy = min(Context->pRecord->Resident.ValueLength, sizeof(ObjectId));
        RtlZeroMemory(ObjectId, sizeof(ObjectId));
        RtlCopyMemory(ObjectId, (PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset, Copy);
        ReleaseAttributeContext(Context);
    }

    Copy = min(OutLength, sizeof(FILE_OBJECTID_BUFFER));
    if (Irp->AssociatedIrp.SystemBuffer != NULL && Copy != 0)
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, ObjectId, Copy);
    Irp->IoStatus.Information = Copy;
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsAddResidentAttribute(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER Record,
                         ULONG Type,
                         PCWSTR Name,
                         UCHAR NameLength,
                         const UCHAR *Value,
                         ULONG ValueLength)
{
    PNTFS_ATTR_RECORD Attribute;
    ULONG Header;
    ULONG ValueOffset;
    ULONG AttributeLength;
    ULONG NameBytes = NameLength * sizeof(WCHAR);

    UNREFERENCED_PARAMETER(Vcb);
    Header = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ValueOffset = ALIGN_UP_BY(Header + NameBytes, VALUE_OFFSET_ALIGNMENT);
    AttributeLength = ALIGN_UP_BY(ValueOffset + ValueLength, ATTR_RECORD_ALIGNMENT);
    if (Record->BytesInUse + AttributeLength > Record->BytesAllocated)
        return STATUS_DISK_FULL;

    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset);
    while (Attribute->Type != AttributeEnd && Attribute->Length != 0)
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);

    RtlZeroMemory(Attribute, AttributeLength);
    Attribute->Type = Type;
    Attribute->Length = AttributeLength;
    Attribute->NameLength = NameLength;
    Attribute->NameOffset = (USHORT)Header;
    Attribute->Instance = Record->NextAttributeNumber++;
    Attribute->Resident.ValueLength = ValueLength;
    Attribute->Resident.ValueOffset = (USHORT)ValueOffset;
    if (NameBytes != 0)
        RtlCopyMemory((PUCHAR)Attribute + Header, Name, NameBytes);
    RtlCopyMemory((PUCHAR)Attribute + ValueOffset, Value, ValueLength);
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + AttributeLength);
    SetFileRecordEnd(Record, Attribute, FILE_RECORD_END);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsSetReparsePoint(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PREPARSE_DATA_BUFFER Reparse = Buffer;
    NTSTATUS Status;
    ULONGLONG Extend;
    UCHAR Key[12];

    if (Length < REPARSE_DATA_BUFFER_HEADER_SIZE || Reparse->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE > Length)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsAddResidentAttribute(Vcb, Record, AttributeReparsePoint, NULL, 0, Buffer, Length);
    if (NT_SUCCESS(Status))
    {
        PSTANDARD_INFORMATION Info;
        ULONG InfoLength = 0;
        Info = NtfsFindStandardInformation(Vcb, Record, &InfoLength);
        if (Info != NULL)
            Info->FileAttribute |= NTFS_FILE_TYPE_REPARSE;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_REPARSE;
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
    }
    NtfsFreeRecord(Vcb, Record);
    if (!NT_SUCCESS(Status))
        return Status;

    if (NT_SUCCESS(NtfsFindExtend(Vcb, L"$Reparse", &Extend)))
    {
        *(PULONG)Key = Reparse->ReparseTag;
        *(PULONGLONG)(Key + 4) = Fcb->MFTIndex;
        NtfsInsertRootEntry(Vcb, Extend, L"$R", Key, 12, (PUCHAR)&Fcb->MFTIndex, sizeof(ULONGLONG));
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsGetReparsePoint(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG OutLength;
    ULONG Have;
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeReparsePoint, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (!NtfsResidentValueInRecord(Context->pRecord))
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Have = Context->pRecord->Resident.ValueLength;
    OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    if (OutLength < Have)
        Status = STATUS_BUFFER_OVERFLOW;
    else
        Status = STATUS_SUCCESS;
    if (Irp->AssociatedIrp.SystemBuffer != NULL)
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                      (PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset,
                      min(OutLength, Have));
    Irp->IoStatus.Information = min(OutLength, Have);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsDeleteReparsePoint(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_REPARSE;
    return STATUS_SUCCESS;
}

ULONG
NtfsGetReparseTag(PDEVICE_EXTENSION Vcb, ULONGLONG MftIndex)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG Tag = 0;

    if (!NT_SUCCESS(NtfsReadRecord(Vcb, MftIndex, &Record)))
        return 0;
    if (NT_SUCCESS(FindAttribute(Vcb, Record, AttributeReparsePoint, L"", 0, &Context, NULL)))
    {
        if (NtfsResidentValueInRecord(Context->pRecord) &&
            Context->pRecord->Resident.ValueLength >= sizeof(ULONG))
            Tag = *(PULONG)((PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset);
        ReleaseAttributeContext(Context);
    }
    NtfsFreeRecord(Vcb, Record);
    return Tag;
}

NTSTATUS
NtfsGrowQuotaIndexForTrack(PDEVICE_EXTENSION Vcb)
{
    ULONGLONG QuotaIndex;
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_RECORD Slot;
    PUCHAR InRoot;
    ULONG InLength;
    PUCHAR OutRoot = NULL;
    SIZE_T OutLength = 0;
    SIZE_T OutCap = 1024;
    NTSTATUS Status;
    LARGE_INTEGER Now;
    PSID Sid101;
    PSID Sid102 = NULL;
    ULONG Sid101Len;
    ULONG Sid102Len;
    SECURITY_SUBJECT_CONTEXT Subject;
    PACCESS_TOKEN Token;
    PTOKEN_USER UserInfo = NULL;
    BOOLEAN Captured = FALSE;
    ULONG ValueEnd;
    ULONG NeedExtra;
    ULONG Spare;

    Status = NtfsFindExtend(Vcb, L"$Quota", &QuotaIndex);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsReadRecord(Vcb, QuotaIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Type == AttributeIndexRoot &&
            Slot->NameLength == 2 &&
            RtlCompareMemory((PWCHAR)((PUCHAR)Slot + Slot->NameOffset), L"$Q", 4) == 4)
        {
            break;
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }
    if (Slot->Type != AttributeIndexRoot || Slot->IsNonResident)
    {
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_FOUND;
    }

    InRoot = (PUCHAR)Slot + Slot->Resident.ValueOffset;
    InLength = Slot->Resident.ValueLength;
    if (InLength >= 332)
    {
        /* Already large enough for absolute 316/324 (Rev.3 post-track). */
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }

    Sid101 = SeExports->SeLocalSystemSid;
    Sid101Len = RtlLengthSid(Sid101);

    SeCaptureSubjectContext(&Subject);
    Captured = TRUE;
    Token = SeQuerySubjectContextToken(&Subject);
    Status = SeQueryInformationToken(Token, TokenUser, (PVOID *)&UserInfo);
    if (NT_SUCCESS(Status) && UserInfo != NULL && UserInfo->User.Sid != NULL)
    {
        Sid102 = UserInfo->User.Sid;
        Sid102Len = RtlLengthSid(Sid102);
    }
    else
    {
        Sid102 = SeExports->SeAliasAdminsSid;
        Sid102Len = RtlLengthSid(Sid102);
        Status = STATUS_SUCCESS;
    }

    OutRoot = ExAllocatePoolWithTag(NonPagedPool, OutCap, TAG_NTFS);
    if (OutRoot == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    KeQuerySystemTime(&Now);
    Status = ntfs_quota_grow_for_track(InRoot,
                                       InLength,
                                       (const UCHAR *)Sid101,
                                       Sid101Len,
                                       (const UCHAR *)Sid102,
                                       Sid102Len,
                                       Now.QuadPart,
                                       OutRoot,
                                       OutCap,
                                       &OutLength);
    if (!NT_SUCCESS(Status))
        goto Done;

    if (OutLength <= InLength)
    {
        RtlCopyMemory(InRoot, OutRoot, OutLength);
        Slot->Resident.ValueLength = (ULONG)OutLength;
        Status = UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
        goto Done;
    }

    /* Grow the resident $Q attribute in-record (same slack move as view insert). */
    ValueEnd = Slot->Resident.ValueOffset + Slot->Resident.ValueLength;
    NeedExtra = ALIGN_UP_BY((ULONG)OutLength - InLength, 8);
    {
        ULONG Slack = (Slot->Length > ValueEnd) ? Slot->Length - ValueEnd : 0;
        ULONG FromRecord = (NeedExtra > Slack) ? ALIGN_UP_BY(NeedExtra - Slack, 8) : 0;

        Spare = Record->BytesAllocated - Record->BytesInUse;
        if (FromRecord > Spare)
        {
            Status = STATUS_DISK_FULL;
            goto Done;
        }
        if (FromRecord != 0)
        {
            PUCHAR Next = (PUCHAR)Slot + Slot->Length;
            ULONG Tail = Record->BytesInUse - (ULONG)(Next - (PUCHAR)Record);

            RtlMoveMemory(Next + FromRecord, Next, Tail);
            Slot->Length += FromRecord;
            Record->BytesInUse += FromRecord;
        }
    }

    RtlZeroMemory((PUCHAR)Slot + Slot->Resident.ValueOffset + InLength, NeedExtra);
    RtlCopyMemory((PUCHAR)Slot + Slot->Resident.ValueOffset, OutRoot, OutLength);
    Slot->Resident.ValueLength = (ULONG)OutLength;
    Status = UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);

Done:
    if (OutRoot != NULL)
        ExFreePoolWithTag(OutRoot, TAG_NTFS);
    if (UserInfo != NULL)
        ExFreePool(UserInfo);
    if (Captured)
        SeReleaseSubjectContext(&Subject);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

static
NTSTATUS
NtfsFindQuotaQSlot(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER Record,
                   PNTFS_ATTR_RECORD *OutSlot)
{
    PNTFS_ATTR_RECORD Slot;

    *OutSlot = NULL;
    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Type == AttributeIndexRoot &&
            Slot->NameLength == 2 &&
            RtlCompareMemory((PWCHAR)((PUCHAR)Slot + Slot->NameOffset), L"$Q", 4) == 4)
        {
            if (Slot->IsNonResident)
                return STATUS_NOT_IMPLEMENTED;
            *OutSlot = Slot;
            return STATUS_SUCCESS;
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }
    return STATUS_NOT_FOUND;
}

static
NTSTATUS
NtfsReplaceQuotaRoot(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER Record,
                     PNTFS_ATTR_RECORD Slot,
                     PUCHAR NewRoot,
                     ULONG NewLength)
{
    ULONG InLength = Slot->Resident.ValueLength;
    ULONG ValueEnd;
    ULONG NeedExtra;

    if (NewLength <= InLength)
    {
        RtlCopyMemory((PUCHAR)Slot + Slot->Resident.ValueOffset, NewRoot, NewLength);
        Slot->Resident.ValueLength = NewLength;
        return UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
    }

    ValueEnd = Slot->Resident.ValueOffset + Slot->Resident.ValueLength;
    NeedExtra = ALIGN_UP_BY(NewLength - InLength, 8);
    {
        ULONG Slack = (Slot->Length > ValueEnd) ? Slot->Length - ValueEnd : 0;
        ULONG FromRecord = (NeedExtra > Slack) ? ALIGN_UP_BY(NeedExtra - Slack, 8) : 0;
        ULONG Spare = Record->BytesAllocated - Record->BytesInUse;

        if (FromRecord > Spare)
            return STATUS_DISK_FULL;
        if (FromRecord != 0)
        {
            PUCHAR Next = (PUCHAR)Slot + Slot->Length;
            ULONG Tail = Record->BytesInUse - (ULONG)(Next - (PUCHAR)Record);

            RtlMoveMemory(Next + FromRecord, Next, Tail);
            Slot->Length += FromRecord;
            Record->BytesInUse += FromRecord;
        }
    }

    RtlZeroMemory((PUCHAR)Slot + Slot->Resident.ValueOffset + InLength, NeedExtra);
    RtlCopyMemory((PUCHAR)Slot + Slot->Resident.ValueOffset, NewRoot, NewLength);
    Slot->Resident.ValueLength = NewLength;
    return UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
}

NTSTATUS
NtfsSetQuota(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length)
{
    PFILE_QUOTA_INFORMATION Entry;
    ULONGLONG QuotaIndex;
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_RECORD Slot = NULL;
    PUCHAR Root;
    ULONG RootLength;
    NTSTATUS Status;
    ULONG Consumed = 0;
    BOOLEAN Dirty = FALSE;

    /* FILE_QUOTA_INFORMATION — MS-FSCC 2.4.41 (Learn URL in file header). */
    if (Buffer == NULL || Length < FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsFindExtend(Vcb, L"$Quota", &QuotaIndex);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsReadRecord(Vcb, QuotaIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsFindQuotaQSlot(Vcb, Record, &Slot);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Root = (PUCHAR)Slot + Slot->Resident.ValueOffset;
    RootLength = Slot->Resident.ValueLength;

    while (Consumed + FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) <= Length)
    {
        ULONG SidLen;

        Entry = (PFILE_QUOTA_INFORMATION)((PUCHAR)Buffer + Consumed);
        SidLen = Entry->SidLength;
        if (SidLen == 0 && RtlValidSid(&Entry->Sid))
            SidLen = RtlLengthSid(&Entry->Sid);
        if (SidLen == 0)
        {
            NtfsFreeRecord(Vcb, Record);
            return STATUS_INVALID_PARAMETER;
        }

        /* QuotaLimit -2: clear thr/lim to -1 by SID (probe-v4 / Rev.4–8). */
        if (Entry->QuotaLimit.QuadPart == -2LL)
        {
            Status = ntfs_quota_clear_for_delete(Root,
                                                 RootLength,
                                                 (const UCHAR *)&Entry->Sid,
                                                 SidLen);
            if (!NT_SUCCESS(Status))
            {
                NtfsFreeRecord(Vcb, Record);
                return Status;
            }
            Dirty = TRUE;
        }
        else
        {
            /* SID match only (owner_key 0). Absolute 316/324 as fallback. */
            Status = ntfs_quota_write_entry_threshold_limit(Root,
                                                            RootLength,
                                                            0,
                                                            (const UCHAR *)&Entry->Sid,
                                                            SidLen,
                                                            Entry->QuotaThreshold.QuadPart,
                                                            Entry->QuotaLimit.QuadPart);
            if (Status == STATUS_NOT_SUPPORTED && RootLength >= 332)
            {
                Status = ntfs_quota_write_threshold_limit(Root,
                                                          RootLength,
                                                          Entry->QuotaThreshold.QuadPart,
                                                          Entry->QuotaLimit.QuadPart);
            }
            if (Status == STATUS_NOT_SUPPORTED)
            {
                ULONG MaxKey = 0;
                ULONG NewKey;
                PUCHAR OutRoot = NULL;
                SIZE_T OutCap = RootLength + 512;
                SIZE_T OutLength = 0;
                LARGE_INTEGER Now;

                Status = ntfs_quota_max_owner_key(Root, RootLength, &MaxKey);
                if (!NT_SUCCESS(Status))
                {
                    NtfsFreeRecord(Vcb, Record);
                    return Status;
                }
                NewKey = MaxKey + 1;
                if (NewKey < 0x103)
                    NewKey = 0x103;

                OutRoot = ExAllocatePoolWithTag(NonPagedPool, OutCap, TAG_NTFS);
                if (OutRoot == NULL)
                {
                    NtfsFreeRecord(Vcb, Record);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                KeQuerySystemTime(&Now);
                Status = ntfs_quota_append_entry(Root,
                                                 RootLength,
                                                 NewKey,
                                                 (const UCHAR *)&Entry->Sid,
                                                 SidLen,
                                                 Entry->QuotaThreshold.QuadPart,
                                                 Entry->QuotaLimit.QuadPart,
                                                 Now.QuadPart,
                                                 OutRoot,
                                                 OutCap,
                                                 &OutLength);
                if (!NT_SUCCESS(Status))
                {
                    ExFreePoolWithTag(OutRoot, TAG_NTFS);
                    NtfsFreeRecord(Vcb, Record);
                    return Status;
                }
                Status = NtfsReplaceQuotaRoot(Vcb, Record, Slot, OutRoot, (ULONG)OutLength);
                ExFreePoolWithTag(OutRoot, TAG_NTFS);
                if (!NT_SUCCESS(Status))
                {
                    NtfsFreeRecord(Vcb, Record);
                    return Status;
                }
                /* Slot moved if attribute grew; re-find and continue. */
                Status = NtfsFindQuotaQSlot(Vcb, Record, &Slot);
                if (!NT_SUCCESS(Status))
                {
                    NtfsFreeRecord(Vcb, Record);
                    return Status;
                }
                Root = (PUCHAR)Slot + Slot->Resident.ValueOffset;
                RootLength = Slot->Resident.ValueLength;
                Dirty = FALSE; /* already written */
            }
            else if (!NT_SUCCESS(Status))
            {
                NtfsFreeRecord(Vcb, Record);
                return Status;
            }
            else
            {
                Dirty = TRUE;
            }
        }

        if (Entry->NextEntryOffset == 0)
            break;
        if (Entry->NextEntryOffset < FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) ||
            Consumed + Entry->NextEntryOffset > Length)
        {
            NtfsFreeRecord(Vcb, Record);
            return STATUS_INVALID_PARAMETER;
        }
        Consumed += Entry->NextEntryOffset;
    }

    if (Dirty)
        Status = UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
    else
        Status = STATUS_SUCCESS;

    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsQueryQuota(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length, PULONG_PTR Information)
{
    ULONGLONG QuotaIndex;
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_RECORD Slot = NULL;
    SIZE_T OutLength = 0;
    NTSTATUS Status;

    if (Information != NULL)
        *Information = 0;

    if (Buffer == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsFindExtend(Vcb, L"$Quota", &QuotaIndex);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsReadRecord(Vcb, QuotaIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsFindQuotaQSlot(Vcb, Record, &Slot);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Status = ntfs_quota_query((const UCHAR *)Slot + Slot->Resident.ValueOffset,
                              Slot->Resident.ValueLength,
                              Buffer,
                              Length,
                              &OutLength);
    NtfsFreeRecord(Vcb, Record);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Information != NULL)
        *Information = OutLength;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsSetQuotaInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PDEVICE_EXTENSION DeviceExt;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    ASSERT(IrpContext);
    Irp = IrpContext->Irp;
    DeviceExt = IrpContext->DeviceObject->DeviceExtension;
    Stack = IrpContext->Stack;

    if (!ExAcquireResourceExclusiveLite(&DeviceExt->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    Status = NtfsSetQuota(DeviceExt,
                          Irp->AssociatedIrp.SystemBuffer,
                          Stack->Parameters.SetQuota.Length);
    ExReleaseResourceLite(&DeviceExt->DirResource);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    return Status;
}

NTSTATUS
NtfsQueryQuotaInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PDEVICE_EXTENSION DeviceExt;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;
    ULONG_PTR Information = 0;

    ASSERT(IrpContext);
    Irp = IrpContext->Irp;
    DeviceExt = IrpContext->DeviceObject->DeviceExtension;
    Stack = IrpContext->Stack;

    if (!ExAcquireResourceSharedLite(&DeviceExt->DirResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    Status = NtfsQueryQuota(DeviceExt,
                            Irp->AssociatedIrp.SystemBuffer,
                            Stack->Parameters.QueryQuota.Length,
                            &Information);
    ExReleaseResourceLite(&DeviceExt->DirResource);

    /* IRP_INPUT_OPERATION copies SystemBuffer to UserBuffer on complete. */
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    return Status;
}

NTSTATUS
NtfsSetSparse(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PFILE_SET_SPARSE_BUFFER Request = Buffer;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;
    BOOLEAN Set = TRUE;

    if (Length >= sizeof(FILE_SET_SPARSE_BUFFER) && Request != NULL)
        Set = Request->SetSparse;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Set)
    {
        PSTANDARD_INFORMATION Info;
        ULONG InfoLength = 0;

        Context->pRecord->Flags |= NTFS_ATTR_SPARSE;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_SPARSE;
        /* MS-FSA / ntfs-host-mount-corruption.md Rev.2: SI sparse bit with ATTR. */
        Info = NtfsFindStandardInformation(Vcb, Record, &InfoLength);
        if (Info != NULL)
            Info->FileAttribute |= NTFS_FILE_TYPE_SPARSE;
    }
    else
    {
        PSTANDARD_INFORMATION Info;
        ULONG InfoLength = 0;

        Context->pRecord->Flags &= ~NTFS_ATTR_SPARSE;
        Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_SPARSE;
        Info = NtfsFindStandardInformation(Vcb, Record, &InfoLength);
        if (Info != NULL)
            Info->FileAttribute &= ~NTFS_FILE_TYPE_SPARSE;
    }
    Status = NtfsStoreAttribute(Vcb, Record, Context);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsSetZeroData(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PFILE_ZERO_DATA_INFORMATION Zero = Buffer;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;
    LONGLONG Vbn;
    LONGLONG Lbn;
    LONGLONG Count;

    if (Length < sizeof(FILE_ZERO_DATA_INFORMATION) || Zero->BeyondFinalZero.QuadPart <= Zero->FileOffset.QuadPart)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Context->pRecord->IsNonResident && (Context->pRecord->Flags & NTFS_ATTR_SPARSE))
    {
        ULONGLONG Start = Zero->FileOffset.QuadPart / Vcb->NtfsInfo.BytesPerCluster;
        ULONGLONG End = (Zero->BeyondFinalZero.QuadPart + Vcb->NtfsInfo.BytesPerCluster - 1) / Vcb->NtfsInfo.BytesPerCluster;
        LONGLONG SavedV[64];
        LONGLONG SavedL[64];
        LONGLONG SavedC[64];
        ULONG Saved = 0;
        ULONG Index = 0;
        ULONG Lowest = MAXULONG;

        while (Saved < 64 &&
               FsRtlGetNextLargeMcbEntry(&Context->DataRunsMCB, Index, &Vbn, &Lbn, &Count))
        {
            SavedV[Saved] = Vbn;
            SavedL[Saved] = Lbn;
            SavedC[Saved] = Count;
            Saved++;
            Index++;
        }
        FsRtlTruncateLargeMcb(&Context->DataRunsMCB, 0);
        for (Index = 0; Index < Saved; Index++)
        {
            ULONGLONG RunStart = (ULONGLONG)SavedV[Index];
            ULONGLONG RunCount = (ULONGLONG)SavedC[Index];
            ULONGLONG RunEnd = RunStart + RunCount;
            ULONGLONG Keep;

            if (RunEnd <= Start || RunStart >= End)
            {
                FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], SavedC[Index]);
                continue;
            }
            if (RunStart < Start)
                FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], Start - RunStart);
            if (RunEnd > End)
            {
                Keep = End - RunStart;
                FsRtlAddLargeMcbEntry(&Context->DataRunsMCB,
                                      End,
                                      SavedL[Index] + (LONGLONG)Keep,
                                      RunEnd - End);
            }
            for (Keep = max(RunStart, Start); Keep < min(RunEnd, End); Keep++)
            {
                ULONGLONG Bit = (ULONGLONG)SavedL[Index] + (Keep - RunStart);
                if (NT_SUCCESS(NtfsBringClusterWindow(Vcb, Bit)))
                {
                    ULONG Base = (ULONG)(Vcb->ClusterWindowBase * 8);
                    RtlClearBits(&Vcb->ClusterBitmap, (ULONG)Bit - Base, 1);
                    Vcb->ClusterWindowDirty = TRUE;
                    Vcb->FreeClusterCount++;
                    if ((ULONG)Bit < Lowest)
                        Lowest = (ULONG)Bit;
                }
            }
        }
        if (Lowest != MAXULONG)
            NtfsWriteVolumeBitmap(Vcb, Lowest, 1);
        if (Context->pRecord->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
            Context->pRecord->NonResident.CompressedSize = 0;
        {
            PUCHAR Pairs = (PUCHAR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset;
            ULONG Room = Context->pRecord->Length - Context->pRecord->NonResident.MappingPairsOffset;
            ULONG Used = 0;
            ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                      Context->pRecord->NonResident.HighestVCN + 1,
                                      Pairs,
                                      Room,
                                      &Used);
        }
        Status = NtfsStoreAttribute(Vcb, Record, Context);
    }
    else
        Status = STATUS_INVALID_PARAMETER;

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

#ifndef COMPRESSION_FORMAT_LZNT1
#define COMPRESSION_FORMAT_NONE  0x0000
#define COMPRESSION_FORMAT_LZNT1 0x0002
#endif

NTSTATUS
NtfsSetCompression(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    USHORT Format = COMPRESSION_FORMAT_NONE;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;

    if (Length >= sizeof(USHORT) && Buffer != NULL)
        Format = *(PUSHORT)Buffer;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Format == COMPRESSION_FORMAT_LZNT1)
    {
        if (Context->pRecord->IsNonResident)
            Context->pRecord->NonResident.CompressionUnit = 4;
        Context->pRecord->Flags |= 0x0001;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_COMPRESSED;
    }
    else if (Format == COMPRESSION_FORMAT_NONE)
    {
        if (Context->pRecord->IsNonResident)
            Context->pRecord->NonResident.CompressionUnit = 0;
        Context->pRecord->Flags &= ~NTFS_ATTR_COMPRESSION_MASK;
        Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_COMPRESSED;
    }
    else
        Status = STATUS_NOT_SUPPORTED;

    if (NT_SUCCESS(Status))
        Status = NtfsStoreAttribute(Vcb, Record, Context);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsGetCompression(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    USHORT Format = COMPRESSION_FORMAT_NONE;
    ULONG OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(Vcb);
    if (OutLength < sizeof(USHORT))
        return STATUS_BUFFER_TOO_SMALL;
    if (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_COMPRESSED)
        Format = COMPRESSION_FORMAT_LZNT1;
    *(PUSHORT)Irp->AssociatedIrp.SystemBuffer = Format;
    Irp->IoStatus.Information = sizeof(USHORT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsOpenJournal(PDEVICE_EXTENSION Vcb, PCWSTR Stream, PNTFS_ATTR_CONTEXT *Context, PFILE_RECORD_HEADER *Record)
{
    ULONGLONG Index;
    NTSTATUS Status;

    Status = NtfsFindExtend(Vcb, L"$UsnJrnl", &Index);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtfsReadRecord(Vcb, Index, Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, *Record, AttributeData, Stream, (ULONG)wcslen(Stream), Context, NULL);
    if (!NT_SUCCESS(Status))
        NtfsFreeRecord(Vcb, *Record);
    return Status;
}

NTSTATUS
NtfsCreateUsnJournal(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length)
{
    PCREATE_USN_JOURNAL_DATA Request = Buffer;
    ULONGLONG Index;
    PFILE_RECORD_HEADER Record;
    UCHAR Max[32];
    NTSTATUS Status;

    if (Length < sizeof(CREATE_USN_JOURNAL_DATA))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsFindExtend(Vcb, L"$UsnJrnl", &Index);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtfsReadRecord(Vcb, Index, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Max, sizeof(Max));
    *(PULONGLONG)Max = Request->MaximumSize;
    *(PULONGLONG)(Max + 8) = Request->AllocationDelta;
    *(PULONGLONG)(Max + 16) = KeQueryInterruptTime();
    Status = NtfsAddResidentAttribute(Vcb, Record, AttributeData, L"$Max", 4, Max, sizeof(Max));
    if (Status == STATUS_OBJECT_NAME_COLLISION || Status == STATUS_DISK_FULL)
        Status = STATUS_SUCCESS;
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Index, Record);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsQueryUsnJournal(PDEVICE_EXTENSION Vcb, PIRP Irp)
{
    PNTFS_ATTR_CONTEXT Context = NULL;
    PFILE_RECORD_HEADER Record = NULL;
    ULONG OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    UCHAR Max[32];
    NTSTATUS Status;

    Status = NtfsOpenJournal(Vcb, L"$Max", &Context, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(Max, sizeof(Max));
    ReadAttribute(Vcb, Context, 0, (PCHAR)Max, min(sizeof(Max), (ULONG)AttributeDataLength(Context->pRecord)));
    if (Irp->AssociatedIrp.SystemBuffer != NULL && OutLength != 0)
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Max, min(OutLength, sizeof(Max)));
    Irp->IoStatus.Information = min(OutLength, sizeof(Max));
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsReadUsnJournal(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length, PIRP Irp)
{
    PREAD_USN_JOURNAL_DATA Request = Buffer;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PFILE_RECORD_HEADER Record = NULL;
    ULONG OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    ULONG Got;
    NTSTATUS Status;

    if (Length < sizeof(READ_USN_JOURNAL_DATA) || OutLength < sizeof(USN))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsOpenJournal(Vcb, L"$J", &Context, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Got = (ULONG)min((ULONGLONG)OutLength - sizeof(USN),
                     AttributeDataLength(Context->pRecord) - (ULONGLONG)Request->StartUsn);
    if (Irp->AssociatedIrp.SystemBuffer != NULL)
    {
        *(USN *)Irp->AssociatedIrp.SystemBuffer = Request->StartUsn + Got;
        if (Got != 0)
            ReadAttribute(Vcb, Context, Request->StartUsn, (PCHAR)Irp->AssociatedIrp.SystemBuffer + sizeof(USN), Got);
    }
    Irp->IoStatus.Information = sizeof(USN) + Got;
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsDeleteUsnJournal(PDEVICE_EXTENSION Vcb)
{
    UNREFERENCED_PARAMETER(Vcb);
    return STATUS_SUCCESS;
}

/* Offset of a named attribute inside a file record, or 0 when it is absent.
 * An offset of 0 is the file-record header, never a real attribute. */
static
ULONG
NtfsAttributeOffset(PFILE_RECORD_HEADER Record,
                    ULONG Type,
                    PCWSTR Name,
                    USHORT NameLength)
{
    PNTFS_ATTR_RECORD Attribute;
    ULONG Offset;

    Offset = Record->AttributeOffset;
    while (Offset + sizeof(ULONG) < Record->BytesAllocated)
    {
        Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Offset);
        if (Attribute->Type == AttributeEnd || Attribute->Length < 16)
            break;

        if (Attribute->Type == Type && Attribute->NameLength == NameLength &&
            (NameLength == 0 ||
             RtlCompareMemory((PUCHAR)Attribute + Attribute->NameOffset,
                              Name,
                              NameLength * sizeof(WCHAR)) == NameLength * sizeof(WCHAR)))
        {
            return Offset;
        }

        Offset += Attribute->Length;
    }

    return 0;
}

NTSTATUS
NtfsAppendUsn(PDEVICE_EXTENSION Vcb,
              ULONGLONG FileReference,
              ULONGLONG ParentReference,
              ULONG Reason,
              ULONG Attributes,
              PCUNICODE_STRING Name)
{
    PNTFS_ATTR_CONTEXT Context = NULL;
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_RECORD Journal;
    USN Next;
    ULONG NameBytes;
    ULONG RecordLength;
    ULONG AttrOffset;
    PUCHAR Raw;
    ULONG Written;
    NTSTATUS Status;

    Status = NtfsOpenJournal(Vcb, L"$J", &Context, &Record);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    /* The file is already in the MFT. A journal problem must not fail the create. */
    AttrOffset = NtfsAttributeOffset(Record, AttributeData, L"$J", 2);
    if (AttrOffset == 0)
    {
        DPRINT1("USN journal $J attribute is missing\n");
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }

    NameBytes = (Name != NULL) ? Name->Length : 0;
    RecordLength = ALIGN_UP_BY(FIELD_OFFSET(USN_RECORD, FileName) + NameBytes, 8);
    Next = AttributeDataLength(Context->pRecord);
    Raw = ExAllocatePoolWithTag(NonPagedPool, RecordLength, TAG_NTFS);
    if (Raw == NULL)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(Raw, RecordLength);
    ((PUSN_RECORD)Raw)->RecordLength = RecordLength;
    ((PUSN_RECORD)Raw)->MajorVersion = 2;
    ((PUSN_RECORD)Raw)->MinorVersion = 0;
    ((PUSN_RECORD)Raw)->FileReferenceNumber = FileReference;
    ((PUSN_RECORD)Raw)->ParentFileReferenceNumber = ParentReference;
    ((PUSN_RECORD)Raw)->Usn = Next;
    KeQuerySystemTime(&((PUSN_RECORD)Raw)->TimeStamp);
    ((PUSN_RECORD)Raw)->Reason = Reason;
    ((PUSN_RECORD)Raw)->FileAttributes = Attributes;
    ((PUSN_RECORD)Raw)->FileNameLength = (USHORT)NameBytes;
    ((PUSN_RECORD)Raw)->FileNameOffset = FIELD_OFFSET(USN_RECORD, FileName);
    if (NameBytes != 0 && Name->Buffer != NULL)
        RtlCopyMemory(((PUSN_RECORD)Raw)->FileName, Name->Buffer, NameBytes);

    if (Context->pRecord->IsNonResident &&
        Next + RecordLength > (ULONGLONG)Context->pRecord->NonResident.AllocatedSize)
    {
        Status = NtfsExtendAllocation(Vcb,
                                      Context,
                                      AttrOffset,
                                      Record,
                                      ROUND_UP(Next + RecordLength, Vcb->NtfsInfo.BytesPerCluster));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("USN journal could not grow (Status %lx)\n", Status);
            ExFreePoolWithTag(Raw, TAG_NTFS);
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_SUCCESS;
        }
    }
    Status = WriteAttribute(Vcb, Context, Next, Raw, RecordLength, &Written, Record);
    if (NT_SUCCESS(Status) && Context->pRecord->IsNonResident)
    {
        Journal = (PNTFS_ATTR_RECORD)((PUCHAR)Record + AttrOffset);
        Journal->NonResident.DataSize = Next + RecordLength;
        Journal->NonResident.InitializedSize = Next + RecordLength;
        Context->pRecord->NonResident.DataSize = Journal->NonResident.DataSize;
        Context->pRecord->NonResident.InitializedSize = Journal->NonResident.InitializedSize;
        UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
    }
    else if (!NT_SUCCESS(Status))
    {
        DPRINT1("USN journal write failed (Status %lx)\n", Status);
    }
    ExFreePoolWithTag(Raw, TAG_NTFS);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);

    if (NT_SUCCESS(Status))
    {
        PFILE_RECORD_HEADER File;
        if (NT_SUCCESS(NtfsReadRecord(Vcb, FileReference & NTFS_MFT_MASK, &File)))
        {
            NtfsStampStandard(Vcb, File, Next, 0);
            UpdateFileRecord(Vcb, FileReference & NTFS_MFT_MASK, File);
            NtfsFreeRecord(Vcb, File);
        }
    }
    return Status;
}

static
BOOLEAN
NtfsUnitClusters(PNTFS_ATTR_RECORD Attribute,
                 PULONG UnitClustersOut)
{
    UCHAR Unit = Attribute->NonResident.CompressionUnit;

    /* NTFS-COMP-001 / ntfs-compression.md Rev.1: accept only units this
     * driver writes (0 or 4) before shift/multiply. */
    if (Unit == 0)
    {
        *UnitClustersOut = 16;
        return TRUE;
    }
    if (Unit == 4)
    {
        *UnitClustersOut = 1u << 4;
        return TRUE;
    }
    *UnitClustersOut = 0;
    return FALSE;
}

NTSTATUS
NtfsReadCompressed(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   PUCHAR Buffer,
                   ULONG Length,
                   ULONGLONG Offset,
                   PULONG LengthRead)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG Cluster;
    ULONG UnitClusters;
    ULONG UnitBytes;
    NTSTATUS Status;

    *LengthRead = 0;
    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }
    if (!Context->pRecord->IsNonResident)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_IMPLEMENTED;
    }

    Cluster = Vcb->NtfsInfo.BytesPerCluster;
    if (!NtfsUnitClusters(Context->pRecord, &UnitClusters) || UnitClusters == 0)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }
    UnitBytes = UnitClusters * Cluster;
    if (UnitBytes == 0)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    while (Length > 0)
    {
        ULONGLONG Unit = Offset / UnitBytes;
        ULONG Skip = (ULONG)(Offset % UnitBytes);
        ULONG Take = min(Length, UnitBytes - Skip);
        PUCHAR Plain;
        PUCHAR Comp;
        ULONG Allocated = 0;
        ULONG Index;
        ULONG Final = 0;
        PVOID Work = NULL;
        ULONG WorkSize = 0;
        ULONG Frag = 0;

        Plain = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        if (Plain == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(Plain, UnitBytes);

        for (Index = 0; Index < UnitClusters; Index++)
        {
            LONGLONG Lbn, Count;
            if (FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB,
                                         Unit * UnitClusters + Index,
                                         &Lbn, &Count, NULL, NULL, NULL) &&
                Lbn != -1)
                Allocated++;
            else
                break;
        }

        if (Allocated == UnitClusters)
        {
            ReadAttribute(Vcb, Context, Unit * UnitBytes, (PCHAR)Plain, UnitBytes);
        }
        else if (Allocated != 0)
        {
            Comp = ExAllocatePoolWithTag(NonPagedPool, Allocated * Cluster, TAG_NTFS);
            if (Comp == NULL)
            {
                ExFreePoolWithTag(Plain, TAG_NTFS);
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            ReadAttribute(Vcb, Context, Unit * UnitBytes, (PCHAR)Comp, Allocated * Cluster);
            RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1, &WorkSize, &Frag);
            if (WorkSize != 0)
                Work = ExAllocatePoolWithTag(NonPagedPool, WorkSize, TAG_NTFS);
            Status = RtlDecompressBuffer(COMPRESSION_FORMAT_LZNT1, Plain, UnitBytes, Comp, Allocated * Cluster, &Final);
            if (Work != NULL)
                ExFreePoolWithTag(Work, TAG_NTFS);
            ExFreePoolWithTag(Comp, TAG_NTFS);
            if (!NT_SUCCESS(Status) && Status != STATUS_BAD_COMPRESSION_BUFFER)
            {
                ExFreePoolWithTag(Plain, TAG_NTFS);
                break;
            }
            Status = STATUS_SUCCESS;
        }

        RtlCopyMemory(Buffer, Plain + Skip, Take);
        ExFreePoolWithTag(Plain, TAG_NTFS);
        Buffer += Take;
        Offset += Take;
        Length -= Take;
        *LengthRead += Take;
    }

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsWriteCompressed(PDEVICE_EXTENSION Vcb,
                    PNTFS_FCB Fcb,
                    const PUCHAR Buffer,
                    ULONG Length,
                    ULONGLONG Offset,
                    PULONG LengthWritten)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG Cluster;
    ULONG UnitClusters;
    ULONG UnitBytes;
    NTSTATUS Status;

    *LengthWritten = 0;
    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Cluster = Vcb->NtfsInfo.BytesPerCluster;
    if (!NtfsUnitClusters(Context->pRecord, &UnitClusters) || UnitClusters == 0)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }
    UnitBytes = UnitClusters * Cluster;
    if (UnitBytes == 0)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    {
    PUCHAR Input = (PUCHAR)Buffer;
    while (Length > 0 && NT_SUCCESS(Status))
    {
        ULONGLONG Unit = Offset / UnitBytes;
        ULONG Skip = (ULONG)(Offset % UnitBytes);
        ULONG Take = min(Length, UnitBytes - Skip);
        PUCHAR Plain;
        PUCHAR Comp;
        ULONG Final = 0;
        ULONG WorkSize = 0;
        ULONG Frag = 0;
        PVOID Work;
        ULONG Written = 0;

        Plain = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        Comp = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        if (Plain == NULL || Comp == NULL)
        {
            if (Plain != NULL)
                ExFreePoolWithTag(Plain, TAG_NTFS);
            if (Comp != NULL)
                ExFreePoolWithTag(Comp, TAG_NTFS);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(Plain, UnitBytes);
        ReadAttribute(Vcb, Context, Unit * UnitBytes, (PCHAR)Plain, UnitBytes);
        RtlCopyMemory(Plain + Skip, Input, Take);

        RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1, &WorkSize, &Frag);
        Work = (WorkSize != 0) ? ExAllocatePoolWithTag(NonPagedPool, WorkSize, TAG_NTFS) : NULL;
        Status = RtlCompressBuffer(COMPRESSION_FORMAT_LZNT1,
                                   Plain,
                                   UnitBytes,
                                   Comp,
                                   UnitBytes - Cluster,
                                   0x1000,
                                   &Final,
                                   Work);
        if (!NT_SUCCESS(Status))
        {
            /* A unit that does not shrink is stored uncompressed. */
            Status = WriteAttribute(Vcb, Context, Unit * UnitBytes, Plain, UnitBytes, &Written, Record);
            Final = UnitBytes;
        }
        else
        {
            ULONG KeepClusters = (Final + Cluster - 1) / Cluster;
            ULONGLONG Tail = Unit * UnitClusters + KeepClusters;
            ULONGLONG UnitEnd = Unit * UnitClusters + UnitClusters;
            LONGLONG Vbn;
            LONGLONG Lbn;
            LONGLONG Count;
            LONGLONG SavedV[32];
            LONGLONG SavedL[32];
            LONGLONG SavedC[32];
            ULONG Saved = 0;
            ULONG Index = 0;

            Status = WriteAttribute(Vcb, Context, Unit * UnitBytes, Comp, Final, &Written, Record);
            while (Saved < 32 &&
                   FsRtlGetNextLargeMcbEntry(&Context->DataRunsMCB, Index, &Vbn, &Lbn, &Count))
            {
                SavedV[Saved] = Vbn;
                SavedL[Saved] = Lbn;
                SavedC[Saved] = Count;
                Saved++;
                Index++;
            }
            FsRtlTruncateLargeMcb(&Context->DataRunsMCB, 0);
            for (Index = 0; Index < Saved && NT_SUCCESS(Status); Index++)
            {
                ULONGLONG RunStart = (ULONGLONG)SavedV[Index];
                ULONGLONG RunEnd = RunStart + (ULONGLONG)SavedC[Index];
                ULONGLONG Bit;

                if (RunEnd <= Tail || RunStart >= UnitEnd)
                {
                    FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], SavedC[Index]);
                    continue;
                }
                if (RunStart < Tail)
                    FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], Tail - RunStart);
                if (RunEnd > UnitEnd)
                    FsRtlAddLargeMcbEntry(&Context->DataRunsMCB,
                                          UnitEnd,
                                          SavedL[Index] + (LONGLONG)(UnitEnd - RunStart),
                                          RunEnd - UnitEnd);
                for (Bit = max(RunStart, Tail); Bit < min(RunEnd, UnitEnd); Bit++)
                {
                    ULONGLONG Disk = (ULONGLONG)SavedL[Index] + (Bit - RunStart);
                    if (NT_SUCCESS(NtfsBringClusterWindow(Vcb, Disk)))
                    {
                        ULONG Base = (ULONG)(Vcb->ClusterWindowBase * 8);
                        RtlClearBits(&Vcb->ClusterBitmap, (ULONG)Disk - Base, 1);
                        Vcb->ClusterWindowDirty = TRUE;
                    }
                }
            }
            if (NT_SUCCESS(Status))
            {
                PUCHAR Pairs = (PUCHAR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset;
                ULONG Room = Context->pRecord->Length - Context->pRecord->NonResident.MappingPairsOffset;
                ULONG Used = 0;
                NtfsWriteVolumeBitmap(Vcb, 0, 1);
                ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                          Context->pRecord->NonResident.HighestVCN + 1,
                                          Pairs,
                                          Room,
                                          &Used);
                NtfsStoreAttribute(Vcb, Record, Context);
            }
        }
        if (Work != NULL)
            ExFreePoolWithTag(Work, TAG_NTFS);
        ExFreePoolWithTag(Plain, TAG_NTFS);
        ExFreePoolWithTag(Comp, TAG_NTFS);
        if (!NT_SUCCESS(Status))
            break;

        Input += Take;
        Offset += Take;
        Length -= Take;
        *LengthWritten += Take;
        UNREFERENCED_PARAMETER(Final);
    }
    }

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

/* probe-v6 / Rev.3: every $EFS is 672; bytes 0..15 fixed; 16+ not invented. */
#define NTFS_EFS_STREAM_LENGTH 672

static const UCHAR NtfsEfsFixedHeader[16] = {
    0xA0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

VOID
NtfsLoadFileKey(PDEVICE_EXTENSION Vcb, PFILE_RECORD_HEADER Record, PNTFS_FCB Fcb)
{
    /* FEK recovery from $EFS offset 16+ is not cited; structural path needs none. */
    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(Record);
    UNREFERENCED_PARAMETER(Fcb);
}

NTSTATUS
NtfsReadEncrypted(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, PUCHAR Buffer, ULONG Length, ULONGLONG Offset, PULONG LengthRead)
{
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONGLONG StreamSize;
    ULONG ToRead;
    NTSTATUS Status;

    if (LengthRead != NULL)
        *LengthRead = 0;
    if (Buffer == NULL || Length == 0)
        return STATUS_SUCCESS;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb,
                           Record,
                           AttributeData,
                           Fcb->Stream,
                           (ULONG)wcslen(Fcb->Stream),
                           &DataContext,
                           NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    StreamSize = AttributeDataLength(DataContext->pRecord);
    if (Offset >= StreamSize)
    {
        ReleaseAttributeContext(DataContext);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_END_OF_FILE;
    }

    ToRead = Length;
    if (Offset + ToRead > StreamSize)
        ToRead = (ULONG)(StreamSize - Offset);

    /* Structural stub: $DATA stays plaintext (no cited cipher). */
    ToRead = ReadAttribute(Vcb, DataContext, Offset, (PCHAR)Buffer, ToRead);
    if (LengthRead != NULL)
        *LengthRead = ToRead;

    ReleaseAttributeContext(DataContext);
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsWriteEncrypted(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, const PUCHAR Buffer, ULONG Length, ULONGLONG Offset, PULONG LengthWritten)
{
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONG AttrOffset = 0;
    ULONG Written = 0;
    LARGE_INTEGER Need;
    NTSTATUS Status;

    if (LengthWritten != NULL)
        *LengthWritten = 0;
    if (Buffer == NULL || Length == 0)
        return STATUS_SUCCESS;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb,
                           Record,
                           AttributeData,
                           Fcb->Stream,
                           (ULONG)wcslen(Fcb->Stream),
                           &DataContext,
                           &AttrOffset);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Need.QuadPart = (LONGLONG)(Offset + Length);
    if ((ULONGLONG)Need.QuadPart > AttributeDataLength(DataContext->pRecord))
    {
        if (DataContext->pRecord->IsNonResident)
            Status = SetNonResidentAttributeDataLength(Vcb, DataContext, AttrOffset, Record, &Need);
        else
            Status = SetResidentAttributeDataLength(Vcb, DataContext, AttrOffset, Record, &Need);
        if (!NT_SUCCESS(Status))
        {
            ReleaseAttributeContext(DataContext);
            NtfsFreeRecord(Vcb, Record);
            return Status;
        }
    }

    Status = WriteAttribute(Vcb, DataContext, Offset, Buffer, Length, &Written, Record);
    if (NT_SUCCESS(Status))
    {
        if (LengthWritten != NULL)
            *LengthWritten = Written;
        if (Offset + Written > (ULONGLONG)Fcb->RFCB.FileSize.QuadPart)
        {
            Fcb->RFCB.FileSize.QuadPart = Offset + Written;
            Fcb->RFCB.ValidDataLength.QuadPart = Fcb->RFCB.FileSize.QuadPart;
        }
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
    }

    ReleaseAttributeContext(DataContext);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

static
NTSTATUS
NtfsEnsureEfsStream(PDEVICE_EXTENSION Vcb, PFILE_RECORD_HEADER Record)
{
    PNTFS_ATTR_CONTEXT Context = NULL;
    PNTFS_ATTR_RECORD Slot;
    ULONG AttrOffset = 0;
    LARGE_INTEGER Size;
    ULONG Written = 0;
    PUCHAR Blob;
    NTSTATUS Status;

    Blob = ExAllocatePoolWithTag(NonPagedPool, NTFS_EFS_STREAM_LENGTH, TAG_NTFS);
    if (Blob == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Blob, NTFS_EFS_STREAM_LENGTH);
    RtlCopyMemory(Blob, NtfsEfsFixedHeader, sizeof(NtfsEfsFixedHeader));

    Status = FindAttribute(Vcb,
                           Record,
                           AttributeLoggedUtilityStream,
                           L"$EFS",
                           4,
                           &Context,
                           &AttrOffset);
    if (!NT_SUCCESS(Status))
    {
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
        while (Slot->Type != AttributeEnd && Slot->Length != 0)
            Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);

        Status = AddLoggedUtilityStream(Vcb, Record, Slot, L"$EFS", 4);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Blob, TAG_NTFS);
            return Status;
        }

        Status = FindAttribute(Vcb,
                               Record,
                               AttributeLoggedUtilityStream,
                               L"$EFS",
                               4,
                               &Context,
                               &AttrOffset);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Blob, TAG_NTFS);
            return Status;
        }
    }

    Size.QuadPart = NTFS_EFS_STREAM_LENGTH;
    if (!Context->pRecord->IsNonResident)
    {
        ReleaseAttributeContext(Context);
        ExFreePoolWithTag(Blob, TAG_NTFS);
        return STATUS_NOT_SUPPORTED;
    }

    if (AttributeDataLength(Context->pRecord) != NTFS_EFS_STREAM_LENGTH)
    {
        Status = SetNonResidentAttributeDataLength(Vcb, Context, AttrOffset, Record, &Size);
        if (!NT_SUCCESS(Status))
        {
            ReleaseAttributeContext(Context);
            ExFreePoolWithTag(Blob, TAG_NTFS);
            return Status;
        }
    }

    Status = WriteAttribute(Vcb, Context, 0, Blob, NTFS_EFS_STREAM_LENGTH, &Written, Record);
    ReleaseAttributeContext(Context);
    ExFreePoolWithTag(Blob, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Written != NTFS_EFS_STREAM_LENGTH)
        return STATUS_UNEXPECTED_IO_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsSetEncryption(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER Record = NULL;
    PENCRYPTION_BUFFER Request = NULL;
    UCHAR Local[4 + 16 + 32];
    PSTANDARD_INFORMATION Info;
    ULONG InfoLength = 0;
    ULONG Op;
    NTSTATUS Status = STATUS_SUCCESS;
    KPROCESSOR_MODE Mode;

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = (PNTFS_FCB)FileObject->FsContext;

    if (Buffer == NULL || Length < sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;

    Mode = ExGetPreviousMode();
    if (Mode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForRead(Buffer, Length, sizeof(UCHAR));
            RtlCopyMemory(Local, Buffer, min(Length, sizeof(Local)));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
            return Status;
        Request = (PENCRYPTION_BUFFER)Local;
    }
    else
    {
        Request = (PENCRYPTION_BUFFER)Buffer;
    }

    Op = Request->EncryptionOperation;
    if (Op != FILE_SET_ENCRYPTION)
        return STATUS_NOT_SUPPORTED;

    /* Optional open-file key from EncryptFileW (4 + 16 + 32). Not used for reopen. */
    if (Length >= 4 + 16 + 32)
    {
        RtlCopyMemory(Fcb->FileKey, (PUCHAR)Request + 4 + 16, 32);
        Fcb->FileKeyLength = 32;
        Fcb->HasFileKey = TRUE;
    }

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsEnsureEfsStream(Vcb, Record);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Info = NtfsFindStandardInformation(Vcb, Record, &InfoLength);
    if (Info != NULL)
        Info->FileAttribute |= NTFS_FILE_TYPE_ENCRYPTED;
    Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_ENCRYPTED;

    Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}
