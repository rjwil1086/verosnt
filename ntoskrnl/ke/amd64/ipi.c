/*
 * PROJECT:     Tavera OS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * FILE:        ntoskrnl/ke/amd64/ipi.c
 * PURPOSE:     IPI code for x64
 * COPYRIGHT:   Copyright 2023 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Ryan Wilson <ryan@taveraos.com>
 *
 * Sources: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keipigenericcall
 * (broadcast worker, barrier, return value from caller, IRQL below IPI_LEVEL);
 * in-tree ntoskrnl/ke/ipi.c (KiReverseStallIpiLock / raise pattern);
 * sdk/include/ndk/amd64/ketypes.h (IPI_APC/DPC/FREEZE/PACKET_READY);
 * tavera/specs/amd64-logical-processors/amd64-logical-processors.md;
 * tavera/specs/amd64-logical-processors/debugging-2026-11-18.md
 * (H2 broadcast hang; H3 sequential DoneCount wait hang; H4 fire-and-forget).
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

typedef struct _KI_GENERIC_IPI_STATE
{
    PKIPI_BROADCAST_WORKER Worker;
    ULONG_PTR Argument;
    volatile LONG DoneCount;
} KI_GENERIC_IPI_STATE;

static KI_GENERIC_IPI_STATE KiGenericIpiState;

/* Per-CPU packet request; avoid IpiFrozen (freeze state machine). */
static volatile LONG KiIpiPacketRequest[MAXIMUM_PROCESSORS];

/*
 * H6g: restore DoneCount barrier under stock setup -mini (not CmdLine probe).
 * TB shootdown stays on H5 DPC path (KeFlushEntireTb). Sources:
 * debugging-2026-11-18.md H4–H6g; output/boot-m32-20261119T044000Z-h5-dpctb.log.
 */
static const BOOLEAN KiIpiGenericCallNoWait = FALSE;

/* FUNCTIONS *****************************************************************/

static
VOID
KiIpiHandlePacketReady(VOID)
{
    PKIPI_BROADCAST_WORKER Worker;
    ULONG_PTR Argument;

    Worker = KiGenericIpiState.Worker;
    Argument = KiGenericIpiState.Argument;
    ASSERT(Worker != NULL);
    Worker(Argument);
    InterlockedIncrement((PLONG)&KiGenericIpiState.DoneCount);
}

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    if (IpiRequest == IPI_APC)
    {
        HalSendSoftwareInterrupt(TargetSet, APC_LEVEL);
    }
    else if (IpiRequest == IPI_DPC)
    {
        HalSendSoftwareInterrupt(TargetSet, DISPATCH_LEVEL);
    }
    else if (IpiRequest == IPI_FREEZE)
    {
        HalSendNMI(TargetSet);
    }
    else
    {
        ASSERT(FALSE);
    }
}

BOOLEAN
NTAPI
KiIpiServiceRoutine(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB Prcb;
    ULONG Number;

    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);

    Prcb = KeGetCurrentPrcb();
    Number = Prcb->Number;

    if (InterlockedBitTestAndReset((PLONG)&Prcb->IpiFrozen, IPI_APC))
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }

    if (InterlockedBitTestAndReset((PLONG)&Prcb->IpiFrozen, IPI_DPC))
    {
        Prcb->DpcInterruptRequested = TRUE;
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }

    if ((Number < MAXIMUM_PROCESSORS) &&
        (InterlockedExchange((PLONG)&KiIpiPacketRequest[Number], 0) != 0))
    {
        KiIpiHandlePacketReady();
    }

    return TRUE;
}

static
VOID
KiIpiWaitOneTarget(
    _In_ ULONG Index,
    _In_ LONG DoneBefore)
{
    ULONG Stall = 0;
    KAFFINITY One = AFFINITY_MASK(Index);

    while (ReadNoFence((PLONG)&KiGenericIpiState.DoneCount) <= DoneBefore)
    {
        YieldProcessor();
        KeMemoryBarrier();
        Stall++;
        if ((Stall & 0xFFFF) == 0)
        {
            InterlockedCompareExchange((PLONG)&KiIpiPacketRequest[Index], 1, 0);
            KeMemoryBarrier();
            HalRequestIpi(One);
        }
        if ((Stall & 0x3FFFFF) == 0x400000)
        {
            DPRINT1("KeIpiGenericCall: stall target=%lu Done=%ld want>%ld Cpu=%lu\n",
                    Index,
                    ReadNoFence((PLONG)&KiGenericIpiState.DoneCount),
                    DoneBefore,
                    (ULONG)KeGetCurrentProcessorNumber());
        }
    }
}

ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql;
    KAFFINITY Affinity;
    KAFFINITY Remaining;
    ULONG Index;
    PKPRCB Prcb;
    LONG DoneBefore;

    ASSERT(KeGetCurrentIrql() < IPI_LEVEL);

    OldIrql = KeRaiseIrqlToSynchLevel();

    Prcb = KeGetCurrentPrcb();
    Affinity = KeActiveProcessors & ~Prcb->SetMember;

    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

    KiGenericIpiState.Worker = Function;
    KiGenericIpiState.Argument = Argument;
    KiGenericIpiState.DoneCount = 0;
    KeMemoryBarrier();

    /*
     * Arm every target, then either wait one-by-one (barrier) or fire-and-
     * forget (H4). Single-bit HalRequestIpi avoids AllExcludingSelf broadcast.
     * Sources: hal/halx86/apic/apicsmp.c; debugging-2026-11-18.md H2–H4;
     * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keipigenericcall
     */
    Remaining = Affinity;
    while (BitScanForwardAffinity(&Index, Remaining))
    {
        Remaining &= ~AFFINITY_MASK(Index);
        ASSERT(Index < MAXIMUM_PROCESSORS);
        InterlockedExchange((PLONG)&KiIpiPacketRequest[Index], 1);
    }

    KeMemoryBarrier();

    Remaining = Affinity;
    while (BitScanForwardAffinity(&Index, Remaining))
    {
        Remaining &= ~AFFINITY_MASK(Index);
        if (KiIpiGenericCallNoWait)
        {
            HalRequestIpi(AFFINITY_MASK(Index));
        }
        else
        {
            DoneBefore = ReadNoFence((PLONG)&KiGenericIpiState.DoneCount);
            HalRequestIpi(AFFINITY_MASK(Index));
            KiIpiWaitOneTarget(Index, DoneBefore);
        }
    }

    Status = Function(Argument);

    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);

    return Status;
}
