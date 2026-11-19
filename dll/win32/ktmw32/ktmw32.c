/*
 * PROJECT:     Tavera OS System Libraries
 * LICENSE:     GNU Lesser General Public License, version 2.1 or later (LGPL-2.1-or-later)
 * FILE:        dll/win32/ktmw32/ktmw32.c
 * PURPOSE:     Kernel Transaction Manager user stubs for Firefox xul.dll
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
 *
 * Sources:
 *   https://learn.microsoft.com/en-us/windows/win32/fileio/how-to-use-transactional-ntfs
 *   https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-createtransaction
 *   https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-committransaction
 *   https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-rollbacktransaction
 *   https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiletransactedw
 *   tavera/specs/ntfs/ntfs-txf.md (probe-v10 NTFS-TXF-PROBE-DONE ok).
 * Process-local staging only — no invented $RmMetadata / $Txf* on-disk layouts.
 */

#include <windef.h>
#include <winbase.h>
#include <string.h>

#define TXF_MAGIC 0x31584654UL /* 'TXF1' */
#define TXF_MAX_PENDING 32
#define TXF_MAP_CAP 64

typedef struct _TXF_PENDING
{
    WCHAR Final[MAX_PATH];
    WCHAR Staging[MAX_PATH];
} TXF_PENDING;

typedef struct _TXF_STATE
{
    ULONG Magic;
    CRITICAL_SECTION Lock;
    ULONG PendingCount;
    TXF_PENDING Pending[TXF_MAX_PENDING];
    BOOLEAN Finished;
} TXF_STATE, *PTXF_STATE;

typedef struct _TXF_MAP_ENTRY
{
    HANDLE Handle;
    PTXF_STATE State;
} TXF_MAP_ENTRY;

static CRITICAL_SECTION TxfMapLock;
static BOOLEAN TxfMapReady;
static TXF_MAP_ENTRY TxfMap[TXF_MAP_CAP];
static ULONG TxfMapCount;

static void
TxfEnsureMap(void)
{
    if (!TxfMapReady)
    {
        InitializeCriticalSection(&TxfMapLock);
        TxfMapReady = TRUE;
    }
}

static PTXF_STATE
TxfLookup(HANDLE TransactionHandle)
{
    ULONG i;
    PTXF_STATE State = NULL;

    TxfEnsureMap();
    EnterCriticalSection(&TxfMapLock);
    for (i = 0; i < TxfMapCount; i++)
    {
        if (TxfMap[i].Handle == TransactionHandle)
        {
            State = TxfMap[i].State;
            break;
        }
    }
    LeaveCriticalSection(&TxfMapLock);
    return State;
}

static BOOL
TxfMapAdd(HANDLE Handle, PTXF_STATE State)
{
    BOOL Ok = FALSE;

    TxfEnsureMap();
    EnterCriticalSection(&TxfMapLock);
    if (TxfMapCount < TXF_MAP_CAP)
    {
        TxfMap[TxfMapCount].Handle = Handle;
        TxfMap[TxfMapCount].State = State;
        TxfMapCount++;
        Ok = TRUE;
    }
    LeaveCriticalSection(&TxfMapLock);
    return Ok;
}

static void
TxfMapRemove(HANDLE Handle)
{
    ULONG i;

    TxfEnsureMap();
    EnterCriticalSection(&TxfMapLock);
    for (i = 0; i < TxfMapCount; i++)
    {
        if (TxfMap[i].Handle == Handle)
        {
            TxfMap[i] = TxfMap[TxfMapCount - 1];
            TxfMapCount--;
            break;
        }
    }
    LeaveCriticalSection(&TxfMapLock);
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreateTransaction(LPSECURITY_ATTRIBUTES lpTransactionAttributes,
                  LPGUID UOW,
                  DWORD CreateOptions,
                  DWORD IsolationLevel,
                  DWORD IsolationFlags,
                  DWORD Timeout,
                  LPWSTR Description)
{
    HANDLE Event;
    PTXF_STATE State;

    UNREFERENCED_PARAMETER(lpTransactionAttributes);
    UNREFERENCED_PARAMETER(UOW);
    UNREFERENCED_PARAMETER(CreateOptions);
    UNREFERENCED_PARAMETER(IsolationLevel);
    UNREFERENCED_PARAMETER(IsolationFlags);
    UNREFERENCED_PARAMETER(Timeout);
    UNREFERENCED_PARAMETER(Description);

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Event == NULL)
        return INVALID_HANDLE_VALUE;

    State = (PTXF_STATE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*State));
    if (State == NULL)
    {
        CloseHandle(Event);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    State->Magic = TXF_MAGIC;
    InitializeCriticalSection(&State->Lock);
    if (!TxfMapAdd(Event, State))
    {
        DeleteCriticalSection(&State->Lock);
        HeapFree(GetProcessHeap(), 0, State);
        CloseHandle(Event);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    return Event;
}

static BOOL
TxfFinish(HANDLE TransactionHandle, BOOL Commit)
{
    PTXF_STATE State;
    ULONG i;

    State = TxfLookup(TransactionHandle);
    if (State == NULL || State->Magic != TXF_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    EnterCriticalSection(&State->Lock);
    if (State->Finished)
    {
        LeaveCriticalSection(&State->Lock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    for (i = 0; i < State->PendingCount; i++)
    {
        if (Commit)
        {
            if (!MoveFileExW(State->Pending[i].Staging,
                             State->Pending[i].Final,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            {
                /* Best-effort: leave remaining staged files for rollback path. */
                LeaveCriticalSection(&State->Lock);
                return FALSE;
            }
        }
        else
        {
            DeleteFileW(State->Pending[i].Staging);
        }
    }

    State->PendingCount = 0;
    State->Finished = TRUE;
    LeaveCriticalSection(&State->Lock);
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CommitTransaction(HANDLE TransactionHandle)
{
    return TxfFinish(TransactionHandle, TRUE);
}

/*
 * @implemented
 */
BOOL
WINAPI
RollbackTransaction(HANDLE TransactionHandle)
{
    return TxfFinish(TransactionHandle, FALSE);
}

/*
 * @implemented
 * Called from kernel32 CreateFileTransactedW after creating the staging file.
 */
BOOL
WINAPI
KtmRegisterTransactedFile(HANDLE TransactionHandle,
                          LPCWSTR FinalPath,
                          LPCWSTR StagingPath)
{
    PTXF_STATE State;
    ULONG Idx;

    if (TransactionHandle == NULL || FinalPath == NULL || StagingPath == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    State = TxfLookup(TransactionHandle);
    if (State == NULL || State->Magic != TXF_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    EnterCriticalSection(&State->Lock);
    if (State->Finished || State->PendingCount >= TXF_MAX_PENDING)
    {
        LeaveCriticalSection(&State->Lock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    Idx = State->PendingCount++;
    wcsncpy(State->Pending[Idx].Final, FinalPath, MAX_PATH - 1);
    State->Pending[Idx].Final[MAX_PATH - 1] = 0;
    wcsncpy(State->Pending[Idx].Staging, StagingPath, MAX_PATH - 1);
    State->Pending[Idx].Staging[MAX_PATH - 1] = 0;
    LeaveCriticalSection(&State->Lock);
    return TRUE;
}
