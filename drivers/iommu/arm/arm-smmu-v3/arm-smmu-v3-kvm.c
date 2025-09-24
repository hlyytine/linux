// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "arm-smmu-v3.h"
#include "pkvm/arm_smmu_v3.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

static size_t smmu_hyp_pgt_pages(void)
{
	/*
	 * SMMUv3 uses the same format as stage-2 and hence have the same memory
	 * requirements, we add extra 500 pages for L2 ste.
	 */
	if (of_find_compatible_node(NULL, NULL, "arm,smmu-v3"))
		return host_s2_pgtable_pages() + 500;
	return 0;
}

static struct platform_driver smmuv3_nesting_driver;
static int smmuv3_nesting_probe(struct platform_device *pdev)
{
	dev_err(&pdev->dev, "%s\n", __func__);
	return 0;
}

static int kvm_arm_smmu_v3_register(void)
{
	size_t nr_pages = smmu_hyp_pgt_pages();
	int ret;

	if (!is_protected_kvm_enabled() || !nr_pages)
		return 0;

	ret = platform_driver_probe(&smmuv3_nesting_driver, smmuv3_nesting_probe);
	if (ret) {
		pr_err("Can't bind to SMMUs: %d\n", ret);
		return ret;
	}

	return kvm_iommu_register_driver(kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))),
					 nr_pages);
};

static int kvm_arm_smmu_v3_post_init(void)
{
	platform_driver_unregister(&smmuv3_nesting_driver);
	return bus_rescan_devices(&platform_bus_type);
}

static const struct of_device_id smmuv3_nested_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver smmuv3_nesting_driver = {
	.driver = {
		.name = "smmuv3-nesting",
		.of_match_table = smmuv3_nested_of_match,
	},
};
device_initcall_sync(kvm_arm_smmu_v3_post_init);
subsys_initcall(kvm_arm_smmu_v3_register);
