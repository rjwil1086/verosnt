/*
 * PROJECT:     Tavera OS Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        hal/halx86/smp/amd64/spinup.c
 * PURPOSE:     AMD64 Application Processor (AP) spinup setup
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Justin Miller <justin.miller@reactos.org>
 *              Ryan Wilson <ryan@taveraos.com>
 *
 * Sources: in-tree hal/halx86/smp/i386/spinup.c; hal/halx86/generic/halinit.c (ONECPU);
 * sdk/include/xdk/amd64/mm.h (PXE_BASE, page shifts); tavera/specs/amd64-logical-processors/
 * amd64-logical-processors.md
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern BOOLEAN HalpOnlyBootProcessor;

extern PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
extern PVOID HalpLowStub;

extern CHAR HalpAPEntry16[];
extern CHAR HalpAPEntryData[];
extern CHAR HalpAPEntry32[];
extern CHAR HalpAPEntry64[];
extern CHAR HalpAPEntry16End[];
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

ULONG HalpStartedProcessorCount = 1;

#ifndef Add2Ptr
#define Add2Ptr(P,I) ((PVOID)((PUCHAR)(P) + (I)))
#endif

#include <pshpack1.h>
typedef struct _AP_ENTRY_DATA
{
    ULONG Jump64Offset;
    ULONG Jump64Segment;
    ULONG PageTableRoot;
    ULONG Reserved0;
    PVOID ProcessorState;
    PVOID LoaderBlock;
    PVOID StubKernelVa;
    USHORT TempGdtrPad;
    USHORT TempGdtrLimit;
    ULONG TempGdtrBase;
    UCHAR TempGdt[32];
} AP_ENTRY_DATA, *PAP_ENTRY_DATA;
#include <poppack.h>

/* FUNCTIONS *****************************************************************/

static
ULONG
HalpSetupTemporaryMappings(VOID)
{
    PMMPTE TempPml4 = Add2Ptr(HalpLowStub, PAGE_SIZE);
    PMMPTE TempPdpt = Add2Ptr(HalpLowStub, 2 * PAGE_SIZE);
    PMMPTE TempPd = Add2Ptr(HalpLowStub, 3 * PAGE_SIZE);
    PMMPTE TempPt = Add2Ptr(HalpLowStub, 4 * PAGE_SIZE);
    PHYSICAL_ADDRESS PhysPml4, PhysPdpt, PhysPd, PhysPt, PhysPage;
    ULONG64 LowStubPhys;
    ULONG Pxi, Ppi, Pdi, Pti;
    ULONG i;

    ASSERT(HALP_LOW_STUB_SIZE_IN_PAGES >= 5);

    /* Keep BSP kernel mappings; patch only the low identity path */
    RtlCopyMemory(TempPml4, (PVOID)PXE_BASE, PAGE_SIZE);
    RtlZeroMemory(TempPdpt, PAGE_SIZE);
    RtlZeroMemory(TempPd, PAGE_SIZE);
    RtlZeroMemory(TempPt, PAGE_SIZE);

    PhysPml4 = MmGetPhysicalAddress(TempPml4);
    PhysPdpt = MmGetPhysicalAddress(TempPdpt);
    PhysPd = MmGetPhysicalAddress(TempPd);
    PhysPt = MmGetPhysicalAddress(TempPt);

    ASSERT(PhysPml4.HighPart == 0);
    ASSERT(PhysPdpt.HighPart == 0);
    ASSERT(PhysPd.HighPart == 0);
    ASSERT(PhysPt.HighPart == 0);

    LowStubPhys = HalpLowStubPhysicalAddress.QuadPart;
    Pxi = (ULONG)((LowStubPhys >> PXI_SHIFT) & PXI_MASK);
    Ppi = (ULONG)((LowStubPhys >> PPI_SHIFT) & PPI_MASK);
    Pdi = (ULONG)((LowStubPhys >> PDI_SHIFT) & PDI_MASK_AMD64);
    Pti = (ULONG)((LowStubPhys >> PTI_SHIFT) & PTI_MASK_AMD64);

    ASSERT(Pti + HALP_LOW_STUB_SIZE_IN_PAGES <= PTE_PER_PAGE);

    TempPml4[Pxi].u.Flush.Valid = 1;
    TempPml4[Pxi].u.Flush.Write = 1;
    TempPml4[Pxi].u.Flush.PageFrameNumber = PhysPdpt.QuadPart >> PAGE_SHIFT;

    TempPdpt[Ppi].u.Flush.Valid = 1;
    TempPdpt[Ppi].u.Flush.Write = 1;
    TempPdpt[Ppi].u.Flush.PageFrameNumber = PhysPd.QuadPart >> PAGE_SHIFT;

    TempPd[Pdi].u.Flush.Valid = 1;
    TempPd[Pdi].u.Flush.Write = 1;
    TempPd[Pdi].u.Flush.PageFrameNumber = PhysPt.QuadPart >> PAGE_SHIFT;

    for (i = 0; i < HALP_LOW_STUB_SIZE_IN_PAGES; i++)
    {
        PhysPage.QuadPart = LowStubPhys + ((ULONG64)i << PAGE_SHIFT);
        TempPt[Pti + i].u.Long = 0;
        TempPt[Pti + i].u.Flush.Valid = 1;
        TempPt[Pti + i].u.Flush.Write = 1;
        TempPt[Pti + i].u.Flush.PageFrameNumber = PhysPage.QuadPart >> PAGE_SHIFT;
    }

    return PhysPml4.LowPart;
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    PAP_ENTRY_DATA ApEntryData;
    ULONG InitialCr3;
    ULONG StubSize;
    ULONG Entry64Offset;

    if (HalpOnlyBootProcessor)
    {
        DPRINT1("HalStartNextProcessor: ONECPU\n");
        return FALSE;
    }

    if (HalpStartedProcessorCount == HalpApicInfoTable.ProcessorCount)
    {
        DPRINT1("HalStartNextProcessor: all %lu processors started\n",
                HalpStartedProcessorCount);
        return FALSE;
    }

    if (!HalpLowStub || !HalpLowStubPhysicalAddress.QuadPart)
    {
        DPRINT1("HalStartNextProcessor: low stub missing\n");
        return FALSE;
    }

    InitialCr3 = HalpSetupTemporaryMappings();
    if (!InitialCr3)
    {
        DPRINT1("HalStartNextProcessor: temporary mappings failed\n");
        return FALSE;
    }

    StubSize = (ULONG)((ULONG_PTR)HalpAPEntry16End - (ULONG_PTR)HalpAPEntry16);
    ASSERT(StubSize <= PAGE_SIZE);
    RtlCopyMemory(HalpLowStub, HalpAPEntry16, StubSize);

    ApEntryData = (PAP_ENTRY_DATA)((PUCHAR)HalpLowStub +
                                   ((ULONG_PTR)HalpAPEntryData - (ULONG_PTR)HalpAPEntry16));

    Entry64Offset = HalpLowStubPhysicalAddress.LowPart +
                    (ULONG)((ULONG_PTR)HalpAPEntry64 - (ULONG_PTR)HalpAPEntry16);

    ApEntryData->Jump64Offset = Entry64Offset;
    ApEntryData->Jump64Segment = 0x10;
    ApEntryData->PageTableRoot = InitialCr3;
    ApEntryData->Reserved0 = 0;
    ApEntryData->ProcessorState = ProcessorState;
    ApEntryData->LoaderBlock = LoaderBlock;
    ApEntryData->StubKernelVa = HalpLowStub;

    /*
     * TempGdt bytes came from the image copy. Only the GDTR base must be the
     * identity/physical address of that TempGdt in the low stub.
     */
    ApEntryData->TempGdtrLimit = (USHORT)(sizeof(ApEntryData->TempGdt) - 1);
    ApEntryData->TempGdtrBase =
        HalpLowStubPhysicalAddress.LowPart +
        (ULONG)((ULONG_PTR)&ApEntryData->TempGdt - (ULONG_PTR)HalpLowStub);

    DPRINT1("HalStartNextProcessor: SIPI CPU %lu stubPA=%lx entry64=%lx cr3=%lx\n",
            HalpStartedProcessorCount,
            HalpLowStubPhysicalAddress.LowPart,
            Entry64Offset,
            InitialCr3);

    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;

    return TRUE;
}
