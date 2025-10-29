// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM SMMUv2 EL1 Stub Driver for pKVM (Tegra234)
 *
 * Copyright (C) 2025 Hannu Lyytinen <hannu.lyytinen@unikie.com>
 *
 * This is a minimal EL1 driver that coordinates with the EL2 SMMUv2
 * hypervisor driver. It donates memory to EL2, issues hypercalls for
 * IOMMU operations, and integrates with the Linux IOMMU framework.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/amba/bus.h>

#include <linux/kvm_host.h>
#include <asm/kvm_mmu.h>

/* Forward declarations */
struct arm_smmu_kvm_device;
struct arm_smmu_kvm_domain;

/**
 * struct arm_smmu_kvm_device - EL1 SMMU device state
 * @dev: Linux device pointer
 * @hyp_smmu_id: Hypervisor SMMU instance ID (0-2 for Tegra234)
 * @base: MMIO base address (donated to EL2, no longer accessible at EL1)
 * @size: MMIO region size
 * @num_context_banks: Number of context banks
 * @num_mapping_groups: Number of stream mapping groups
 */
struct arm_smmu_kvm_device {
	struct device			*dev;
	u32				hyp_smmu_id;
	phys_addr_t			base;
	resource_size_t			size;
	u32				num_context_banks;
	u32				num_mapping_groups;

	/* Memory donated to EL2 */
	void				*donated_mem;
	size_t				donated_mem_size;
};

/**
 * struct arm_smmu_kvm_domain - EL1 IOMMU domain state
 * @domain: Linux IOMMU domain
 * @smmu: Owning SMMU device
 * @hyp_domain_id: Hypervisor domain handle
 */
struct arm_smmu_kvm_domain {
	struct iommu_domain		domain;
	struct arm_smmu_kvm_device	*smmu;
	u32				hyp_domain_id;
};

/* Convert Linux domain to KVM domain */
static inline struct arm_smmu_kvm_domain *
to_arm_smmu_kvm_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_kvm_domain, domain);
}

/*
 * Hypercall Wrappers
 *
 * TODO: Define actual hypercall interface. These are placeholders
 * based on the pKVM IOMMU infrastructure.
 */

static int kvm_smmu_init_device(u32 smmu_id, phys_addr_t base, size_t size)
{
	/* TODO: Call __pkvm_host_iommu_init() or equivalent */
	return -ENOSYS;
}

static int kvm_smmu_alloc_domain(u32 smmu_id, u32 *domain_id, u32 type)
{
	/* TODO: Call __pkvm_host_iommu_alloc_domain() */
	return -ENOSYS;
}

static int kvm_smmu_free_domain(u32 domain_id)
{
	/* TODO: Call __pkvm_host_iommu_free_domain() */
	return -ENOSYS;
}

static int kvm_smmu_attach_dev(u32 smmu_id, u32 domain_id, u32 sid)
{
	/* TODO: Call __pkvm_host_iommu_attach_dev() */
	return -ENOSYS;
}

static int kvm_smmu_detach_dev(u32 smmu_id, u32 domain_id, u32 sid)
{
	/* TODO: Call __pkvm_host_iommu_detach_dev() */
	return -ENOSYS;
}

static int kvm_smmu_map_pages(u32 domain_id, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize,
			      size_t pgcount, int prot)
{
	/* TODO: Call __pkvm_host_iommu_map_pages() */
	return -ENOSYS;
}

static int kvm_smmu_unmap_pages(u32 domain_id, unsigned long iova,
				size_t pgsize, size_t pgcount)
{
	/* TODO: Call __pkvm_host_iommu_unmap_pages() */
	return -ENOSYS;
}

static phys_addr_t kvm_smmu_iova_to_phys(u32 domain_id, unsigned long iova)
{
	/* TODO: Call __pkvm_host_iommu_iova_to_phys() */
	return 0;
}

/*
 * IOMMU Domain Operations
 */

static struct iommu_domain *arm_smmu_kvm_domain_alloc(unsigned type)
{
	struct arm_smmu_kvm_domain *smmu_domain;

	if (type != IOMMU_DOMAIN_UNMANAGED &&
	    type != IOMMU_DOMAIN_DMA)
		return NULL;

	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return NULL;

	/* Domain will be fully initialized in attach_dev */

	return &smmu_domain->domain;
}

static void arm_smmu_kvm_domain_free(struct iommu_domain *domain)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);

	if (smmu_domain->hyp_domain_id)
		kvm_smmu_free_domain(smmu_domain->hyp_domain_id);

	kfree(smmu_domain);
}

static int arm_smmu_kvm_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);
	struct arm_smmu_kvm_device *smmu = dev_iommu_priv_get(dev);
	u32 sid;
	int ret;

	/* TODO: Get Stream ID from device tree or firmware */
	sid = 0;  /* Placeholder */

	/* Allocate domain at EL2 if not already done */
	if (!smmu_domain->hyp_domain_id) {
		ret = kvm_smmu_alloc_domain(smmu->hyp_smmu_id,
					    &smmu_domain->hyp_domain_id,
					    domain->type);
		if (ret)
			return ret;

		smmu_domain->smmu = smmu;
	}

	/* Attach device to domain at EL2 */
	ret = kvm_smmu_attach_dev(smmu->hyp_smmu_id,
				  smmu_domain->hyp_domain_id, sid);
	if (ret)
		return ret;

	return 0;
}

static void arm_smmu_kvm_detach_dev(struct iommu_domain *domain,
				    struct device *dev)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);
	struct arm_smmu_kvm_device *smmu = dev_iommu_priv_get(dev);
	u32 sid;

	/* TODO: Get Stream ID from device tree or firmware */
	sid = 0;  /* Placeholder */

	kvm_smmu_detach_dev(smmu->hyp_smmu_id,
			    smmu_domain->hyp_domain_id, sid);
}

static int arm_smmu_kvm_map_pages(struct iommu_domain *domain,
				  unsigned long iova, phys_addr_t paddr,
				  size_t pgsize, size_t pgcount,
				  int prot, gfp_t gfp, size_t *mapped)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);
	int ret;

	ret = kvm_smmu_map_pages(smmu_domain->hyp_domain_id, iova, paddr,
				 pgsize, pgcount, prot);
	if (ret == 0 && mapped)
		*mapped = pgsize * pgcount;

	return ret;
}

static size_t arm_smmu_kvm_unmap_pages(struct iommu_domain *domain,
				       unsigned long iova, size_t pgsize,
				       size_t pgcount,
				       struct iommu_iotlb_gather *gather)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);
	int ret;

	ret = kvm_smmu_unmap_pages(smmu_domain->hyp_domain_id, iova,
				   pgsize, pgcount);
	if (ret)
		return 0;

	return pgsize * pgcount;
}

static phys_addr_t arm_smmu_kvm_iova_to_phys(struct iommu_domain *domain,
					     dma_addr_t iova)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);

	return kvm_smmu_iova_to_phys(smmu_domain->hyp_domain_id, iova);
}

static struct iommu_device *arm_smmu_kvm_probe_device(struct device *dev)
{
	struct arm_smmu_kvm_device *smmu;
	struct device_node *np;

	/* TODO: Properly find SMMU device from device tree */
	np = of_parse_phandle(dev->of_node, "iommus", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	smmu = dev_get_drvdata(of_find_device_by_node(np));
	of_node_put(np);

	if (!smmu)
		return ERR_PTR(-ENODEV);

	dev_iommu_priv_set(dev, smmu);

	return &smmu->iommu;
}

static void arm_smmu_kvm_release_device(struct device *dev)
{
	dev_iommu_priv_set(dev, NULL);
}

static struct iommu_ops arm_smmu_kvm_ops = {
	.domain_alloc		= arm_smmu_kvm_domain_alloc,
	.domain_free		= arm_smmu_kvm_domain_free,
	.attach_dev		= arm_smmu_kvm_attach_dev,
	.detach_dev		= arm_smmu_kvm_detach_dev,
	.map_pages		= arm_smmu_kvm_map_pages,
	.unmap_pages		= arm_smmu_kvm_unmap_pages,
	.iova_to_phys		= arm_smmu_kvm_iova_to_phys,
	.probe_device		= arm_smmu_kvm_probe_device,
	.release_device		= arm_smmu_kvm_release_device,
	.pgsize_bitmap		= SZ_4K | SZ_2M | SZ_1G,
};

/*
 * Platform Driver
 */

static int arm_smmu_kvm_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct arm_smmu_kvm_device *smmu;
	struct resource *res;
	int ret;

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu)
		return -ENOMEM;

	smmu->dev = dev;

	/* Get MMIO resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "missing MMIO resource\n");
		return -EINVAL;
	}

	smmu->base = res->start;
	smmu->size = resource_size(res);

	/* Parse device tree for capabilities */
	/* TODO: Read num_context_banks, num_mapping_groups from DT or probe */
	smmu->num_context_banks = 64;
	smmu->num_mapping_groups = 128;

	/* Allocate memory to donate to EL2 */
	/* TODO: Calculate required memory size */
	/* Need: SMR arrays, S2CR arrays, CB state, shadow state */
	smmu->donated_mem_size = PAGE_SIZE * 16;  /* Placeholder */
	smmu->donated_mem = (void *)__get_free_pages(GFP_KERNEL,
						     get_order(smmu->donated_mem_size));
	if (!smmu->donated_mem) {
		dev_err(dev, "failed to allocate memory for EL2\n");
		return -ENOMEM;
	}

	/* Initialize SMMU at EL2 */
	ret = kvm_smmu_init_device(smmu->hyp_smmu_id, smmu->base, smmu->size);
	if (ret) {
		dev_err(dev, "failed to initialize SMMU at EL2: %d\n", ret);
		goto err_free_mem;
	}

	/* Register with IOMMU framework */
	ret = iommu_device_register(&smmu->iommu, &arm_smmu_kvm_ops, dev);
	if (ret) {
		dev_err(dev, "failed to register IOMMU device: %d\n", ret);
		goto err_free_mem;
	}

	platform_set_drvdata(pdev, smmu);

	dev_info(dev, "pKVM ARM SMMUv2 driver initialized (hyp_id=%u)\n",
		 smmu->hyp_smmu_id);

	return 0;

err_free_mem:
	free_pages((unsigned long)smmu->donated_mem, get_order(smmu->donated_mem_size));
	return ret;
}

static int arm_smmu_kvm_device_remove(struct platform_device *pdev)
{
	struct arm_smmu_kvm_device *smmu = platform_get_drvdata(pdev);

	iommu_device_unregister(&smmu->iommu);

	if (smmu->donated_mem)
		free_pages((unsigned long)smmu->donated_mem,
			   get_order(smmu->donated_mem_size));

	return 0;
}

static const struct of_device_id arm_smmu_kvm_of_match[] = {
	{ .compatible = "arm,mmu-500" },
	{ .compatible = "nvidia,tegra234-smmu" },
	{ },
};
MODULE_DEVICE_TABLE(of, arm_smmu_kvm_of_match);

static struct platform_driver arm_smmu_kvm_driver = {
	.driver	= {
		.name			= "arm-smmu-kvm",
		.of_match_table		= arm_smmu_kvm_of_match,
		.suppress_bind_attrs	= true,
	},
	.probe	= arm_smmu_kvm_device_probe,
	.remove	= arm_smmu_kvm_device_remove,
};

static int __init arm_smmu_kvm_init(void)
{
	/* Only load if running under pKVM */
	if (!is_protected_kvm_enabled())
		return -ENODEV;

	return platform_driver_register(&arm_smmu_kvm_driver);
}
module_init(arm_smmu_kvm_init);

static void __exit arm_smmu_kvm_exit(void)
{
	platform_driver_unregister(&arm_smmu_kvm_driver);
}
module_exit(arm_smmu_kvm_exit);

MODULE_DESCRIPTION("ARM SMMUv2 EL1 stub driver for pKVM (Tegra234)");
MODULE_AUTHOR("NVIDIA Corporation");
MODULE_LICENSE("GPL");
