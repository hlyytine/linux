// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <asm/kvm_mmu.h>
#include <linux/kvm_host.h>

struct kvm_iommu_driver *iommu_driver;
extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);
extern size_t kvm_nvhe_sym(hyp_kvm_iommu_pages);

int kvm_iommu_register_driver(struct kvm_iommu_driver *kern_ops)
{
	if (WARN_ON(!kern_ops))
		return -EINVAL;

	/*
	 * Paired with smp_load_acquire(&iommu_driver)
	 * Ensure memory stores happening during a driver
	 * init are observed before executing kvm iommu callbacks.
	 */
	return cmpxchg_release(&iommu_driver, NULL, kern_ops) ? -EBUSY : 0;
}
EXPORT_SYMBOL(kvm_iommu_register_driver);

int kvm_iommu_init_hyp(struct kvm_iommu_ops *hyp_ops)
{
	if (!hyp_ops)
		return -EINVAL;

	return kvm_call_hyp_nvhe(__pkvm_iommu_init, hyp_ops);
}
EXPORT_SYMBOL(kvm_iommu_init_hyp);

int kvm_iommu_init_driver(void)
{
	/* See kvm_iommu_register_driver() */
	if (WARN_ON(!smp_load_acquire(&iommu_driver))) {
		kvm_err("pKVM enabled without an IOMMU driver, do not run confidential workload in virtual machines\n");
		return -ENODEV;
	}

	return iommu_driver->init_driver();
}
EXPORT_SYMBOL(kvm_iommu_init_driver);

void kvm_iommu_remove_driver(void)
{
	/* See kvm_iommu_register_driver() */
	if (smp_load_acquire(&iommu_driver))
		iommu_driver->remove_driver();
}

size_t kvm_iommu_pages(void)
{
	size_t nr_pages = 0;
	/*
	 * This is called very early during setup_arch() where no initcalls,
	 * so this has to call specific functions per each KVM driver.
	 */
#ifdef CONFIG_ARM_SMMU_V3_PKVM
	nr_pages = smmu_hyp_pgt_pages();
#endif

	kvm_nvhe_sym(hyp_kvm_iommu_pages) = nr_pages;
	return nr_pages;
}
