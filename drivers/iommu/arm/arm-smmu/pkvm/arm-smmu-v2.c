// SPDX-License-Identifier: GPL-2.0-only
/*
 * pKVM IOMMU driver for ARM SMMUv2 (Tegra234)
 *
 * Copyright (C) 2025 Hannu Lyytinen <hannu.lyytinen@unikie.com>
 *
 * This driver runs at EL2 and provides IOMMU virtualization for protected
 * VMs on NVIDIA Tegra234 (Jetson AGX Orin). It manages SMMUv2 hardware,
 * enforces Stream ID assignments, and provides DMA isolation.
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/io-pgtable.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <nvhe/iommu.h>
#include <nvhe/memory.h>
#include <nvhe/mm.h>

#include "arm-smmu-v2.h"
#include "../../../io-pgtable-arm.h"

/*
 * Global State
 */
struct hyp_arm_smmu_v2_device *kvm_hyp_arm_smmu_v2_smmus[ARM_SMMU_MAX_INSTANCES];
struct sid_assignment sid_map[ARM_SMMU_MAX_SIDS];

/*
 * Global identity-mapped page table (protected by host_mmu.lock from core code)
 * All protected domains share this single Stage-2 page table that mirrors
 * the host's CPU stage-2 mappings for DMA isolation.
 */
static struct io_pgtable *idmap_pgtable;

/*
 * ARM SMMUv2 Register Definitions
 * Complete set imported from arm-smmu.h for EL2 implementation
 */

/* GR0 Configuration Registers */
#define ARM_SMMU_GR0_sCR0		0x0
#define ARM_SMMU_sCR0_VMID16EN		BIT(31)
#define ARM_SMMU_sCR0_BSU		GENMASK(15, 14)
#define ARM_SMMU_sCR0_FB		BIT(13)
#define ARM_SMMU_sCR0_PTM		BIT(12)
#define ARM_SMMU_sCR0_VMIDPNE		BIT(11)
#define ARM_SMMU_sCR0_USFCFG		BIT(10)
#define ARM_SMMU_sCR0_GCFGFIE		BIT(5)
#define ARM_SMMU_sCR0_GCFGFRE		BIT(4)
#define ARM_SMMU_sCR0_EXIDENABLE	BIT(3)
#define ARM_SMMU_sCR0_GFIE		BIT(2)
#define ARM_SMMU_sCR0_GFRE		BIT(1)
#define ARM_SMMU_sCR0_CLIENTPD		BIT(0)

/* GR0 Identification Registers */
#define ARM_SMMU_GR0_ID0		0x20
#define ARM_SMMU_ID0_S1TS		BIT(30)
#define ARM_SMMU_ID0_S2TS		BIT(29)
#define ARM_SMMU_ID0_NTS		BIT(28)
#define ARM_SMMU_ID0_SMS		BIT(27)
#define ARM_SMMU_ID0_ATOSNS		BIT(26)
#define ARM_SMMU_ID0_PTFS_NO_AARCH32	BIT(25)
#define ARM_SMMU_ID0_PTFS_NO_AARCH32S	BIT(24)
#define ARM_SMMU_ID0_NUMIRPT		GENMASK(23, 16)
#define ARM_SMMU_ID0_CTTW		BIT(14)
#define ARM_SMMU_ID0_NUMSIDB		GENMASK(12, 9)
#define ARM_SMMU_ID0_EXIDS		BIT(8)
#define ARM_SMMU_ID0_NUMSMRG		GENMASK(7, 0)

#define ARM_SMMU_GR0_ID1		0x24
#define ARM_SMMU_ID1_PAGESIZE		BIT(31)
#define ARM_SMMU_ID1_NUMPAGENDXB	GENMASK(30, 28)
#define ARM_SMMU_ID1_NUMS2CB		GENMASK(23, 16)
#define ARM_SMMU_ID1_NUMCB		GENMASK(7, 0)

#define ARM_SMMU_GR0_ID2		0x28
#define ARM_SMMU_ID2_VMID16		BIT(15)
#define ARM_SMMU_ID2_PTFS_64K		BIT(14)
#define ARM_SMMU_ID2_PTFS_16K		BIT(13)
#define ARM_SMMU_ID2_PTFS_4K		BIT(12)
#define ARM_SMMU_ID2_UBS		GENMASK(11, 8)
#define ARM_SMMU_ID2_OAS		GENMASK(7, 4)
#define ARM_SMMU_ID2_IAS		GENMASK(3, 0)

#define ARM_SMMU_GR0_ID7		0x3c
#define ARM_SMMU_ID7_MAJOR		GENMASK(7, 4)
#define ARM_SMMU_ID7_MINOR		GENMASK(3, 0)

/* GR0 Fault Registers */
#define ARM_SMMU_GR0_sGFSR		0x48
#define ARM_SMMU_sGFSR_USF		BIT(1)

#define ARM_SMMU_GR0_sGFSYNR0		0x50
#define ARM_SMMU_GR0_sGFSYNR1		0x54
#define ARM_SMMU_GR0_sGFSYNR2		0x58

/* GR0 Global TLB Invalidation */
#define ARM_SMMU_GR0_TLBIVMID		0x64
#define ARM_SMMU_GR0_TLBIALLNSNH	0x68
#define ARM_SMMU_GR0_TLBIALLH		0x6c
#define ARM_SMMU_GR0_sTLBGSYNC		0x70

#define ARM_SMMU_GR0_sTLBGSTATUS	0x74
#define ARM_SMMU_sTLBGSTATUS_GSACTIVE	BIT(0)

/* GR0 Stream Mapping Registers */
#define ARM_SMMU_GR0_SMR(n)		(0x800 + ((n) << 2))
#define ARM_SMMU_SMR_VALID		BIT(31)
#define ARM_SMMU_SMR_MASK		GENMASK(31, 16)
#define ARM_SMMU_SMR_ID			GENMASK(15, 0)

#define ARM_SMMU_GR0_S2CR(n)		(0xc00 + ((n) << 2))
#define ARM_SMMU_S2CR_PRIVCFG		GENMASK(25, 24)
#define ARM_SMMU_S2CR_TYPE		GENMASK(17, 16)
#define ARM_SMMU_S2CR_EXIDVALID		BIT(10)
#define ARM_SMMU_S2CR_CBNDX		GENMASK(7, 0)

/* S2CR Type values */
#define S2CR_TYPE_TRANS			0
#define S2CR_TYPE_BYPASS		1
#define S2CR_TYPE_FAULT			2

/* GR1 Context Bank Attribute Registers */
#define ARM_SMMU_GR1_CBAR(n)		(0x0 + ((n) << 2))
#define ARM_SMMU_CBAR_IRPTNDX		GENMASK(31, 24)
#define ARM_SMMU_CBAR_TYPE		GENMASK(17, 16)
#define ARM_SMMU_CBAR_S1_MEMATTR	GENMASK(15, 12)
#define ARM_SMMU_CBAR_S1_MEMATTR_WB	0xf
#define ARM_SMMU_CBAR_S1_BPSHCFG	GENMASK(9, 8)
#define ARM_SMMU_CBAR_S1_BPSHCFG_NSH	3
#define ARM_SMMU_CBAR_VMID		GENMASK(7, 0)

/* CBAR Type values */
#define CBAR_TYPE_S2_TRANS		0
#define CBAR_TYPE_S1_TRANS_S2_BYPASS	1
#define CBAR_TYPE_S1_TRANS_S2_FAULT	2
#define CBAR_TYPE_S1_TRANS_S2_TRANS	3

#define ARM_SMMU_GR1_CBFRSYNRA(n)	(0x400 + ((n) << 2))
#define ARM_SMMU_CBFRSYNRA_SID		GENMASK(15, 0)

#define ARM_SMMU_GR1_CBA2R(n)		(0x800 + ((n) << 2))
#define ARM_SMMU_CBA2R_VMID16		GENMASK(31, 16)
#define ARM_SMMU_CBA2R_VA64		BIT(0)

/* Context Bank Registers (relative to CB base) */
#define ARM_SMMU_CB_SCTLR		0x0
#define ARM_SMMU_SCTLR_S1_ASIDPNE	BIT(12)
#define ARM_SMMU_SCTLR_CFCFG		BIT(7)
#define ARM_SMMU_SCTLR_HUPCF		BIT(8)
#define ARM_SMMU_SCTLR_CFIE		BIT(6)
#define ARM_SMMU_SCTLR_CFRE		BIT(5)
#define ARM_SMMU_SCTLR_E		BIT(4)
#define ARM_SMMU_SCTLR_AFE		BIT(2)
#define ARM_SMMU_SCTLR_TRE		BIT(1)
#define ARM_SMMU_SCTLR_M		BIT(0)

#define ARM_SMMU_CB_ACTLR		0x4
#define ARM_SMMU_CB_RESUME		0x8
#define ARM_SMMU_RESUME_TERMINATE	BIT(0)

#define ARM_SMMU_CB_TCR2		0x10
#define ARM_SMMU_TCR2_SEP		GENMASK(17, 15)
#define ARM_SMMU_TCR2_SEP_UPSTREAM	0x7
#define ARM_SMMU_TCR2_AS		BIT(4)
#define ARM_SMMU_TCR2_PASIZE		GENMASK(3, 0)

#define ARM_SMMU_CB_TTBR0		0x20
#define ARM_SMMU_CB_TTBR1		0x28
#define ARM_SMMU_TTBRn_ASID		GENMASK_ULL(63, 48)

#define ARM_SMMU_CB_TCR			0x30
#define ARM_SMMU_TCR_EAE		BIT(31)
#define ARM_SMMU_TCR_EPD1		BIT(23)
#define ARM_SMMU_TCR_A1			BIT(22)
#define ARM_SMMU_TCR_TG0		GENMASK(15, 14)
#define ARM_SMMU_TCR_SH0		GENMASK(13, 12)
#define ARM_SMMU_TCR_ORGN0		GENMASK(11, 10)
#define ARM_SMMU_TCR_IRGN0		GENMASK(9, 8)
#define ARM_SMMU_TCR_EPD0		BIT(7)
#define ARM_SMMU_TCR_T0SZ		GENMASK(5, 0)

/* VTCR fields (Stage-2 translation control via TCR2 in Stage-2-only mode) */
#define ARM_SMMU_VTCR_RES1		BIT(31)
#define ARM_SMMU_VTCR_PS		GENMASK(18, 16)
#define ARM_SMMU_VTCR_TG0		ARM_SMMU_TCR_TG0
#define ARM_SMMU_VTCR_SH0		ARM_SMMU_TCR_SH0
#define ARM_SMMU_VTCR_ORGN0		ARM_SMMU_TCR_ORGN0
#define ARM_SMMU_VTCR_IRGN0		ARM_SMMU_TCR_IRGN0
#define ARM_SMMU_VTCR_SL0		GENMASK(7, 6)
#define ARM_SMMU_VTCR_T0SZ		ARM_SMMU_TCR_T0SZ

#define ARM_SMMU_CB_CONTEXTIDR		0x34
#define ARM_SMMU_CB_S1_MAIR0		0x38
#define ARM_SMMU_CB_S1_MAIR1		0x3c

#define ARM_SMMU_CB_FSR			0x58
#define ARM_SMMU_CB_FSR_MULTI		BIT(31)
#define ARM_SMMU_CB_FSR_SS		BIT(30)
#define ARM_SMMU_CB_FSR_FORMAT		GENMASK(10, 9)
#define ARM_SMMU_CB_FSR_UUT		BIT(8)
#define ARM_SMMU_CB_FSR_ASF		BIT(7)
#define ARM_SMMU_CB_FSR_TLBLKF		BIT(6)
#define ARM_SMMU_CB_FSR_TLBMCF		BIT(5)
#define ARM_SMMU_CB_FSR_EF		BIT(4)
#define ARM_SMMU_CB_FSR_PF		BIT(3)
#define ARM_SMMU_CB_FSR_AFF		BIT(2)
#define ARM_SMMU_CB_FSR_TF		BIT(1)

#define ARM_SMMU_CB_FSR_IGN		(ARM_SMMU_CB_FSR_AFF |		\
					 ARM_SMMU_CB_FSR_ASF |		\
					 ARM_SMMU_CB_FSR_TLBMCF |	\
					 ARM_SMMU_CB_FSR_TLBLKF)

#define ARM_SMMU_CB_FSR_FAULT		(ARM_SMMU_CB_FSR_MULTI |	\
					 ARM_SMMU_CB_FSR_SS |		\
					 ARM_SMMU_CB_FSR_UUT |		\
					 ARM_SMMU_CB_FSR_EF |		\
					 ARM_SMMU_CB_FSR_PF |		\
					 ARM_SMMU_CB_FSR_TF |		\
					 ARM_SMMU_CB_FSR_IGN)

#define ARM_SMMU_CB_FAR			0x60

#define ARM_SMMU_CB_FSYNR0		0x68
#define ARM_SMMU_CB_FSYNR0_PLVL		GENMASK(1, 0)
#define ARM_SMMU_CB_FSYNR0_WNR		BIT(4)
#define ARM_SMMU_CB_FSYNR0_PNU		BIT(5)
#define ARM_SMMU_CB_FSYNR0_IND		BIT(6)
#define ARM_SMMU_CB_FSYNR0_NSATTR	BIT(8)
#define ARM_SMMU_CB_FSYNR0_PTWF		BIT(10)
#define ARM_SMMU_CB_FSYNR0_AFR		BIT(11)
#define ARM_SMMU_CB_FSYNR0_S1CBNDX	GENMASK(23, 16)

/* Context Bank TLB Invalidation */
#define ARM_SMMU_CB_S1_TLBIVA		0x600
#define ARM_SMMU_CB_S1_TLBIASID		0x610
#define ARM_SMMU_CB_S1_TLBIVAL		0x620
#define ARM_SMMU_CB_S2_TLBIIPAS2	0x630
#define ARM_SMMU_CB_S2_TLBIIPAS2L	0x638
#define ARM_SMMU_CB_TLBSYNC		0x7f0
#define ARM_SMMU_CB_TLBSTATUS		0x7f4

/* Timeouts */
#define TLB_LOOP_TIMEOUT		1000000	/* 1s */

/*
 * Helper Functions
 */

/**
 * arm_smmu_id_size_to_bits - Convert ID register size field to actual bits
 * @size: Size field from ID register (0-7)
 *
 * Returns: Actual address size in bits
 */
static int arm_smmu_id_size_to_bits(int size)
{
	switch (size) {
	case 0:
		return 32;
	case 1:
		return 36;
	case 2:
		return 40;
	case 3:
		return 42;
	case 4:
		return 44;
	case 5:
		return 48;
	default:
		return 48;  /* Maximum supported */
	}
}

/*
 * Device Initialization
 */

/**
 * smmu_v2_probe_device - Read SMMU capabilities from hardware
 * @smmu: SMMU device to probe
 *
 * Reads IDR registers to determine capabilities (number of context banks,
 * stream mapping groups, page sizes, coherent walk support, etc.)
 *
 * Returns: 0 on success, negative error code on failure
 */
int smmu_v2_probe_device(struct hyp_arm_smmu_v2_device *smmu)
{
	u32 id0, id1, id2, id7;
	u32 size;

	/* Read capability registers */
	id0 = smmu_readl(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_ID0);
	id1 = smmu_readl(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_ID1);
	id2 = smmu_readl(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_ID2);
	id7 = smmu_readl(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_ID7);

	/* ID0: Stream mapping and translation support */
	smmu->features = 0;

	/* Check Stage-1 translation support */
	if (id0 & ARM_SMMU_ID0_S1TS)
		smmu->features |= ARM_SMMU_FEAT_TRANS_S1;

	/* Check Stage-2 translation support (required for pKVM) */
	if (id0 & ARM_SMMU_ID0_S2TS)
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;

	/* Check nested translation support */
	if (id0 & ARM_SMMU_ID0_NTS)
		smmu->features |= ARM_SMMU_FEAT_TRANS_NESTED;

	/* Check coherent table walk support */
	if (id0 & ARM_SMMU_ID0_CTTW)
		smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;

	/* Get number of stream mapping register groups */
	if (id0 & ARM_SMMU_ID0_SMS) {
		smmu->features |= ARM_SMMU_FEAT_STREAM_MATCH;
		size = FIELD_GET(ARM_SMMU_ID0_NUMSMRG, id0);
		if (size == 0) {
			/* Stream matching supported but no SMRs? Invalid! */
			return -ENODEV;
		}
		smmu->num_mapping_groups = size;
	} else {
		/* No stream matching, use direct Stream ID indexing */
		smmu->num_mapping_groups = 128;  /* Tegra234 has 128 */
	}

	/* ID1: Context banks and page size */
	smmu->pgshift = (id1 & ARM_SMMU_ID1_PAGESIZE) ? 16 : 12;  /* 64KB or 4KB */

	/* Tegra234 erratum: force 4KB pages due to walk cache bug */
	smmu->pgshift = 12;
	smmu->pgsize_bitmap = SZ_4K;

	/* Get number of context banks */
	smmu->num_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMCB, id1);
	smmu->num_s2_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMS2CB, id1);

	if (smmu->num_s2_context_banks > smmu->num_context_banks) {
		/* Impossible configuration */
		return -ENODEV;
	}

	/* ID2: Address sizes and VMID support */
	size = arm_smmu_id_size_to_bits(FIELD_GET(ARM_SMMU_ID2_IAS, id2));
	smmu->ias = size;  /* Input Address Size (IPA) */

	size = arm_smmu_id_size_to_bits(FIELD_GET(ARM_SMMU_ID2_OAS, id2));
	smmu->oas = size;  /* Output Address Size (PA) */

	/* Check for 16-bit VMID support */
	if (id2 & ARM_SMMU_ID2_VMID16) {
		smmu->features |= ARM_SMMU_FEAT_VMID16;
		smmu->vmid_bits = 16;
	} else {
		smmu->vmid_bits = 8;
	}

	/* Check page size support (used for Tegra234 quirk above) */
	if (id2 & ARM_SMMU_ID2_PTFS_4K)
		smmu->pgsize_bitmap |= SZ_4K;
	if (id2 & ARM_SMMU_ID2_PTFS_16K)
		smmu->pgsize_bitmap |= SZ_16K;
	if (id2 & ARM_SMMU_ID2_PTFS_64K)
		smmu->pgsize_bitmap |= SZ_64K;

	/* Force 4K only due to Tegra234 erratum */
	smmu->pgsize_bitmap = SZ_4K;

	/* ID7: SMMU architecture version */
	/* For Tegra234, this is SMMUv2 (ARM MMU-500) */
	/* Version info is informational only */

	return 0;
}

/**
 * smmu_v2_reset - Reset and initialize SMMU hardware
 * @smmu: SMMU device to reset
 *
 * Configures global registers and prepares hardware for operation.
 * This follows the standard ARM SMMUv2 reset sequence.
 */
int smmu_v2_reset(struct hyp_arm_smmu_v2_device *smmu)
{
	u32 scr0, reg;
	int i, ret;

	/* 1. Clear global fault status register */
	reg = smmu_readl(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sGFSR);
	smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sGFSR, reg);

	/*
	 * 2. Reset stream mapping groups: Initial values mark all SMRn as
	 * invalid and all S2CRn as fault unless overridden.
	 */
	for (i = 0; i < smmu->num_mapping_groups; i++) {
		/* Clear SMR (mark as invalid) */
		smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_SMR(i), 0);

		/* Set S2CR to FAULT type (deny unmapped streams) */
		smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_S2CR(i),
			    FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_FAULT));
	}

	/* 3. Make sure all context banks are disabled and clear CB_FSR */
	for (i = 0; i < smmu->num_context_banks; i++) {
		/* Disable context bank (clear SCTLR.M) */
		smmu_writel(smmu, (i + 2) << smmu->pgshift,
			    ARM_SMMU_CB_SCTLR, 0);

		/* Clear context bank fault status */
		smmu_writel(smmu, (i + 2) << smmu->pgshift,
			    ARM_SMMU_CB_FSR, ARM_SMMU_CB_FSR_FAULT);
	}

	/* 4. Invalidate the TLB, just in case */
	smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_TLBIALLH, 0);
	smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_TLBIALLNSNH, 0);

	/* 5. Configure sCR0 with appropriate settings */
	scr0 = 0;

	/* Enable fault reporting */
	scr0 |= ARM_SMMU_sCR0_GFRE | ARM_SMMU_sCR0_GFIE |
		ARM_SMMU_sCR0_GCFGFRE | ARM_SMMU_sCR0_GCFGFIE;

	/* Disable TLB broadcasting, enable VMID partitioning */
	scr0 |= ARM_SMMU_sCR0_VMIDPNE | ARM_SMMU_sCR0_PTM;

	/* Handle unmatched streams (deny by default for security) */
	scr0 |= ARM_SMMU_sCR0_USFCFG;

	/* Disable forced broadcasting */
	/* (FB bit is implicitly 0, no need to clear) */

	/* Don't upgrade barriers */
	/* (BSU bits are implicitly 0) */

	/* Enable 16-bit VMIDs if supported */
	if (smmu->features & ARM_SMMU_FEAT_VMID16)
		scr0 |= ARM_SMMU_sCR0_VMID16EN;

	/* Enable client access (clear CLIENTPD) */
	/* (CLIENTPD is bit 0, already 0 in our scr0) */

	/* 6. Perform global TLB sync before enabling */
	ret = smmu_v2_tlb_sync_global(smmu);
	if (ret)
		return ret;

	/* 7. Write final sCR0 value to enable SMMU */
	smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sCR0, scr0);

	return 0;
}

/*
 * TLB Operations for io-pgtable Integration
 */

/**
 * smmu_v2_tlb_flush_walk - Flush TLB after unmapping non-leaf PTEs
 * @iova: I/O virtual address
 * @size: Size of the range to invalidate
 * @granule: Page granule size
 * @cookie: SMMU device (unused - we invalidate all SMMUs)
 *
 * Called by io-pgtable when unmapping intermediate page table entries.
 */
static void smmu_v2_tlb_flush_walk(unsigned long iova, size_t size,
				   size_t granule, void *cookie)
{
	struct hyp_arm_smmu_v2_device *smmu;
	int i;

	/* Invalidate on ALL SMMU instances (global identity mapping) */
	for (i = 0; i < ARM_SMMU_MAX_INSTANCES; i++) {
		smmu = kvm_hyp_arm_smmu_v2_smmus[i];
		if (!smmu)
			continue;

		/* Global TLB invalidation (all VMIDs) */
		smmu_v2_tlb_inv_context(smmu, 0);  /* CB 0 - could be any CB */
	}
}

/**
 * smmu_v2_tlb_add_page - Add page to TLB invalidation gather
 * @gather: TLB gather structure (unused for SMMUv2)
 * @iova: I/O virtual address
 * @granule: Page granule size
 * @cookie: SMMU device (unused)
 *
 * Called by io-pgtable when unmapping leaf page table entries.
 * SMMUv2 doesn't support gather/batch TLB invalidation, so we invalidate immediately.
 */
static void smmu_v2_tlb_add_page(struct iommu_iotlb_gather *gather,
				 unsigned long iova, size_t granule, void *cookie)
{
	/* For now, just do a full context invalidation */
	/* TODO: Implement range-based invalidation for better performance */
	smmu_v2_tlb_flush_walk(iova, granule, granule, cookie);
}

static const struct iommu_flush_ops smmu_v2_tlb_ops = {
	.tlb_flush_walk	= smmu_v2_tlb_flush_walk,
	.tlb_add_page	= smmu_v2_tlb_add_page,
};

/**
 * smmu_v2_init_pgt - Initialize global identity-mapped page table
 *
 * Creates a single Stage-2 page table shared by all protected domains.
 * This table mirrors the host's CPU stage-2 mappings for DMA isolation.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int smmu_v2_init_pgt(void)
{
	struct io_pgtable_cfg cfg = {
		.tlb		= &smmu_v2_tlb_ops,
		.ias		= 48,	/* Input address size */
		.oas		= 48,	/* Output address size */
		.coherent_walk	= true,
		.pgsize_bitmap	= SZ_4K,  /* Tegra234: 4K only (walk cache erratum) */
	};
	struct hyp_arm_smmu_v2_device *smmu;
	struct io_pgtable_ops *ops;
	int i;

	/* Determine common capabilities across all SMMU instances */
	for (i = 0; i < ARM_SMMU_MAX_INSTANCES; i++) {
		smmu = kvm_hyp_arm_smmu_v2_smmus[i];
		if (!smmu)
			continue;

		/* Use minimum IAS/OAS across all SMMUs */
		if (smmu->ias < cfg.ias)
			cfg.ias = smmu->ias;
		if (smmu->oas < cfg.oas)
			cfg.oas = smmu->oas;

		/* AND together page size support (most restrictive) */
		cfg.pgsize_bitmap &= smmu->pgsize_bitmap;

		/* Coherent walk only if all SMMUs support it */
		if (!(smmu->features & ARM_SMMU_FEAT_COHERENT_WALK))
			cfg.coherent_walk = false;
	}

	/* Allocate Stage-2 page table */
	ops = kvm_alloc_io_pgtable_ops(ARM_64_LPAE_S2, &cfg, NULL);
	if (!ops)
		return -ENOMEM;

	idmap_pgtable = io_pgtable_ops_to_pgtable(ops);
	if (!idmap_pgtable) {
		/* This shouldn't happen, but handle it anyway */
		return -ENOMEM;
	}

	return 0;
}

/**
 * smmu_v2_init - Initialize SMMU device at EL2
 * @smmu: SMMU device structure
 *
 * Assumes that shadow arrays have already been allocated and donated from EL1.
 * This function initializes the donated memory to a known state.
 *
 * Note: Tegra234 dual register base support (niso0/niso1 with secondary bases)
 * is handled automatically by smmu_writel/smmu_readl helper functions defined
 * in arm-smmu-v2.h. These check has_secondary_base and mirror writes to base_sec.
 */
int smmu_v2_init(struct hyp_arm_smmu_v2_device *smmu)
{
	int ret, i;

	/* Validate that shadow arrays have been set up */
	if (!smmu->smrs_shadow || !smmu->s2crs_shadow ||
	    !smmu->smrs_hw || !smmu->s2crs_hw) {
		return -EINVAL;  /* Shadow arrays not donated from EL1 */
	}

	hyp_spin_lock_init(&smmu->lock);

	/* Probe hardware capabilities */
	ret = smmu_v2_probe_device(smmu);
	if (ret)
		return ret;

	/* Initialize context bank bitmap (all free initially) */
	bitmap_zero(smmu->context_map, ARM_SMMU_MAX_CBS);

	/* Initialize context bank state (all inactive) */
	for (i = 0; i < ARM_SMMU_MAX_CBS; i++) {
		smmu->cb_state[i].active = false;
		smmu->cb_state[i].domain_id = 0;
	}

	/* Initialize shadow SMR arrays to invalid/fault state */
	for (i = 0; i < smmu->num_mapping_groups; i++) {
		/* Shadow state: what host thinks is programmed */
		smmu->smrs_shadow[i].valid = false;
		smmu->smrs_shadow[i].id = 0;
		smmu->smrs_shadow[i].mask = 0;

		/* S2CR shadow: fault mode by default */
		smmu->s2crs_shadow[i].type = S2CR_TYPE_FAULT;
		smmu->s2crs_shadow[i].cbndx = 0;
		smmu->s2crs_shadow[i].privcfg = 0;
		smmu->s2crs_shadow[i].bypass = false;

		/* Hardware state: initially same as shadow */
		smmu->smrs_hw[i] = smmu->smrs_shadow[i];
		smmu->s2crs_hw[i] = smmu->s2crs_shadow[i];
	}

	/* Reset and configure hardware */
	ret = smmu_v2_reset(smmu);
	if (ret)
		return ret;

	return 0;
}

/*
 * MMIO Emulation
 */

/**
 * smmu_v2_handle_gr0 - Handle GR0 register access
 * @smmu: SMMU device
 * @offset: Register offset within GR0 page
 * @is_write: true for write access, false for read
 * @val: Pointer to value (read or write)
 *
 * Emulates GR0 (Global Register page 0) accesses with shadow state management.
 * Key register types:
 * - ID registers (IDR0-IDR7): Read-only capability reporting
 * - sCR0: Global control (enable/disable, fault reporting)
 * - SMR: Stream match configuration (shadowed)
 * - S2CR: Stream-to-context mapping (shadowed, enforces Stage-2)
 * - TLB operations: Wire to existing TLB implementations
 */
int smmu_v2_handle_gr0(struct hyp_arm_smmu_v2_device *smmu, u32 offset,
		       bool is_write, u64 *val)
{
	u32 val32;
	int ret = 0;

	/* ID registers - read-only capability reporting */
	if (offset >= ARM_SMMU_GR0_ID0 && offset <= ARM_SMMU_GR0_ID7) {
		if (is_write)
			return -EINVAL;  /* Read-only */

		/* Pass through hardware capabilities */
		*val = smmu_readl(smmu, ARM_SMMU_GR0, offset);
		return 0;
	}

	/* sCR0 - Global control register */
	if (offset == ARM_SMMU_GR0_sCR0) {
		if (is_write) {
			val32 = (u32)*val;

			/* Allow most configuration but filter dangerous bits */
			/* Always keep fault reporting enabled */
			val32 |= (ARM_SMMU_sCR0_GFRE | ARM_SMMU_sCR0_GFIE |
				  ARM_SMMU_sCR0_GCFGFRE | ARM_SMMU_sCR0_GCFGFIE);

			/* Always keep VMID partitioning enabled for nesting */
			if (smmu->features & ARM_SMMU_FEAT_TRANS_NESTED)
				val32 |= ARM_SMMU_sCR0_VMIDPNE;

			smmu_writel(smmu, ARM_SMMU_GR0, offset, val32);
			return 0;
		} else {
			*val = smmu_readl(smmu, ARM_SMMU_GR0, offset);
			return 0;
		}
	}

	/* Global fault status/syndrome registers - read/clear */
	if (offset == ARM_SMMU_GR0_sGFSR) {
		if (is_write) {
			/* Write-1-to-clear */
			smmu_writel(smmu, ARM_SMMU_GR0, offset, (u32)*val);
			return 0;
		} else {
			*val = smmu_readl(smmu, ARM_SMMU_GR0, offset);
			return 0;
		}
	}

	if (offset == ARM_SMMU_GR0_sGFSYNR0 || offset == ARM_SMMU_GR0_sGFSYNR1 ||
	    offset == ARM_SMMU_GR0_sGFSYNR2) {
		if (is_write)
			return -EINVAL;  /* Read-only */
		*val = smmu_readl(smmu, ARM_SMMU_GR0, offset);
		return 0;
	}

	/* TLB invalidation registers - write-only trigger registers */
	if (offset == ARM_SMMU_GR0_TLBIVMID) {
		if (!is_write)
			return -EINVAL;  /* Write-only */

		/* TLBIVMID: Invalidate all TLB entries for this VMID */
		smmu_writel(smmu, ARM_SMMU_GR0, offset, (u32)*val);
		ret = smmu_v2_tlb_sync_global(smmu);
		return ret;
	}

	if (offset == ARM_SMMU_GR0_TLBIALLNSNH || offset == ARM_SMMU_GR0_TLBIALLH) {
		if (!is_write)
			return -EINVAL;  /* Write-only */

		/* Global TLB invalidation */
		smmu_writel(smmu, ARM_SMMU_GR0, offset, 0);
		ret = smmu_v2_tlb_sync_global(smmu);
		return ret;
	}

	if (offset == ARM_SMMU_GR0_sTLBGSYNC) {
		if (!is_write)
			return -EINVAL;  /* Write-only */

		/* Host requested TLB sync, execute it */
		ret = smmu_v2_tlb_sync_global(smmu);
		return ret;
	}

	if (offset == ARM_SMMU_GR0_sTLBGSTATUS) {
		if (is_write)
			return -EINVAL;  /* Read-only */
		*val = smmu_tlb_sync_status(smmu, ARM_SMMU_GR0, offset);
		return 0;
	}

	/* SMR registers - stream match configuration (shadow state) */
	if (offset >= ARM_SMMU_GR0_SMR(0) &&
	    offset < ARM_SMMU_GR0_SMR(0) + (smmu->num_mapping_groups * 4)) {
		u32 idx = (offset - ARM_SMMU_GR0_SMR(0)) >> 2;

		if (idx >= smmu->num_mapping_groups)
			return -EINVAL;

		if (is_write) {
			val32 = (u32)*val;

			/* Update shadow state (what host thinks it programmed) */
			smmu->smrs_shadow[idx].valid = !!(val32 & ARM_SMMU_SMR_VALID);
			smmu->smrs_shadow[idx].mask = FIELD_GET(ARM_SMMU_SMR_MASK, val32);
			smmu->smrs_shadow[idx].id = FIELD_GET(ARM_SMMU_SMR_ID, val32);

			/* Write through to hardware (no modification needed for SMR) */
			smmu_writel(smmu, ARM_SMMU_GR0, offset, val32);

			/* Update hardware state tracking */
			smmu->smrs_hw[idx] = smmu->smrs_shadow[idx];
			return 0;
		} else {
			/* Return shadow state (what host thinks hardware has) */
			val32 = 0;
			if (smmu->smrs_shadow[idx].valid)
				val32 |= ARM_SMMU_SMR_VALID;
			val32 |= FIELD_PREP(ARM_SMMU_SMR_MASK, smmu->smrs_shadow[idx].mask);
			val32 |= FIELD_PREP(ARM_SMMU_SMR_ID, smmu->smrs_shadow[idx].id);
			*val = val32;
			return 0;
		}
	}

	/* S2CR registers - stream-to-context mapping (shadow + enforce Stage-2) */
	if (offset >= ARM_SMMU_GR0_S2CR(0) &&
	    offset < ARM_SMMU_GR0_S2CR(0) + (smmu->num_mapping_groups * 4)) {
		u32 idx = (offset - ARM_SMMU_GR0_S2CR(0)) >> 2;
		u8 cb_idx;

		if (idx >= smmu->num_mapping_groups)
			return -EINVAL;

		if (is_write) {
			val32 = (u32)*val;

			/* Extract fields from host's write */
			smmu->s2crs_shadow[idx].type = FIELD_GET(ARM_SMMU_S2CR_TYPE, val32);
			smmu->s2crs_shadow[idx].cbndx = FIELD_GET(ARM_SMMU_S2CR_CBNDX, val32);
			smmu->s2crs_shadow[idx].privcfg = FIELD_GET(ARM_SMMU_S2CR_PRIVCFG, val32);

			cb_idx = smmu->s2crs_shadow[idx].cbndx;

			/*
			 * For protected domains (those with active CB state),
			 * the CB is already configured as Stage-2-only via CBAR.TYPE.
			 * No need to modify S2CR.TYPE here - just pass through.
			 *
			 * The CBAR configuration is the authority for translation mode,
			 * not S2CR.TYPE in Stage-2-only implementations.
			 */
			smmu_writel(smmu, ARM_SMMU_GR0, offset, val32);

			/* Update hardware state tracking */
			smmu->s2crs_hw[idx] = smmu->s2crs_shadow[idx];
			return 0;
		} else {
			/* Return shadow state */
			val32 = 0;
			val32 |= FIELD_PREP(ARM_SMMU_S2CR_TYPE, smmu->s2crs_shadow[idx].type);
			val32 |= FIELD_PREP(ARM_SMMU_S2CR_CBNDX, smmu->s2crs_shadow[idx].cbndx);
			val32 |= FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, smmu->s2crs_shadow[idx].privcfg);
			*val = val32;
			return 0;
		}
	}

	/* Unknown or unsupported register */
	return -EINVAL;
}

/**
 * smmu_v2_handle_gr1 - Handle GR1 register access
 * @smmu: SMMU device
 * @offset: Register offset within GR1 page
 * @is_write: true for write access, false for read
 * @val: Pointer to value (read or write)
 *
 * Emulates GR1 (Global Register page 1) accesses for context bank attributes.
 * Key register types:
 * - CBAR: Context Bank Attribute Register (translation mode, VMID)
 * - CBA2R: Extended attributes (VMID16, 64-bit VA)
 * - CBFRSYNRA: Fault syndrome auxiliary (read-only)
 *
 * Note: CBAR.TYPE is the authority for translation mode in Stage-2-only.
 * Protected domains always use CBAR_TYPE_S2_TRANS for Stage-2 translation.
 */
int smmu_v2_handle_gr1(struct hyp_arm_smmu_v2_device *smmu, u32 offset,
		       bool is_write, u64 *val)
{
	u32 val32;
	u8 cb_idx;

	/* CBAR registers - context bank attributes */
	if (offset >= ARM_SMMU_GR1_CBAR(0) &&
	    offset < ARM_SMMU_GR1_CBAR(0) + (smmu->num_context_banks * 4)) {
		cb_idx = (offset - ARM_SMMU_GR1_CBAR(0)) >> 2;

		if (cb_idx >= smmu->num_context_banks)
			return -EINVAL;

		if (is_write) {
			val32 = (u32)*val;

			/*
			 * For protected domains (active CBs with domain_id set),
			 * enforce Stage-2 translation mode via CBAR.TYPE.
			 *
			 * If this CB is active and belongs to a protected domain,
			 * override CBAR.TYPE to CBAR_TYPE_S2_TRANS.
			 */
			if (smmu->cb_state[cb_idx].active &&
			    smmu->cb_state[cb_idx].domain_id != 0) {
				u32 cbar_type = FIELD_GET(ARM_SMMU_CBAR_TYPE, val32);

				/*
				 * Force Stage-2-only translation for protected domains.
				 * This is the primary enforcement point for DMA isolation.
				 */
				val32 &= ~ARM_SMMU_CBAR_TYPE;
				val32 |= FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_TRANS);

				hyp_dbg("SMMU[%u]: CB%u forced Stage-2 (host requested type=%u)\n",
					smmu->id, cb_idx, cbar_type);
			}

			/* Store in CB state for tracking */
			smmu->cb_state[cb_idx].cbar = val32;

			/* Write to hardware */
			smmu_writel(smmu, ARM_SMMU_GR1, offset, val32);
			return 0;
		} else {
			/* Return current CB state */
			*val = smmu->cb_state[cb_idx].cbar;
			return 0;
		}
	}

	/* CBA2R registers - extended attributes */
	if (offset >= ARM_SMMU_GR1_CBA2R(0) &&
	    offset < ARM_SMMU_GR1_CBA2R(0) + (smmu->num_context_banks * 4)) {
		cb_idx = (offset - ARM_SMMU_GR1_CBA2R(0)) >> 2;

		if (cb_idx >= smmu->num_context_banks)
			return -EINVAL;

		if (is_write) {
			val32 = (u32)*val;

			/* Allow all CBA2R fields (VA64, VMID16) */
			smmu_writel(smmu, ARM_SMMU_GR1, offset, val32);

			/* Track VMID for this CB (needed for TLB ops) */
			if (smmu->features & ARM_SMMU_FEAT_VMID16) {
				u16 vmid = FIELD_GET(ARM_SMMU_CBA2R_VMID16, val32);
				smmu->cb_state[cb_idx].vmid = vmid;
			}
			return 0;
		} else {
			*val = smmu_readl(smmu, ARM_SMMU_GR1, offset);
			return 0;
		}
	}

	/* CBFRSYNRA registers - fault syndrome auxiliary (read-only) */
	if (offset >= ARM_SMMU_GR1_CBFRSYNRA(0) &&
	    offset < ARM_SMMU_GR1_CBFRSYNRA(0) + (smmu->num_context_banks * 4)) {
		cb_idx = (offset - ARM_SMMU_GR1_CBFRSYNRA(0)) >> 2;

		if (cb_idx >= smmu->num_context_banks)
			return -EINVAL;

		if (is_write)
			return -EINVAL;  /* Read-only */

		*val = smmu_readl(smmu, ARM_SMMU_GR1, offset);
		return 0;
	}

	/* Unknown or unsupported register */
	return -EINVAL;
}

/**
 * smmu_v2_handle_cb - Handle context bank register access
 * @smmu: SMMU device
 * @offset: Register offset (CB page + offset within CB)
 * @is_write: true for write access, false for read
 * @val: Pointer to value (read or write)
 *
 * Emulates context bank register accesses for translation control.
 * Key register types:
 * - SCTLR: System control (MMU enable, fault handling)
 * - TTBR0/TTBR1: Translation table base registers
 * - TCR/TCR2: Translation control registers
 * - MAIR: Memory attribute indirection registers
 * - FSR/FAR/FSYNR0: Fault status and address registers
 * - TLBSYNC/TLBSTATUS: Per-CB TLB synchronization
 *
 * Note: For Stage-2-only protected domains, TTBR0 points to EL2's Stage-2
 * page table. Host writes are shadowed but the actual hardware uses EL2's PT.
 */
int smmu_v2_handle_cb(struct hyp_arm_smmu_v2_device *smmu, u32 offset,
		      bool is_write, u64 *val)
{
	u32 page_offset, cb_offset;
	u8 cb_idx;
	void __iomem *cb_base;
	u32 val32;
	u64 val64;

	/* Calculate which CB this is */
	page_offset = offset >> smmu->pgshift;
	if (page_offset < 2)
		return -EINVAL;  /* Pages 0-1 are GR0/GR1 */

	cb_idx = page_offset - 2;
	if (cb_idx >= smmu->num_context_banks)
		return -EINVAL;

	cb_offset = offset & ((1 << smmu->pgshift) - 1);
	cb_base = smmu->base + ((cb_idx + 2) << smmu->pgshift);

	/* SCTLR - System Control Register */
	if (cb_offset == ARM_SMMU_CB_SCTLR) {
		if (is_write) {
			val32 = (u32)*val;

			/* Store in shadow state */
			smmu->cb_state[cb_idx].sctlr = val32;

			/* Write to hardware */
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = smmu->cb_state[cb_idx].sctlr;
			return 0;
		}
	}

	/* TCR2 - Translation Control Register 2 (VTCR for Stage-2) */
	if (cb_offset == ARM_SMMU_CB_TCR2) {
		if (is_write) {
			val32 = (u32)*val;

			/* Store in shadow state */
			smmu->cb_state[cb_idx].vtcr = val32;

			/* Write to hardware */
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = smmu->cb_state[cb_idx].vtcr;
			return 0;
		}
	}

	/* TCR - Translation Control Register (Stage-1) */
	if (cb_offset == ARM_SMMU_CB_TCR) {
		if (is_write) {
			val32 = (u32)*val;

			/* Store in shadow state */
			smmu->cb_state[cb_idx].tcr = val32;

			/*
			 * For Stage-2-only protected domains, TCR is not used
			 * by hardware (VTCR/TCR2 controls translation).
			 * Write through for completeness but it has no effect.
			 */
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = smmu->cb_state[cb_idx].tcr;
			return 0;
		}
	}

	/* TTBR0 - Translation Table Base Register 0 */
	if (cb_offset == ARM_SMMU_CB_TTBR0) {
		if (is_write) {
			val64 = *val;

			/*
			 * For Stage-2-only protected domains:
			 * - Host writes TTBR0 thinking it's programming S2 PT base
			 * - We shadow host's write but use EL2's idmap_pgtable instead
			 * - Hardware actually uses ttbr0_s2 (programmed during CB init)
			 */
			if (smmu->cb_state[cb_idx].active &&
			    smmu->cb_state[cb_idx].domain_id != 0) {
				/* Shadow host's TTBR0 but don't write to hardware */
				smmu->cb_state[cb_idx].ttbr1_s1 = val64;

				/*
				 * Keep hardware TTBR0 pointing to EL2's Stage-2 PT.
				 * This was set during smmu_v2_init_context_bank().
				 * Do NOT overwrite it with host's value.
				 */
				hyp_dbg("SMMU[%u]: CB%u TTBR0 write shadowed (0x%llx), HW unchanged\n",
					smmu->id, cb_idx, val64);
				return 0;
			} else {
				/* Host/bypass domains: write through */
				writeq_relaxed(val64, cb_base + cb_offset);
				if (smmu->has_secondary_base) {
					void __iomem *cb_base_sec = smmu->base_sec +
						((cb_idx + 2) << smmu->pgshift);
					writeq_relaxed(val64, cb_base_sec + cb_offset);
				}
				return 0;
			}
		} else {
			/* Return shadow state (what host thinks it programmed) */
			if (smmu->cb_state[cb_idx].active &&
			    smmu->cb_state[cb_idx].domain_id != 0) {
				*val = smmu->cb_state[cb_idx].ttbr1_s1;
			} else {
				*val = readq_relaxed(cb_base + cb_offset);
			}
			return 0;
		}
	}

	/* TTBR1 - Translation Table Base Register 1 (Stage-1 second PT) */
	if (cb_offset == ARM_SMMU_CB_TTBR1) {
		if (is_write) {
			val64 = *val;

			/*
			 * TTBR1 is only used in Stage-1 translation.
			 * For Stage-2-only domains, this is not used.
			 * Write through for host/bypass domains.
			 */
			writeq_relaxed(val64, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writeq_relaxed(val64, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = readq_relaxed(cb_base + cb_offset);
			return 0;
		}
	}

	/* MAIR0/MAIR1 - Memory Attribute Indirection Registers */
	if (cb_offset == ARM_SMMU_CB_S1_MAIR0) {
		if (is_write) {
			val64 = *val;

			/* Store in shadow state */
			smmu->cb_state[cb_idx].mair[0] = (u32)val64;
			smmu->cb_state[cb_idx].mair[1] = (u32)(val64 >> 32);

			/* Write to hardware */
			writeq_relaxed(val64, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writeq_relaxed(val64, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			val64 = smmu->cb_state[cb_idx].mair[0] |
				((u64)smmu->cb_state[cb_idx].mair[1] << 32);
			*val = val64;
			return 0;
		}
	}

	if (cb_offset == ARM_SMMU_CB_S1_MAIR1) {
		/* MAIR1 is the high 32 bits, but typically accessed via MAIR0 as 64-bit */
		if (is_write) {
			val32 = (u32)*val;
			smmu->cb_state[cb_idx].mair[1] = val32;
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = smmu->cb_state[cb_idx].mair[1];
			return 0;
		}
	}

	/* FSR - Fault Status Register (write-1-to-clear) */
	if (cb_offset == ARM_SMMU_CB_FSR) {
		if (is_write) {
			val32 = (u32)*val;
			/* Write-1-to-clear fault status */
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = readl_relaxed(cb_base + cb_offset);
			return 0;
		}
	}

	/* FAR - Fault Address Register (read-only) */
	if (cb_offset == ARM_SMMU_CB_FAR) {
		if (is_write)
			return -EINVAL;  /* Read-only */
		*val = readq_relaxed(cb_base + cb_offset);
		return 0;
	}

	/* FSYNR0 - Fault Syndrome Register 0 (read-only) */
	if (cb_offset == ARM_SMMU_CB_FSYNR0) {
		if (is_write)
			return -EINVAL;  /* Read-only */
		*val = readl_relaxed(cb_base + cb_offset);
		return 0;
	}

	/* TLBSYNC - Trigger CB TLB sync */
	if (cb_offset == ARM_SMMU_CB_TLBSYNC) {
		if (!is_write)
			return -EINVAL;  /* Write-only */

		return smmu_v2_tlb_sync_context(smmu, cb_idx);
	}

	/* TLBSTATUS - CB TLB sync status (read-only) */
	if (cb_offset == ARM_SMMU_CB_TLBSTATUS) {
		if (is_write)
			return -EINVAL;  /* Read-only */

		val32 = readl_relaxed(cb_base + cb_offset);
		if (smmu->has_secondary_base) {
			void __iomem *cb_base_sec = smmu->base_sec +
				((cb_idx + 2) << smmu->pgshift);
			val32 |= readl_relaxed(cb_base_sec + cb_offset);
		}
		*val = val32;
		return 0;
	}

	/* S2_TLBIIPAS2 - Stage-2 TLB invalidate by IPA */
	if (cb_offset == ARM_SMMU_CB_S2_TLBIIPAS2) {
		if (!is_write)
			return -EINVAL;  /* Write-only */

		val32 = (u32)*val;
		writel_relaxed(val32, cb_base + cb_offset);
		if (smmu->has_secondary_base) {
			void __iomem *cb_base_sec = smmu->base_sec +
				((cb_idx + 2) << smmu->pgshift);
			writel_relaxed(val32, cb_base_sec + cb_offset);
		}
		return smmu_v2_tlb_sync_context(smmu, cb_idx);
	}

	/* CONTEXTIDR - Context ID Register */
	if (cb_offset == ARM_SMMU_CB_CONTEXTIDR) {
		if (is_write) {
			val32 = (u32)*val;
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = readl_relaxed(cb_base + cb_offset);
			return 0;
		}
	}

	/* RESUME - Resume processing after stall */
	if (cb_offset == ARM_SMMU_CB_RESUME) {
		if (!is_write)
			return -EINVAL;  /* Write-only */

		val32 = (u32)*val;
		writel_relaxed(val32, cb_base + cb_offset);
		if (smmu->has_secondary_base) {
			void __iomem *cb_base_sec = smmu->base_sec +
				((cb_idx + 2) << smmu->pgshift);
			writel_relaxed(val32, cb_base_sec + cb_offset);
		}
		return 0;
	}

	/* ACTLR - Auxiliary Control Register */
	if (cb_offset == ARM_SMMU_CB_ACTLR) {
		if (is_write) {
			val32 = (u32)*val;
			writel_relaxed(val32, cb_base + cb_offset);
			if (smmu->has_secondary_base) {
				void __iomem *cb_base_sec = smmu->base_sec +
					((cb_idx + 2) << smmu->pgshift);
				writel_relaxed(val32, cb_base_sec + cb_offset);
			}
			return 0;
		} else {
			*val = readl_relaxed(cb_base + cb_offset);
			return 0;
		}
	}

	/* Unknown or unsupported register */
	return -EINVAL;
}

/**
 * smmu_v2_mmio_handler - Main MMIO trap handler
 * @addr: Physical address being accessed
 * @is_write: true for write access, false for read
 * @val: Pointer to value (read or write)
 *
 * Called by EL2 MMIO trap infrastructure when host accesses SMMU registers.
 */
bool smmu_v2_mmio_handler(u64 addr, bool is_write, u64 *val)
{
	struct hyp_arm_smmu_v2_device *smmu;
	u32 offset, page;
	int ret;

	smmu = smmu_v2_find_by_mmio_addr(addr);
	if (!smmu)
		return false;

	/* Calculate offset relative to SMMU base */
	if (addr >= smmu->mmio_addr && addr < smmu->mmio_addr + smmu->mmio_size)
		offset = addr - smmu->mmio_addr;
	else if (smmu->has_secondary_base &&
		 addr >= smmu->mmio_addr_sec && addr < smmu->mmio_addr_sec + smmu->mmio_size)
		offset = addr - smmu->mmio_addr_sec;
	else
		return false;

	page = offset & ~((1 << smmu->pgshift) - 1);

	/* Route to appropriate page handler */
	if (page == ARM_SMMU_GR0)
		ret = smmu_v2_handle_gr0(smmu, offset - ARM_SMMU_GR0, is_write, val);
	else if (page == ARM_SMMU_GR1)
		ret = smmu_v2_handle_gr1(smmu, offset - ARM_SMMU_GR1, is_write, val);
	else
		ret = smmu_v2_handle_cb(smmu, offset, is_write, val);

	return ret == 0;
}

/*
 * Context Bank Management
 */

/**
 * smmu_v2_alloc_context_bank - Allocate a free context bank
 * @smmu: SMMU device
 *
 * Returns: Context bank index, or ARM_SMMU_INVALID_CB if none available
 */
u8 smmu_v2_alloc_context_bank(struct hyp_arm_smmu_v2_device *smmu)
{
	unsigned long idx;

	hyp_spin_lock(&smmu->lock);
	idx = find_first_zero_bit(smmu->context_map, smmu->num_context_banks);
	if (idx >= smmu->num_context_banks) {
		hyp_spin_unlock(&smmu->lock);
		return ARM_SMMU_INVALID_CB;
	}

	__set_bit(idx, smmu->context_map);
	smmu->cb_state[idx].active = true;
	hyp_spin_unlock(&smmu->lock);

	return idx;
}

/**
 * smmu_v2_free_context_bank - Free a context bank
 * @smmu: SMMU device
 * @idx: Context bank index
 */
void smmu_v2_free_context_bank(struct hyp_arm_smmu_v2_device *smmu, u8 idx)
{
	if (idx >= smmu->num_context_banks)
		return;

	hyp_spin_lock(&smmu->lock);
	__clear_bit(idx, smmu->context_map);
	smmu->cb_state[idx].active = false;
	smmu->cb_state[idx].domain_id = 0;
	hyp_spin_unlock(&smmu->lock);
}

/**
 * smmu_v2_init_context_bank - Configure a context bank for a domain
 * @smmu: SMMU device
 * @domain: IOMMU domain
 * @cb_idx: Context bank index
 *
 * Programs CBAR, TTBR, TCR, and other CB registers for Stage-2 translation.
 * Uses the global identity-mapped page table (idmap_pgtable) which mirrors
 * the host's CPU stage-2 mappings.
 */
int smmu_v2_init_context_bank(struct hyp_arm_smmu_v2_device *smmu,
			       struct kvm_hyp_iommu_domain *domain, u8 cb_idx)
{
	struct smmu_v2_cb_state *cb = &smmu->cb_state[cb_idx];
	struct io_pgtable_cfg *pgt_cfg;
	u32 cbar, vtcr, sctlr, cb_page;
	u64 ttbr0, mair;

	if (cb_idx >= smmu->num_context_banks)
		return -EINVAL;

	if (WARN_ON(!idmap_pgtable))
		return -EINVAL;

	pgt_cfg = &idmap_pgtable->cfg;

	/* Calculate CB page offset: context banks start at page (2 + cb_idx) */
	cb_page = (cb_idx + 2) << smmu->pgshift;

	/* 1. Configure CBAR (Context Bank Attribute Register) for Stage-2 only */
	cbar = FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_TRANS);
	cbar |= FIELD_PREP(ARM_SMMU_CBAR_VMID, 0);  /* VMID = 0 (global identity mapping) */
	smmu_writel(smmu, ARM_SMMU_GR1, ARM_SMMU_GR1_CBAR(cb_idx), cbar);

	/* Configure CBA2R (extended attributes) for 64-bit addressing */
	smmu_writel(smmu, ARM_SMMU_GR1, ARM_SMMU_GR1_CBA2R(cb_idx),
		    ARM_SMMU_CBA2R_VA64);

	/* 2. Program VTCR via TCR2 register for Stage-2 translation control */
	vtcr = ARM_SMMU_VTCR_RES1;  /* Reserved bit that must be 1 */

	/* Extract configuration from global page table */
	vtcr |= (pgt_cfg->arm_lpae_s2_cfg.vtcr.ps << 16);    /* Physical address size */
	vtcr |= (pgt_cfg->arm_lpae_s2_cfg.vtcr.tg << 14);    /* Translation granule */
	vtcr |= (pgt_cfg->arm_lpae_s2_cfg.vtcr.sh << 12);    /* Shareability */
	vtcr |= (pgt_cfg->arm_lpae_s2_cfg.vtcr.orgn << 10);  /* Outer cacheability */
	vtcr |= (pgt_cfg->arm_lpae_s2_cfg.vtcr.irgn << 8);   /* Inner cacheability */
	vtcr |= (pgt_cfg->arm_lpae_s2_cfg.vtcr.sl << 6);     /* Start level */
	vtcr |= (64 - pgt_cfg->ias);                          /* T0SZ: input address size */

	smmu_writeq(smmu, cb_page, ARM_SMMU_CB_TCR2, vtcr);

	/* 3. Write TTBR0 with Stage-2 page table base address */
	ttbr0 = pgt_cfg->arm_lpae_s2_cfg.vttbr;
	smmu_writeq(smmu, cb_page, ARM_SMMU_CB_TTBR0, ttbr0);

	/* 4. Enable translation by setting SCTLR.M bit */
	sctlr = ARM_SMMU_SCTLR_M;        /* Enable MMU */
	sctlr |= ARM_SMMU_SCTLR_TRE;     /* TEX remap enable */
	sctlr |= ARM_SMMU_SCTLR_AFE;     /* Access flag enable */
	sctlr |= ARM_SMMU_SCTLR_CFIE;    /* Context fault interrupt enable */
	sctlr |= ARM_SMMU_SCTLR_CFRE;    /* Context fault report enable */
	smmu_writel(smmu, cb_page, ARM_SMMU_CB_SCTLR, sctlr);

	/* Update CB state tracking */
	cb->domain_id = domain->domain_id;
	cb->cbar = cbar;
	cb->vtcr = vtcr;
	cb->ttbr0_s2 = ttbr0;
	cb->sctlr = sctlr;
	cb->vmid = 0;  /* Global VMID for identity mapping */
	cb->active = true;

	return 0;
}

/**
 * smmu_v2_global_init - Global initialization for all SMMU instances
 *
 * Called once during hypervisor initialization to set up all SMMU devices
 * and create the global identity-mapped page table.
 *
 * This should be called from the kvm_iommu_ops->init() callback.
 *
 * Returns: 0 on success, negative error code on failure
 */
int smmu_v2_global_init(void)
{
	struct hyp_arm_smmu_v2_device *smmu;
	int i, ret;

	/* Initialize each SMMU instance */
	for (i = 0; i < ARM_SMMU_MAX_INSTANCES; i++) {
		smmu = kvm_hyp_arm_smmu_v2_smmus[i];
		if (!smmu)
			continue;

		ret = smmu_v2_init(smmu);
		if (WARN_ON(ret))
			return ret;
	}

	/* Initialize global identity-mapped page table (shared by all SMMUs) */
	ret = smmu_v2_init_pgt();
	if (WARN_ON(ret))
		return ret;

	return 0;
}

/*
 * Stream Mapping
 */

/**
 * smmu_v2_map_stream - Map a Stream ID to a context bank
 * @smmu: SMMU device
 * @sid: Stream ID
 * @cb_idx: Context bank index
 *
 * Configures SMR and S2CR registers to route traffic from @sid to @cb_idx.
 */
int smmu_v2_map_stream(struct hyp_arm_smmu_v2_device *smmu, u32 sid, u8 cb_idx)
{
	/* TODO: Implement stream mapping */
	/* 1. Find free SMR */
	/* 2. Configure SMR with SID */
	/* 3. Configure S2CR to point to CB */
	/* 4. Update shadow state */
	return -ENOSYS;
}

/**
 * smmu_v2_unmap_stream - Unmap a Stream ID
 * @smmu: SMMU device
 * @sid: Stream ID
 */
int smmu_v2_unmap_stream(struct hyp_arm_smmu_v2_device *smmu, u32 sid)
{
	/* TODO: Implement stream unmapping */
	return -ENOSYS;
}

/*
 * TLB Operations
 */

/**
 * smmu_v2_tlb_sync_global - Wait for global TLB sync to complete
 * @smmu: SMMU device
 *
 * Returns: 0 on success, -ETIMEDOUT on timeout
 */
int smmu_v2_tlb_sync_global(struct hyp_arm_smmu_v2_device *smmu)
{
	u32 val;
	unsigned int timeout = TLB_LOOP_TIMEOUT;

	/* Trigger sync */
	smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sTLBGSYNC, 0);

	/* Poll for completion with timeout */
	do {
		val = smmu_tlb_sync_status(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sTLBGSTATUS);
		if (!(val & ARM_SMMU_sTLBGSTATUS_GSACTIVE))
			return 0;
		timeout--;
	} while (timeout);

	/* Timeout - hardware error */
	hyp_err("SMMU[%u]: Global TLB sync timeout (GSACTIVE still set)\n", smmu->id);
	return -ETIMEDOUT;
}

/**
 * smmu_v2_tlb_sync_context - Wait for context TLB sync to complete
 * @smmu: SMMU device
 * @cb_idx: Context bank index
 *
 * Returns: 0 on success, -ETIMEDOUT on timeout
 */
int smmu_v2_tlb_sync_context(struct hyp_arm_smmu_v2_device *smmu, u8 cb_idx)
{
	void __iomem *cb_base;
	u32 val;
	unsigned int timeout = TLB_LOOP_TIMEOUT;

	/* Calculate context bank base address */
	cb_base = smmu->base + ((cb_idx + 2) << smmu->pgshift);

	/* Trigger sync */
	writel_relaxed(0, cb_base + ARM_SMMU_CB_TLBSYNC);
	if (smmu->has_secondary_base) {
		void __iomem *cb_base_sec = smmu->base_sec + ((cb_idx + 2) << smmu->pgshift);
		writel_relaxed(0, cb_base_sec + ARM_SMMU_CB_TLBSYNC);
	}

	/* Poll for completion with timeout */
	do {
		val = readl_relaxed(cb_base + ARM_SMMU_CB_TLBSTATUS);
		if (smmu->has_secondary_base)
			val |= readl_relaxed(smmu->base_sec + ((cb_idx + 2) << smmu->pgshift) + ARM_SMMU_CB_TLBSTATUS);

		if (!(val & BIT(0)))  /* SACTIVE bit */
			return 0;
		timeout--;
	} while (timeout);

	/* Timeout - hardware error */
	hyp_err("SMMU[%u]: Context bank %u TLB sync timeout (SACTIVE still set)\n",
		smmu->id, cb_idx);
	return -ETIMEDOUT;
}

/**
 * smmu_v2_tlb_inv_context - Invalidate all TLB entries for a context
 * @smmu: SMMU device
 * @cb_idx: Context bank index
 *
 * Uses TLBIVMID to invalidate all TLB entries for a given VMID.
 * This is more efficient than per-page invalidation for large ranges.
 */
void smmu_v2_tlb_inv_context(struct hyp_arm_smmu_v2_device *smmu, u8 cb_idx)
{
	struct smmu_v2_cb_state *cb = &smmu->cb_state[cb_idx];
	int ret;

	/* Invalidate all TLB entries for this VMID */
	smmu_writel(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_TLBIVMID, cb->vmid);

	/* Ensure invalidation completes */
	ret = smmu_v2_tlb_sync_global(smmu);
	if (ret)
		hyp_err("SMMU[%u]: TLB sync failed after context invalidation (CB %u, VMID %u)\n",
			smmu->id, cb_idx, cb->vmid);
}

/**
 * smmu_v2_tlb_inv_range - Invalidate TLB entries for an IOVA range
 * @smmu: SMMU device
 * @cb_idx: Context bank index
 * @iova: Starting IOVA
 * @size: Size of range
 * @granule: Invalidation granule
 *
 * For small ranges, use address-based invalidation. For large ranges,
 * fall back to full context invalidation which is more efficient.
 */
void smmu_v2_tlb_inv_range(struct hyp_arm_smmu_v2_device *smmu, u8 cb_idx,
			   unsigned long iova, size_t size, size_t granule)
{
	void __iomem *cb_base;
	unsigned long iova_start, iova_end;
	size_t num_pages;
	int ret;

	/* Calculate number of pages in range */
	num_pages = (size + granule - 1) / granule;

	/*
	 * Threshold for full context invalidation vs per-page:
	 * If more than 32 pages, invalidate entire context for efficiency.
	 * This avoids excessive register writes for large unmaps.
	 */
	if (num_pages > 32) {
		smmu_v2_tlb_inv_context(smmu, cb_idx);
		return;
	}

	/* Calculate context bank base address */
	cb_base = smmu->base + ((cb_idx + 2) << smmu->pgshift);

	/* Invalidate each page in the range */
	iova_start = iova & ~(granule - 1);
	iova_end = iova_start + size;

	for (; iova_start < iova_end; iova_start += granule) {
		/*
		 * Use Stage-2 TLB invalidate by IPA (S2_TLBIIPAS2).
		 * For Stage-2-only translation, this is the appropriate operation.
		 * The address is shifted right by 12 bits (4K page boundary).
		 */
		u64 addr = iova_start >> 12;

		writel_relaxed((u32)addr, cb_base + ARM_SMMU_CB_S2_TLBIIPAS2);
		if (smmu->has_secondary_base) {
			void __iomem *cb_base_sec = smmu->base_sec + ((cb_idx + 2) << smmu->pgshift);
			writel_relaxed((u32)addr, cb_base_sec + ARM_SMMU_CB_S2_TLBIIPAS2);
		}
	}

	/* Ensure TLB invalidations complete */
	ret = smmu_v2_tlb_sync_context(smmu, cb_idx);
	if (ret)
		hyp_err("SMMU[%u]: TLB sync failed after range invalidation\n", smmu->id);
}

/*
 * Stream ID Management
 */

/**
 * smmu_v2_assign_sid - Assign a Stream ID to a domain
 * @smmu_id: SMMU instance ID
 * @sid: Stream ID
 * @client_id: Memory controller client ID
 * @domain_id: Domain handle
 */
int smmu_v2_assign_sid(u32 smmu_id, u32 sid, u32 client_id, pkvm_handle_t domain_id)
{
	struct sid_assignment *entry;

	if (sid >= ARM_SMMU_MAX_SIDS)
		return -EINVAL;

	entry = &sid_map[sid];

	/* Check if already assigned */
	if (entry->active && entry->domain_id != domain_id)
		return -EBUSY;

	entry->sid = sid;
	entry->client_id = client_id;
	entry->domain_id = domain_id;
	entry->smmu_id = smmu_id;
	entry->active = true;

	return 0;
}

/**
 * smmu_v2_release_sid - Release a Stream ID assignment
 * @smmu_id: SMMU instance ID
 * @sid: Stream ID
 */
int smmu_v2_release_sid(u32 smmu_id, u32 sid)
{
	struct sid_assignment *entry;

	if (sid >= ARM_SMMU_MAX_SIDS)
		return -EINVAL;

	entry = &sid_map[sid];
	if (!entry->active || entry->smmu_id != smmu_id)
		return -EINVAL;

	entry->active = false;
	entry->domain_id = 0;

	return 0;
}

/**
 * smmu_v2_lookup_sid - Look up Stream ID assignment
 * @sid: Stream ID
 */
struct sid_assignment *smmu_v2_lookup_sid(u32 sid)
{
	if (sid >= ARM_SMMU_MAX_SIDS)
		return NULL;

	return sid_map[sid].active ? &sid_map[sid] : NULL;
}
