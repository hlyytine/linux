// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/of_platform.h>
#include <linux/pm_runtime.h>

#include "arm-smmu-v3.h"
#include "pkvm/arm_smmu_v3.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

struct host_arm_smmu_device {
	struct arm_smmu_device		smmu;
	pkvm_handle_t			id;
	u32				boot_gbpa;
	struct kvm_power_domain         power_domain;
};

#define smmu_to_host(_smmu) \
	container_of(_smmu, struct host_arm_smmu_device, smmu);

static size_t				kvm_arm_smmu_cur;
static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;

static bool kvm_arm_smmu_validate_features(struct arm_smmu_device *smmu)
{
	unsigned int required_features =
		ARM_SMMU_FEAT_TT_LE |
		ARM_SMMU_FEAT_TRANS_S2;
	unsigned int forbidden_features =
		ARM_SMMU_FEAT_STALL_FORCE;
	unsigned int keep_features =
		ARM_SMMU_FEAT_2_LVL_STRTAB	|
		ARM_SMMU_FEAT_2_LVL_CDTAB	|
		ARM_SMMU_FEAT_TT_LE		|
		ARM_SMMU_FEAT_SEV		|
		ARM_SMMU_FEAT_COHERENCY		|
		ARM_SMMU_FEAT_TRANS_S1		|
		ARM_SMMU_FEAT_TRANS_S2		|
		ARM_SMMU_FEAT_VAX		|
		ARM_SMMU_FEAT_RANGE_INV;

	if (smmu->options & ARM_SMMU_OPT_PAGE0_REGS_ONLY) {
		dev_err(smmu->dev, "unsupported layout\n");
		return false;
	}

	if ((smmu->features & required_features) != required_features) {
		dev_err(smmu->dev, "missing features 0x%x\n",
			required_features & ~smmu->features);
		return false;
	}

	if (smmu->features & forbidden_features) {
		dev_err(smmu->dev, "features 0x%x forbidden\n",
			smmu->features & forbidden_features);
		return false;
	}

	smmu->features &= keep_features;

	return true;
}

static int kvm_arm_smmu_handle_event(struct arm_smmu_device *smmu, u64 *evt,
				    struct arm_smmu_event *event)
{
	/* KVM driver identity maps all of memory it doesn't handle events */
	return -EOPNOTSUPP;
}

static irqreturn_t kvm_arm_smmu_evt_handler(int irq, void *dev)
{
	return arm_smmu_evtq_common(irq, dev, kvm_arm_smmu_handle_event);
}

static void kvm_arm_smmu_cmdq_err(struct arm_smmu_device *smmu)
{
	dev_err(smmu->dev, "Hypervisor command queue corrupted!\n");
	BUG();
}

static irqreturn_t kvm_arm_smmu_gerror_handler(int irq, void *dev)
{
	return arm_smmu_gerror_common(irq, dev, kvm_arm_smmu_cmdq_err);
}

static irqreturn_t kvm_arm_smmu_combined_handler(int irq, void *dev)
{
	kvm_arm_smmu_gerror_handler(irq, dev);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t kvm_arm_smmu_pri_handler(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;

	dev_err(smmu->dev, "PRI not supported in KVM driver!\n");
	return IRQ_HANDLED;
}

static int kvm_arm_smmu_device_reset(struct host_arm_smmu_device *host_smmu)
{
	int ret;
	u32 reg;
	struct arm_smmu_device *smmu = &host_smmu->smmu;

	reg = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (reg & CR0_SMMUEN)
		dev_warn(smmu->dev, "SMMU currently enabled! Resetting...\n");

	/* Disable bypass */
	host_smmu->boot_gbpa = readl_relaxed(smmu->base + ARM_SMMU_GBPA);
	ret = arm_smmu_update_gbpa(smmu, GBPA_ABORT, 0);
	if (ret)
		return ret;

	ret = arm_smmu_device_disable(smmu);
	if (ret)
		return ret;

	/* Stream table */
	arm_smmu_write_strtab(smmu);

	/* Command queue */
	writeq_relaxed(smmu->cmdq.q.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);

	/* Event queue */
	writeq_relaxed(smmu->evtq.q.q_base, smmu->base + ARM_SMMU_EVTQ_BASE);
	writel_relaxed(smmu->evtq.q.llq.prod, smmu->base + SZ_64K + ARM_SMMU_EVTQ_PROD);
	writel_relaxed(smmu->evtq.q.llq.cons, smmu->base + SZ_64K + ARM_SMMU_EVTQ_CONS);

	ret = arm_smmu_setup_irqs(smmu,
				  kvm_arm_smmu_evt_handler,
				  kvm_arm_smmu_combined_handler,
				  kvm_arm_smmu_evt_handler,
				  kvm_arm_smmu_gerror_handler,
				  kvm_arm_smmu_pri_handler);
	return 0;
}

static struct platform_driver kvm_arm_smmu_driver;
static struct arm_smmu_device *
kvm_arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev;

	dev = driver_find_device_by_fwnode(&kvm_arm_smmu_driver.driver, fwnode);
	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_device *kvm_arm_smmu_probe_device(struct device *dev)
{
	struct arm_smmu_device *smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master *master;
	int ret;

	if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))
		return ERR_PTR(-EBUSY);

	smmu = kvm_arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return ERR_PTR(-ENODEV);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->smmu = smmu;
	dev_iommu_priv_set(dev, master);

	ret = arm_smmu_insert_master(smmu, master, false);
	if (ret)
		goto err_free_master;

	device_link_add(dev, smmu->dev,
			DL_FLAG_PM_RUNTIME |
			DL_FLAG_AUTOREMOVE_SUPPLIER);

	return &smmu->iommu;

	err_free_master:
	kfree(master);
	return ERR_PTR(ret);
}

static void kvm_arm_smmu_release_device(struct device *dev)
{
	int i;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(master->smmu);

	for (i = 0; i < fwspec->num_ids; i++) {
		int sid = fwspec->ids[i];

		kvm_call_hyp_nvhe(__pkvm_iommu_disable_dev, host_smmu->id, sid);
	}
	arm_smmu_remove_master(master);
}

static phys_addr_t kvm_arm_smmu_iova_to_phys(struct iommu_domain *domain,
					     dma_addr_t iova)
{
	return iova;
}

static int kvm_arm_smmu_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	int i, ret = 0;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(master->smmu);

	for (i = 0; i < fwspec->num_ids; i++) {
		int sid = fwspec->ids[i];

		ret = kvm_call_hyp_nvhe(__pkvm_iommu_enable_dev, host_smmu->id, sid);
		if (ret)
			goto out_err;
	}
	return ret;
out_err:
	while (i--)
		kvm_call_hyp_nvhe(__pkvm_iommu_disable_dev, host_smmu->id, fwspec->ids[i]);

	return ret;
}

static struct iommu_domain kvm_arm_smmu_def_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev	= kvm_arm_smmu_attach_dev,
		.iova_to_phys	= kvm_arm_smmu_iova_to_phys,
	}
};

static struct iommu_ops kvm_arm_smmu_ops = {
	.device_group		= arm_smmu_device_group,
	.of_xlate		= arm_smmu_of_xlate,
	.get_resv_regions	= arm_smmu_get_resv_regions,
	.probe_device		= kvm_arm_smmu_probe_device,
	.release_device		= kvm_arm_smmu_release_device,
	.pgsize_bitmap		= -1UL,
	.owner			= THIS_MODULE,
	.default_domain 	= &kvm_arm_smmu_def_domain,
};

static int kvm_arm_probe_power_domain(struct device *dev,
				      struct kvm_power_domain *pd)
{
	int ret;
	struct of_phandle_args args;

	if (!of_get_property(dev->of_node, "power-domains", NULL))
		return 0;

	ret = of_parse_phandle_with_args(dev->of_node, "power-domains",
					 "#power-domain-cells", 0, &args);
	if (ret)
		return ret;

	pd->type = KVM_POWER_DOMAIN_HOST_HVC;
	pd->device_id = kvm_arm_smmu_cur;
	of_node_put(args.np);
	return ret;
}

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	int ret;
	size_t size;
	phys_addr_t ioaddr;
	struct resource *res;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	struct host_arm_smmu_device *host_smmu;
	struct hyp_arm_smmu_v3_device *hyp_smmu;

	if (kvm_arm_smmu_cur >= kvm_arm_smmu_count)
		return -ENOSPC;

	hyp_smmu = &kvm_arm_smmu_array[kvm_arm_smmu_cur];

	host_smmu = devm_kzalloc(dev, sizeof(*host_smmu), GFP_KERNEL);
	if (!host_smmu)
		return -ENOMEM;

	smmu = &host_smmu->smmu;
	smmu->dev = dev;

	ret = arm_smmu_fw_probe(pdev, smmu);
	if (ret)
		return ret;

	ret = kvm_arm_probe_power_domain(dev, &host_smmu->power_domain);
	if (ret)
		return ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
	if (size < SZ_128K) {
		dev_err(dev, "unsupported MMIO region size (%pr)\n", res);
		return -EINVAL;
	}
	ioaddr = res->start;
	host_smmu->id = kvm_arm_smmu_cur;

	smmu->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);

	arm_smmu_probe_irq(pdev, smmu);

	ret = arm_smmu_device_hw_probe(smmu);
	if (ret)
		return ret;

	if (!kvm_arm_smmu_validate_features(smmu))
		return -ENODEV;

	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, smmu->base,
				      ARM_SMMU_CMDQ_PROD, ARM_SMMU_CMDQ_CONS,
				      CMDQ_ENT_DWORDS, "cmdq");
	if (ret)
		return ret;

	ret = arm_smmu_init_one_queue(smmu, &smmu->evtq.q, smmu->base + SZ_64K,
				      ARM_SMMU_EVTQ_PROD, ARM_SMMU_EVTQ_CONS,
				      EVTQ_ENT_DWORDS, "evtq");
	if (ret)
		return ret;
	ret = arm_smmu_init_strtab(smmu);
	if (ret)
		return ret;

	ret = kvm_arm_smmu_device_reset(host_smmu);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, smmu);

	if (host_smmu->power_domain.type != KVM_POWER_DOMAIN_NONE) {
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
		/*
		 * Take a reference to keep the SMMU powered on while the hypervisor
		 * initializes it.
		 */
		pm_runtime_resume_and_get(dev);
	}

	/* Hypervisor parameters */
	hyp_smmu->cmdq = smmu->cmdq.q;
	hyp_smmu->evtq = smmu->evtq.q;
	hyp_smmu->strtab_cfg = smmu->strtab_cfg;
	hyp_smmu->pgsize_bitmap = smmu->pgsize_bitmap;
	hyp_smmu->oas = smmu->oas;
	hyp_smmu->ias = smmu->ias;
	hyp_smmu->mmio_addr = ioaddr;
	hyp_smmu->mmio_size = size;
	hyp_smmu->features = smmu->features;
	hyp_smmu->power_domain = host_smmu->power_domain;
	kvm_arm_smmu_cur++;

	return arm_smmu_register_iommu(smmu, &kvm_arm_smmu_ops, ioaddr);
}

static void kvm_arm_smmu_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	/*
	 * There was an error during hypervisor setup. The hyp driver may
	 * have already enabled the device, so disable it.
	 */
	arm_smmu_device_disable(smmu);
	arm_smmu_update_gbpa(smmu, host_smmu->boot_gbpa, GBPA_ABORT);
	if (host_smmu->power_domain.type != KVM_POWER_DOMAIN_NONE) {
		pm_runtime_disable(&pdev->dev);
		pm_runtime_set_suspended(&pdev->dev);
	}
	arm_smmu_unregister_iommu(smmu);
}

static int kvm_arm_smmu_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->power_domain.type == KVM_POWER_DOMAIN_HOST_HVC)
		return kvm_call_hyp_nvhe(__pkvm_host_hvc_pd, host_smmu->id, 0);
	return 0;
}

static int kvm_arm_smmu_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->power_domain.type == KVM_POWER_DOMAIN_HOST_HVC)
		return kvm_call_hyp_nvhe(__pkvm_host_hvc_pd, host_smmu->id, 1);
	return 0;
}

static const struct dev_pm_ops kvm_arm_smmu_pm_ops = {
	SET_RUNTIME_PM_OPS(kvm_arm_smmu_suspend, kvm_arm_smmu_resume, NULL)
};

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver kvm_arm_smmu_driver = {
	.driver = {
		.name = "kvm-arm-smmu-v3",
		.of_match_table = arm_smmu_of_match,
		.pm = &kvm_arm_smmu_pm_ops,
	},
	.remove = kvm_arm_smmu_remove,
};

static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order;
	struct device_node *np;

	kvm_arm_smmu_count = 0;
	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return 0;

	/* Allocate the parameter list shared with the hypervisor */
	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
						      smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;

	return 0;
}

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
}

static int smmu_put_device(struct device *dev, void *data)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->power_domain.type != KVM_POWER_DOMAIN_NONE)
		pm_runtime_put(dev);

	return 0;
}

static int kvm_arm_smmu_v3_init_drv(void)
{
	int ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		goto err_free;

	if (kvm_arm_smmu_cur != kvm_arm_smmu_count) {
		/* A device exists but failed to probe */
		ret = -EUNATCH;
		goto err_free;
	}

	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_count;
	return 0;

err_free:
	kvm_arm_smmu_array_free();
	return ret;
}

static void kvm_arm_smmu_v3_remove_drv(void)
{
	/* Prevent post init for doing anything. */
	kvm_arm_smmu_count = 0;
	platform_driver_unregister(&kvm_arm_smmu_driver);
}

size_t smmu_hyp_pgt_pages(void)
{
	/*
	 * SMMUv3 uses the same format as stage-2 and hence have the same memory
	 * requirements, we add extra 100 pages for L2 ste.
	 */
	if (of_find_compatible_node(NULL, NULL, "arm,smmu-v3"))
		return host_s2_pgtable_pages() + 100;
	return 0;
}

struct kvm_iommu_driver kvm_smmu_v3_ops = {
	.init_driver = kvm_arm_smmu_v3_init_drv,
	.remove_driver = kvm_arm_smmu_v3_remove_drv,
};

static int kvm_arm_smmu_v3_register(void)
{
	int ret;

	if (!is_protected_kvm_enabled())
		return 0;

	/*
	 * Only one KVM IOMMU driver can be registered, so only call the
	 * register function if any SMMUv3 exists on the platform.
	 */
	ret = kvm_arm_smmu_array_alloc();
	if (ret || !kvm_arm_smmu_count)
		return ret;

	ret = kvm_iommu_register_driver(&kvm_smmu_v3_ops,
					kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))));
	if (ret)
		kvm_arm_smmu_array_free();
	return ret;
};

/*
 * Drop the PM references of the SMMU taken at probe
 * after it's guaranteed the hypervisor as initialized the SMMUs.
 */
static int kvm_arm_smmu_v3_post_init(void)
{
	if (!kvm_arm_smmu_count)
		return 0;
	WARN_ON(driver_for_each_device(&kvm_arm_smmu_driver.driver, NULL,
		NULL, smmu_put_device));
	return 0;
}

core_initcall(kvm_arm_smmu_v3_register);
late_initcall(kvm_arm_smmu_v3_post_init);
