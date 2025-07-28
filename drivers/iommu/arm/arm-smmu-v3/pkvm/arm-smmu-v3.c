// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/trap_handler.h>

#include "arm_smmu_v3.h"
#include "../../../io-pgtable-arm.h"

#define ARM_SMMU_POLL_TIMEOUT_US	100000 /* 100ms arbitrary timeout */

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

/*
 * Wait until @cond is true.
 * Return 0 on success, or -ETIMEDOUT
 */
#define smmu_wait(_cond)					\
({								\
	int __ret = 0;						\
	u64 delay = pkvm_time_get() + ARM_SMMU_POLL_TIMEOUT_US;	\
								\
	while (!(_cond)) {					\
		if (pkvm_time_get() >= delay) {			\
			__ret = -ETIMEDOUT;			\
			break;					\
		}						\
	}							\
	__ret;							\
})

#define smmu_wait_event(_smmu, _cond)				\
({								\
	if ((_smmu)->features & ARM_SMMU_FEAT_SEV) {		\
		while (!(_cond))				\
			wfe();					\
	}							\
	smmu_wait(_cond);					\
})

static struct io_pgtable *idmap_pgtable;

static int smmu_write_cr0(struct hyp_arm_smmu_v3_device *smmu, u32 val)
{
	writel_relaxed(val, smmu->base + ARM_SMMU_CR0);
	return smmu_wait(readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == val);
}

/* Transfer ownership of structures from host to hyp */
static int smmu_take_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	return __pkvm_host_donate_hyp(phys >> PAGE_SHIFT, size >> PAGE_SHIFT);
}

static void smmu_reclaim_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	WARN_ON(__pkvm_hyp_donate_host(phys >> PAGE_SHIFT, size >> PAGE_SHIFT));
}

static bool smmu_cmdq_full(struct arm_smmu_queue *cmdq)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_full(llq);
}

static bool smmu_cmdq_empty(struct arm_smmu_queue *cmdq)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_empty(llq);
}

static int smmu_add_cmd(struct hyp_arm_smmu_v3_device *smmu,
			struct arm_smmu_cmdq_ent *ent)
{
	int ret;
	u64 cmd[CMDQ_ENT_DWORDS];
	struct arm_smmu_queue *q = &smmu->cmdq;
	struct arm_smmu_ll_queue *llq = &q->llq;

	ret = smmu_wait_event(smmu, !smmu_cmdq_full(&smmu->cmdq));
	if (ret)
		return ret;

	ret = arm_smmu_cmdq_build_cmd(cmd, ent);
	if (ret)
		return ret;

	queue_write(Q_ENT(q, llq->prod), cmd,  CMDQ_ENT_DWORDS);
	llq->prod = queue_inc_prod_n(llq, 1);
	writel_relaxed(llq->prod, q->prod_reg);
	return 0;
}

static int smmu_sync_cmd(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CMD_SYNC,
	};

	ret = smmu_add_cmd(smmu, &cmd);
	if (ret)
		return ret;

	return smmu_wait_event(smmu, smmu_cmdq_empty(&smmu->cmdq));
}

static int smmu_send_cmd(struct hyp_arm_smmu_v3_device *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret;

	if (smmu->power_is_off)
		return 0;

	ret = smmu_add_cmd(smmu, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(smmu);
}

static void __smmu_add_cmd(struct hyp_arm_smmu_v3_device *smmu, void *unused,
			   struct arm_smmu_cmdq_ent *cmd)
{
	WARN_ON(smmu_add_cmd(smmu, cmd));
}

static int smmu_tlb_inv_range_smmu(struct hyp_arm_smmu_v3_device *smmu,
				   struct arm_smmu_cmdq_ent *cmd,
				   unsigned long iova, size_t size, size_t granule)
{
	int ret;

	hyp_spin_lock(&smmu->lock);
	if (smmu->power_is_off) {
		hyp_spin_unlock(&smmu->lock);
		return 0;
	}
	arm_smmu_tlb_inv_build(cmd, iova, size, granule,
			       idmap_pgtable->cfg.pgsize_bitmap, smmu,
			       __smmu_add_cmd, NULL);
	ret = smmu_sync_cmd(smmu);
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static void smmu_tlb_inv_range(unsigned long iova, size_t size, size_t granule,
			       bool leaf)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_TLBI_S2_IPA,
		.tlbi = {
			.leaf = leaf,
			.vmid = 0,
		},
	};
	struct hyp_arm_smmu_v3_device *smmu;

	for_each_smmu(smmu)
		WARN_ON(smmu_tlb_inv_range_smmu(smmu, &cmd, iova, size, granule));
}

static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	smmu_tlb_inv_range(iova, size, granule, false);
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t granule,
			      void *cookie)
{
	smmu_tlb_inv_range(iova, granule, granule, true);
}

static const struct iommu_flush_ops smmu_tlb_ops = {
	.tlb_flush_walk = smmu_tlb_flush_walk,
	.tlb_add_page	= smmu_tlb_add_page,
};

static int smmu_sync_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_STE,
		.cfgi.sid = sid,
		.cfgi.leaf = true,
	};

	return smmu_send_cmd(smmu, &cmd);
}

static int smmu_alloc_l2_strtab(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	struct arm_smmu_strtab_l1 *l1_desc;
	struct arm_smmu_strtab_l2 *l2table;

	l1_desc = &cfg->l2.l1tab[arm_smmu_strtab_l1_idx(sid)];
	if (l1_desc->l2ptr)
		return 0;

	l2table = kvm_iommu_donate_pages_atomic(get_order(sizeof(*l2table)));
	if (!l2table)
		return -ENOMEM;

	arm_smmu_write_strtab_l1_desc(l1_desc, hyp_virt_to_phys(l2table));
	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(l1_desc, sizeof(*l1_desc));
	return 0;
}

static struct arm_smmu_ste *
smmu_get_ste_ptr(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		u32 l1_idx = arm_smmu_strtab_l1_idx(sid);
		struct arm_smmu_strtab_l2 *l2ptr;

		if (l1_idx >= cfg->l2.num_l1_ents)
			return NULL;
		l2ptr = hyp_phys_to_virt(cfg->l2.l1tab[l1_idx].l2ptr & STRTAB_L1_DESC_L2PTR_MASK);
		/* Two-level walk */
		return &l2ptr->stes[arm_smmu_strtab_l2_idx(sid)];
	}

	if (sid >= cfg->linear.num_ents)
		return NULL;
	/* Simple linear lookup */
	return &cfg->linear.table[sid];
}

__maybe_unused
static struct arm_smmu_ste *
smmu_get_alloc_ste_ptr(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		int ret = smmu_alloc_l2_strtab(smmu, sid);

		if (ret)
			return NULL;
	}
	return smmu_get_ste_ptr(smmu, sid);
}

static int smmu_init_registers(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 val, old;
	int ret;

	if (!(readl_relaxed(smmu->base + ARM_SMMU_GBPA) & GBPA_ABORT))
		return -EINVAL;

	/* Initialize all RW registers that will be read by the SMMU */
	ret = smmu_write_cr0(smmu, 0);
	if (ret)
		return ret;

	val = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
	writel_relaxed(val, smmu->base + ARM_SMMU_CR1);
	writel_relaxed(CR2_PTM, smmu->base + ARM_SMMU_CR2);

	val = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	old = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);
	/* Service Failure Mode is fatal */
	if ((val ^ old) & GERROR_SFM_ERR)
		return -EIO;
	/* Clear pending errors */
	writel_relaxed(val, smmu->base + ARM_SMMU_GERRORN);

	return 0;
}

/* Put the device in a state that can be probed by the host driver. */
static void smmu_deinit_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int i;
	size_t nr_pages = smmu->mmio_size >> PAGE_SHIFT;

	for (i = 0 ; i < nr_pages ; ++i) {
		u64 pfn = (smmu->mmio_addr >> PAGE_SHIFT) + i;

		WARN_ON(__pkvm_hyp_donate_host(pfn, 1));
	}
}

static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t cmdq_size;
	int ret;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	cmdq_size = (1 << (smmu->cmdq.llq.max_n_shift)) *
		     CMDQ_ENT_DWORDS * 8;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NORMAL_NC;

	ret = ___pkvm_host_donate_hyp_prot(smmu->cmdq.base_dma >> PAGE_SHIFT,
					   PAGE_ALIGN(cmdq_size) >> PAGE_SHIFT,
					   false, prot);
	if (ret)
		return ret;

	smmu->cmdq.base = hyp_phys_to_virt(smmu->cmdq.base_dma);
	smmu->cmdq.prod_reg = smmu->base + ARM_SMMU_CMDQ_PROD;
	smmu->cmdq.cons_reg = smmu->base + ARM_SMMU_CMDQ_CONS;
	memset(smmu->cmdq.base, 0, cmdq_size);
	writel_relaxed(0, smmu->cmdq.prod_reg);
	writel_relaxed(0, smmu->cmdq.cons_reg);

	return 0;
}

static int smmu_init_strtab(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t strtab_size;
	u64 strtab_base;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NORMAL_NC;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		strtab_size = PAGE_ALIGN(cfg->l2.num_l1_ents * sizeof(struct arm_smmu_strtab_l1));
		strtab_base = (u64)cfg->l2.l1_dma;
		cfg->linear.table = hyp_phys_to_virt(strtab_base);
	} else {
		strtab_size = PAGE_ALIGN(cfg->linear.num_ents * sizeof(struct arm_smmu_ste));
		strtab_base = (u64)cfg->linear.ste_dma;
		cfg->l2.l1tab = hyp_phys_to_virt(strtab_base);
	}
	return ___pkvm_host_donate_hyp_prot(hyp_phys_to_pfn(strtab_base),
					    strtab_size >> PAGE_SHIFT, false, prot);
}

static int smmu_reset_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cfgi_cmd = {
		.opcode = CMDQ_OP_CFGI_ALL,
	};
	struct arm_smmu_cmdq_ent tlbi_cmd = {
		.opcode = CMDQ_OP_TLBI_NSNH_ALL,
	};

	/* Invalidate all cached configs and TLBs */
	ret = smmu_write_cr0(smmu, CR0_CMDQEN);
	if (ret)
		return ret;

	ret = smmu_add_cmd(smmu, &cfgi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_add_cmd(smmu, &tlbi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_sync_cmd(smmu);
	if (ret)
		goto err_disable_cmdq;

	/* Enable translation */
	return smmu_write_cr0(smmu, CR0_SMMUEN | CR0_CMDQEN | CR0_ATSCHK | CR0_EVTQEN);

err_disable_cmdq:
	return smmu_write_cr0(smmu, 0);
}

static int smmu_init_evtq(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t evtq_size, evtq_nr_pages;
	size_t i;
	int ret;

	evtq_size = (1 << (smmu->evtq.llq.max_n_shift)) *
		     EVTQ_ENT_DWORDS * 8;
	evtq_nr_pages = PAGE_ALIGN(evtq_size) >> PAGE_SHIFT;
	for (i = 0 ; i < evtq_nr_pages ; ++i) {
		u64 evtq_pfn = hyp_phys_to_pfn(smmu->evtq.base_dma) + i;

		/*
		 * Evtq is not accessed by hyp, but set in shared state
		 * to prevent donation/sharing it to VMs.
		 */
		ret = __pkvm_host_share_hyp(evtq_pfn);
		if (ret)
			return ret;
	}

	return hyp_pin_shared_mem(hyp_phys_to_virt(smmu->evtq.base_dma),
				  hyp_phys_to_virt(smmu->evtq.base_dma + evtq_size));
}

static int smmu_power_on(struct kvm_power_domain *pd)
{
	/*
	 * We currently assume that the device retains its architectural state
	 * across power off, hence no save/restore.
	 */
	struct hyp_arm_smmu_v3_device *smmu = container_of(pd, struct hyp_arm_smmu_v3_device,
							   power_domain);

	hyp_spin_lock(&smmu->lock);
	smmu->power_is_off = false;
	hyp_spin_unlock(&smmu->lock);
	return 0;
}

static int smmu_power_off(struct kvm_power_domain *pd)
{
	struct hyp_arm_smmu_v3_device *smmu = container_of(pd, struct hyp_arm_smmu_v3_device,
							   power_domain);

	hyp_spin_lock(&smmu->lock);
	smmu->power_is_off = true;
	hyp_spin_unlock(&smmu->lock);
	return 0;
}


static const struct kvm_power_domain_ops smmu_power_ops = {
	.power_on       = smmu_power_on,
	.power_off      = smmu_power_off,
};

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int i;
	size_t nr_pages;
	int ret;

	if (!PAGE_ALIGNED(smmu->mmio_addr | smmu->mmio_size))
		return -EINVAL;

	nr_pages = smmu->mmio_size >> PAGE_SHIFT;
	for (i = 0 ; i < nr_pages ; ++i) {
		u64 pfn = (smmu->mmio_addr >> PAGE_SHIFT) + i;

		/*
		 * This should never happen, so it's fine to be strict to avoid
		 * complicated error handling.
		 */
		WARN_ON(___pkvm_host_donate_hyp(pfn, 1, true));
	}
	smmu->base = hyp_phys_to_virt(smmu->mmio_addr);

	hyp_spin_lock_init(&smmu->lock);
	BUILD_BUG_ON(sizeof(smmu->lock) != sizeof(hyp_spinlock_t));

	ret = smmu_init_registers(smmu);
	if (ret)
		goto out_err;
	ret = smmu_init_cmdq(smmu);
	if (ret)
		goto out_err;

	ret = smmu_init_evtq(smmu);
	if (ret)
		goto out_err;

	ret = smmu_init_strtab(smmu);
	if (ret)
		goto out_err;

	ret = pkvm_init_power_domain(&smmu->power_domain, &smmu_power_ops);
	if (ret)
		goto out_err;

	ret = smmu_reset_device(smmu);
	if (ret)
		goto out_err;
	return ret;

out_err:
	smmu_deinit_device(smmu);
	return ret;
}

static struct hyp_arm_smmu_v3_device *smmu_id_to_ptr(pkvm_handle_t smmu_id)
{
	if (smmu_id >= kvm_hyp_arm_smmu_v3_count)
		return NULL;
	smmu_id = array_index_nospec(smmu_id, kvm_hyp_arm_smmu_v3_count);

	return &kvm_hyp_arm_smmu_v3_smmus[smmu_id];
}

static void smmu_init_s2_ste(struct arm_smmu_ste *ste)
{
	struct io_pgtable_cfg *cfg;
	u64 ts, sl, ic, oc, sh, tg, ps;

	cfg = &idmap_pgtable->cfg;
	ps = cfg->arm_lpae_s2_cfg.vtcr.ps;
	tg = cfg->arm_lpae_s2_cfg.vtcr.tg;
	sh = cfg->arm_lpae_s2_cfg.vtcr.sh;
	oc = cfg->arm_lpae_s2_cfg.vtcr.orgn;
	ic = cfg->arm_lpae_s2_cfg.vtcr.irgn;
	sl = cfg->arm_lpae_s2_cfg.vtcr.sl;
	ts = cfg->arm_lpae_s2_cfg.vtcr.tsz;

	ste->data[0] = STRTAB_STE_0_V |
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS);
	ste->data[1] = FIELD_PREP(STRTAB_STE_1_SHCFG, STRTAB_STE_1_SHCFG_INCOMING);
	ste->data[2] = FIELD_PREP(STRTAB_STE_2_VTCR,
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2PS, ps) |
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2TG, tg) |
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2SH0, sh) |
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2OR0, oc) |
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2IR0, ic) |
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2SL0, sl) |
				  FIELD_PREP(STRTAB_STE_2_VTCR_S2T0SZ, ts)) |
		 FIELD_PREP(STRTAB_STE_2_S2VMID, 0) |
		 STRTAB_STE_2_S2AA64 | STRTAB_STE_2_S2R;
	ste->data[3] = cfg->arm_lpae_s2_cfg.vttbr & STRTAB_STE_3_S2TTB_MASK;
}

static int smmu_attach_dev(pkvm_handle_t iommu, pkvm_handle_t domain, pkvm_handle_t dev,
			   u32 pasid, u32 pasid_bits, unsigned long flags)
{
	static struct arm_smmu_ste *ste, target;
	struct hyp_arm_smmu_v3_device *smmu = smmu_id_to_ptr(iommu);
	int ret;

	if (!smmu)
		return -ENODEV;

	hyp_spin_lock(&smmu->lock);
	ste = smmu_get_alloc_ste_ptr(smmu, dev);
	if (!ste) {
		ret = -EINVAL;
		goto out_ret;
	}

	smmu_init_s2_ste(&target);
	WRITE_ONCE(ste->data[1], target.data[1]);
	WRITE_ONCE(ste->data[2], target.data[2]);
	WRITE_ONCE(ste->data[3], target.data[3]);
	smmu_sync_ste(smmu, dev);
	WRITE_ONCE(ste->data[0], target.data[0]);
	ret = smmu_sync_ste(smmu, dev);

out_ret:
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static int smmu_detach_dev(pkvm_handle_t iommu, pkvm_handle_t domain, pkvm_handle_t dev,
			   u32 pasid)
{
	static struct arm_smmu_ste *ste;
	struct hyp_arm_smmu_v3_device *smmu = smmu_id_to_ptr(iommu);
	int ret;

	if (!smmu)
		return -ENODEV;

	hyp_spin_lock(&smmu->lock);
	ste = smmu_get_alloc_ste_ptr(smmu, dev);
	if (!ste) {
		ret = -EINVAL;
		goto out_ret;
	}

	WRITE_ONCE(ste->data[0], 0);
	smmu_sync_ste(smmu, dev);
	WRITE_ONCE(ste->data[1], 0);
	WRITE_ONCE(ste->data[2], 0);
	WRITE_ONCE(ste->data[3], 0);
	ret = smmu_sync_ste(smmu, dev);

out_ret:
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static int smmu_init_pgt(void)
{
	/* Default values overridden based on SMMUs common features. */
	struct io_pgtable_cfg cfg = (struct io_pgtable_cfg) {
		.tlb = &smmu_tlb_ops,
		.pgsize_bitmap = -1,
		.ias = 48,
		.oas = 48,
		.coherent_walk = true,
	};
	int ret = 0;
	struct hyp_arm_smmu_v3_device *smmu;

	for_each_smmu(smmu) {
		cfg.ias = min(cfg.ias, smmu->ias);
		cfg.oas = min(cfg.oas, smmu->oas);
		cfg.pgsize_bitmap &= smmu->pgsize_bitmap;
		cfg.coherent_walk &= !!(smmu->features & ARM_SMMU_FEAT_COHERENCY);
	}

	/* At least PAGE_SIZE must be supported by all SMMUs*/
	if ((cfg.pgsize_bitmap & PAGE_SIZE) == 0)
		return -EINVAL;

	idmap_pgtable = kvm_arm_io_pgtable_alloc(&cfg, NULL, ARM_64_LPAE_S2, &ret);
	return ret;
}

static int smmu_init(void)
{
	int ret;
	struct hyp_arm_smmu_v3_device *smmu;
	size_t smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) *
					  kvm_hyp_arm_smmu_v3_count);
	phys_addr_t smmu_arr_phys;

	kvm_hyp_arm_smmu_v3_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_smmus);
	smmu_arr_phys = hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_smmus);

	ret = smmu_take_pages(smmu_arr_phys, smmu_arr_size);
	if (ret)
		return ret;

	for_each_smmu(smmu) {
		ret = smmu_init_device(smmu);
		if (ret)
			goto out_reclaim_smmu;
	}

	return smmu_init_pgt();
out_reclaim_smmu:
	while (smmu != kvm_hyp_arm_smmu_v3_smmus)
		smmu_deinit_device(--smmu);
	smmu_reclaim_pages(smmu_arr_phys, smmu_arr_size);
	return ret;
}

static size_t smmu_pgsize_idmap(size_t size, u64 paddr, size_t pgsize_bitmap)
{
	size_t pgsizes;

	/* Remove page sizes that are larger than the current size */
	pgsizes = pgsize_bitmap & GENMASK_ULL(__fls(size), 0);

	/* Remove page sizes that the address is not aligned to. */
	if (likely(paddr))
		pgsizes &= GENMASK_ULL(__ffs(paddr), 0);

	WARN_ON(!pgsizes);

	/* Return the larget page size that fits. */
	return BIT(__fls(pgsizes));
}

static void smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
	size_t size = end - start;
	size_t pgsize = PAGE_SIZE, pgcount;
	size_t mapped, unmapped;
	int ret;
	struct io_pgtable *pgtable = idmap_pgtable;

	end = min(end, BIT(pgtable->cfg.oas));
	if (start >= end)
		return;

	if (prot) {
		if (!(prot & IOMMU_MMIO))
			prot |= IOMMU_CACHE;

		while (size) {
			mapped = 0;
			/*
			 * We handle pages size for memory and MMIO differently:
			 * - memory: Map everything with PAGE_SIZE, that is guaranteed to
			 *   find memory as we allocated enough pages to cover the entire
			 *   memory, we do that as io-pgtable-arm doesn't support
			 *   split_blk_unmap logic any more, so we can't break blocks once
			 *   mapped to tables.
			 * - MMIO: Unlike memory, pKVM allocate 1G to for all MMIO, while
			 *   the MMIO space can be large, as it is assumed to cover the
			 *   whole IAS that is not memory, we have to use block mappings,
			 *   that is fine for MMIO as it is never donated at the moment,
			 *   so we never need to unmap MMIO at the run time triggereing
			 *   split block logic.
			 */
			if (prot & IOMMU_MMIO)
				pgsize = smmu_pgsize_idmap(size, start, pgtable->cfg.pgsize_bitmap);

			pgcount = size / pgsize;
			ret = pgtable->ops.map_pages(&pgtable->ops, start, start,
						     pgsize, pgcount, prot, 0, &mapped);
			size -= mapped;
			start += mapped;
			if (!mapped || ret)
				return;
		}
	} else {
		/* Shouldn't happen. */
		WARN_ON(prot & IOMMU_MMIO);
		while (size) {
			pgcount = size / pgsize;
			unmapped = pgtable->ops.unmap_pages(&pgtable->ops, start,
							    pgsize, pgcount, NULL);
			size -= unmapped;
			start += unmapped;
			if (!unmapped)
				return;
		}
		/* Some memory were not unmapped. */
		WARN_ON(size);
	}
}

static bool smmu_dabt_device(struct hyp_arm_smmu_v3_device *smmu,
			     struct user_pt_regs *regs,
			     u64 esr, u32 off)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	const u32 no_access  = 0;
	const u32 read_write = (u32)(-1);
	const u32 read_only = is_write ? no_access : read_write;
	u32 mask = no_access;

	/*
	 * Only handle MMIO access with u32 size and alignment.
	 * We don't need to change 64-bit registers for now.
	 */
	if ((len != sizeof(u32)) || (off & (sizeof(u32) - 1)))
		return false;

	switch (off) {
	case ARM_SMMU_EVTQ_PROD + SZ_64K:
		mask = read_write;
		break;
	case ARM_SMMU_EVTQ_CONS + SZ_64K:
		mask = read_write;
		break;
	case ARM_SMMU_GERROR:
		mask = read_only;
		break;
	case ARM_SMMU_GERRORN:
		mask = read_write;
		break;
	};

	if (!mask)
		return false;
	if (is_write)
		writel_relaxed(regs->regs[rd] & mask, smmu->base + off);
	else
		regs->regs[rd] = readl_relaxed(smmu->base + off);

	return true;
}

static bool smmu_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
	struct hyp_arm_smmu_v3_device *smmu;

	for_each_smmu(smmu) {
		if (addr < smmu->mmio_addr || addr >= smmu->mmio_addr + smmu->mmio_size)
			continue;
		return smmu_dabt_device(smmu, regs, esr, addr - smmu->mmio_addr);
	}
	return false;
}

/* Shared with the kernel driver in EL1 */
struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
	.attach_dev			= smmu_attach_dev,
	.detach_dev			= smmu_detach_dev,
	.dabt_handler			= smmu_dabt_handler,
};
