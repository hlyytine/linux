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
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/amba/bus.h>

#include <linux/kvm_host.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

/* Forward declarations for EL2 structures */
#define ARM_SMMU_MAX_INSTANCES		3
#define ARM_SMMU_FEAT_COHERENT_WALK	BIT(3)

struct hyp_arm_smmu_v2_device {
	phys_addr_t mmio_addr;
	phys_addr_t mmio_size;
	phys_addr_t mmio_addr_sec;
	bool has_secondary_base;
	u32 features;
	/* Rest of structure not needed by EL1 */
};

/* External EL2 symbols */
extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_v2_ops);
extern struct hyp_arm_smmu_v2_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v2_smmus)[ARM_SMMU_MAX_INSTANCES];
#define kvm_hyp_arm_smmu_v2_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v2_smmus)

#ifdef MODULE
static unsigned long pkvm_module_token;

#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(pkvm_el2_mod_va(&kvm_nvhe_sym(x), pkvm_module_token)))

int kvm_nvhe_sym(smmu_v2_init_hyp_module)(const struct pkvm_module_ops *ops);
#else
#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(kern_hyp_va(lm_alias(&kvm_nvhe_sym(x)))))
#endif

/* Global SMMU array for EL2 */
static size_t kvm_arm_smmu_v2_count;
static struct hyp_arm_smmu_v2_device *kvm_arm_smmu_v2_array;

/* Forward declarations */
struct arm_smmu_kvm_device;
struct arm_smmu_kvm_domain;

/**
 * struct arm_smmu_kvm_device - EL1 SMMU device state
 * @dev: Linux device pointer
 * @iommu: IOMMU device for framework registration
 * @hyp_smmu_id: Hypervisor SMMU instance ID (0-2 for Tegra234)
 * @base: MMIO base address (donated to EL2, no longer accessible at EL1)
 * @size: MMIO region size
 * @num_context_banks: Number of context banks
 * @num_mapping_groups: Number of stream mapping groups
 */
struct arm_smmu_kvm_device {
	struct device			*dev;
	struct iommu_device		iommu;
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
 * These use the exported kvm_iommu_* functions from arch/arm64/kvm/iommu.c
 * which handle the actual hypercalls to EL2.
 */

/* Global domain ID counter for allocation */
static atomic_t next_domain_id = ATOMIC_INIT(1);

/**
 * arm_smmu_kvm_alloc_domain_id - Allocate a new domain ID
 *
 * Domain IDs must be unique across all IOMMUs. We use a simple atomic
 * counter for allocation.
 *
 * Returns: New domain ID (>0), or negative error code
 */
static pkvm_handle_t arm_smmu_kvm_alloc_domain_id(void)
{
	return atomic_inc_return(&next_domain_id);
}

/**
 * arm_smmu_kvm_get_stream_id - Get Stream ID for a device
 * @dev: Device to get Stream ID for
 *
 * Parses the device tree "iommus" property to extract the Stream ID.
 * For Tegra234, the format is: iommus = <&smmu STREAM_ID>;
 *
 * Returns: Stream ID (0-255), or negative error code
 */
static int arm_smmu_kvm_get_stream_id(struct device *dev)
{
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells",
					  0, &args);
	if (ret) {
		dev_err(dev, "failed to parse iommus property: %d\n", ret);
		return ret;
	}

	/*
	 * For ARM SMMUs, args.args[0] is the Stream ID
	 * Tegra234 uses 8-bit Stream IDs (0-255)
	 */
	if (args.args_count < 1) {
		dev_err(dev, "iommus property has no Stream ID\n");
		of_node_put(args.np);
		return -EINVAL;
	}

	if (args.args[0] > 255) {
		dev_err(dev, "invalid Stream ID %u (max 255)\n", args.args[0]);
		of_node_put(args.np);
		return -EINVAL;
	}

	of_node_put(args.np);

	return args.args[0];
}

/*
 * IOMMU Domain Operations
 */

static struct iommu_domain *arm_smmu_kvm_domain_alloc(struct device *dev)
{
	struct arm_smmu_kvm_domain *smmu_domain;

	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return NULL;

	/* Domain will be fully initialized in attach_dev */

	return &smmu_domain->domain;
}

static void arm_smmu_kvm_domain_free(struct iommu_domain *domain)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);

	if (smmu_domain->hyp_domain_id) {
		kvm_iommu_free_domain(smmu_domain->hyp_domain_id);
		smmu_domain->hyp_domain_id = 0;
	}

	kfree(smmu_domain);
}

static int arm_smmu_kvm_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);
	struct arm_smmu_kvm_device *smmu = dev_iommu_priv_get(dev);
	int sid;
	int ret;

	if (!smmu) {
		dev_err(dev, "device not associated with SMMU\n");
		return -ENODEV;
	}

	/* Get Stream ID from device tree */
	sid = arm_smmu_kvm_get_stream_id(dev);
	if (sid < 0)
		return sid;

	/* Allocate domain at EL2 if not already done */
	if (!smmu_domain->hyp_domain_id) {
		smmu_domain->hyp_domain_id = arm_smmu_kvm_alloc_domain_id();

		ret = kvm_iommu_alloc_domain(smmu_domain->hyp_domain_id, domain->type);
		if (ret) {
			dev_err(dev, "failed to allocate domain at EL2: %d\n", ret);
			smmu_domain->hyp_domain_id = 0;
			return ret;
		}

		smmu_domain->smmu = smmu;
	}

	/* Attach device to domain at EL2 */
	ret = kvm_iommu_attach_dev(smmu->hyp_smmu_id, smmu_domain->hyp_domain_id,
				   sid, 0 /* pasid */, 0 /* pasid_bits */, 0 /* flags */);
	if (ret) {
		dev_err(dev, "failed to attach SID %u to domain %u: %d\n",
			sid, smmu_domain->hyp_domain_id, ret);
		return ret;
	}

	dev_info(dev, "attached to SMMU domain %u (SID %u)\n",
		 smmu_domain->hyp_domain_id, sid);

	return 0;
}

static int arm_smmu_kvm_map_pages(struct iommu_domain *domain,
				  unsigned long iova, phys_addr_t paddr,
				  size_t pgsize, size_t pgcount,
				  int prot, gfp_t gfp, size_t *mapped)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);
	size_t total_mapped = 0;
	int ret;

	if (!smmu_domain->hyp_domain_id)
		return -EINVAL;

	ret = kvm_iommu_map_pages(smmu_domain->hyp_domain_id, iova, paddr,
				  pgsize, pgcount, prot, gfp, &total_mapped);

	if (mapped)
		*mapped = total_mapped;

	return ret;
}

static size_t arm_smmu_kvm_unmap_pages(struct iommu_domain *domain,
				       unsigned long iova, size_t pgsize,
				       size_t pgcount,
				       struct iommu_iotlb_gather *gather)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);

	if (!smmu_domain->hyp_domain_id)
		return 0;

	return kvm_iommu_unmap_pages(smmu_domain->hyp_domain_id, iova,
				     pgsize, pgcount);
}

static phys_addr_t arm_smmu_kvm_iova_to_phys(struct iommu_domain *domain,
					     dma_addr_t iova)
{
	struct arm_smmu_kvm_domain *smmu_domain = to_arm_smmu_kvm_domain(domain);

	if (!smmu_domain->hyp_domain_id)
		return 0;

	return kvm_iommu_iova_to_phys(smmu_domain->hyp_domain_id, iova);
}

static struct iommu_device *arm_smmu_kvm_probe_device(struct device *dev)
{
	struct arm_smmu_kvm_device *smmu = NULL;
	struct of_phandle_args args;
	struct platform_device *smmu_pdev;
	int ret;

	/* Parse "iommus" property to find the SMMU */
	ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells",
					  0, &args);
	if (ret) {
		dev_dbg(dev, "no iommus property found\n");
		return ERR_PTR(-ENODEV);
	}

	/* Find the platform device for this SMMU */
	smmu_pdev = of_find_device_by_node(args.np);
	of_node_put(args.np);

	if (!smmu_pdev) {
		dev_err(dev, "SMMU device not found\n");
		return ERR_PTR(-ENODEV);
	}

	smmu = platform_get_drvdata(smmu_pdev);
	platform_device_put(smmu_pdev);

	if (!smmu) {
		dev_err(dev, "SMMU driver data not set\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	/* Store SMMU reference in device's IOMMU private data */
	dev_iommu_priv_set(dev, smmu);

	dev_info(dev, "probed by pKVM SMMU (hyp_id=%u)\n", smmu->hyp_smmu_id);

	return &smmu->iommu;
}

static void arm_smmu_kvm_release_device(struct device *dev)
{
	dev_iommu_priv_set(dev, NULL);
}

static const struct iommu_ops arm_smmu_kvm_ops = {
	.domain_alloc_paging	= arm_smmu_kvm_domain_alloc,
	.probe_device		= arm_smmu_kvm_probe_device,
	.release_device		= arm_smmu_kvm_release_device,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= arm_smmu_kvm_attach_dev,
		.map_pages	= arm_smmu_kvm_map_pages,
		.unmap_pages	= arm_smmu_kvm_unmap_pages,
		.iova_to_phys	= arm_smmu_kvm_iova_to_phys,
		.free		= arm_smmu_kvm_domain_free,
	}
};

/*
 * Platform Driver
 */

/**
 * arm_smmu_kvm_get_smmu_id - Determine SMMU instance ID from device tree
 * @pdev: Platform device
 *
 * Returns: SMMU instance ID (0-2 for Tegra234), or negative error code
 */
static int arm_smmu_kvm_get_smmu_id(struct platform_device *pdev)
{
	struct resource *res;
	phys_addr_t base;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	base = res->start;

	/* Tegra234 SMMU instance addresses (from device tree) */
	if (base == 0x8000000 || base == 0x7000000)
		return 0;  /* smmu_niso1 */
	else if (base == 0x10000000)
		return 1;  /* smmu_iso */
	else if (base == 0x12000000 || base == 0x11000000)
		return 2;  /* smmu_niso0 */

	dev_err(&pdev->dev, "unknown SMMU MMIO base 0x%llx\n", (u64)base);
	return -EINVAL;
}

static int arm_smmu_kvm_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct arm_smmu_kvm_device *smmu;
	struct resource *res;
	u32 reg;
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

	/* Determine SMMU instance ID from base address */
	ret = arm_smmu_kvm_get_smmu_id(pdev);
	if (ret < 0) {
		dev_err(dev, "failed to determine SMMU instance ID\n");
		return ret;
	}
	smmu->hyp_smmu_id = ret;

	/* Parse device tree for #iommu-cells (should be 1 for SMMUv2) */
	ret = of_property_read_u32(dev->of_node, "#iommu-cells", &reg);
	if (ret || reg != 1) {
		dev_err(dev, "invalid or missing #iommu-cells property\n");
		return -EINVAL;
	}

	/*
	 * Note: We cannot read IDR registers here because EL2 owns the
	 * SMMU hardware. The hypervisor will probe capabilities during
	 * initialization. We use hardcoded values based on Tegra234 spec.
	 */
	smmu->num_context_banks = 64;
	smmu->num_mapping_groups = 128;

	/*
	 * Memory donation to EL2:
	 * EL2 needs memory for:
	 * - SMR shadow arrays (2 * num_mapping_groups * sizeof(struct arm_smmu_smr))
	 * - S2CR shadow arrays (2 * num_mapping_groups * sizeof(struct arm_smmu_s2cr))
	 * - CB state (num_context_banks * sizeof(struct smmu_v2_cb_state))
	 *
	 * However, the actual donation is handled by EL2 via kvm_iommu_donate_pages_atomic()
	 * when needed. We don't need to pre-allocate and donate here.
	 */

	/* Initialize IOMMU device structure */
	ret = iommu_device_sysfs_add(&smmu->iommu, dev, NULL,
				      "smmu-kvm.%u", smmu->hyp_smmu_id);
	if (ret) {
		dev_err(dev, "failed to register IOMMU in sysfs: %d\n", ret);
		return ret;
	}

	ret = iommu_device_register(&smmu->iommu, &arm_smmu_kvm_ops, dev);
	if (ret) {
		dev_err(dev, "failed to register IOMMU device: %d\n", ret);
		goto err_sysfs_remove;
	}

	platform_set_drvdata(pdev, smmu);

	dev_info(dev, "pKVM ARM SMMUv2 driver initialized (hyp_id=%u, base=0x%llx)\n",
		 smmu->hyp_smmu_id, (u64)smmu->base);

	return 0;

err_sysfs_remove:
	iommu_device_sysfs_remove(&smmu->iommu);
	return ret;
}

static void arm_smmu_kvm_device_remove(struct platform_device *pdev)
{
	struct arm_smmu_kvm_device *smmu = platform_get_drvdata(pdev);

	iommu_device_unregister(&smmu->iommu);
	iommu_device_sysfs_remove(&smmu->iommu);

	if (smmu->donated_mem)
		free_pages((unsigned long)smmu->donated_mem,
			   get_order(smmu->donated_mem_size));
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

/*
 * pKVM IOMMU Driver Registration
 *
 * This must run early (subsys_initcall) to register with the pKVM framework
 * before KVM initialization.
 */

static void kvm_arm_smmu_v2_array_free(void)
{
	int order;

	if (!kvm_arm_smmu_v2_array)
		return;

	order = get_order(kvm_arm_smmu_v2_count * sizeof(*kvm_arm_smmu_v2_array));
	free_pages((unsigned long)kvm_arm_smmu_v2_array, order);
	kvm_arm_smmu_v2_array = NULL;
}

/**
 * kvm_arm_smmu_v2_array_alloc - Allocate SMMU device array for EL2
 *
 * Parses device tree to find all SMMUv2 instances, allocates shadow
 * array structures, and populates basic MMIO information for EL2.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int kvm_arm_smmu_v2_array_alloc(void)
{
	struct device_node *np;
	int smmu_order;
	int ret;
	int i = 0;

	/* Count SMMU instances in device tree */
	kvm_arm_smmu_v2_count = 0;
	for_each_compatible_node(np, NULL, "arm,mmu-500")
		kvm_arm_smmu_v2_count++;
	for_each_compatible_node(np, NULL, "nvidia,tegra234-smmu")
		kvm_arm_smmu_v2_count++;

	if (!kvm_arm_smmu_v2_count)
		return -ENODEV;

	/* Allocate array */
	smmu_order = get_order(kvm_arm_smmu_v2_count * sizeof(*kvm_arm_smmu_v2_array));
	kvm_arm_smmu_v2_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, smmu_order);
	if (!kvm_arm_smmu_v2_array)
		return -ENOMEM;

	/* Parse device tree and populate MMIO addresses */
	for_each_compatible_node(np, NULL, "arm,mmu-500") {
		struct resource res;

		ret = of_address_to_resource(np, 0, &res);
		if (ret)
			goto out_err;

		kvm_arm_smmu_v2_array[i].mmio_addr = res.start;
		kvm_arm_smmu_v2_array[i].mmio_size = resource_size(&res);

		/* Check for secondary register base (Tegra234 niso0/niso1) */
		if (!of_address_to_resource(np, 1, &res)) {
			kvm_arm_smmu_v2_array[i].mmio_addr_sec = res.start;
			kvm_arm_smmu_v2_array[i].has_secondary_base = true;
		}

		if (of_dma_is_coherent(np))
			kvm_arm_smmu_v2_array[i].features |= ARM_SMMU_FEAT_COHERENT_WALK;

		i++;
	}

	for_each_compatible_node(np, NULL, "nvidia,tegra234-smmu") {
		struct resource res;

		ret = of_address_to_resource(np, 0, &res);
		if (ret)
			goto out_err;

		kvm_arm_smmu_v2_array[i].mmio_addr = res.start;
		kvm_arm_smmu_v2_array[i].mmio_size = resource_size(&res);

		/* Check for secondary register base */
		if (!of_address_to_resource(np, 1, &res)) {
			kvm_arm_smmu_v2_array[i].mmio_addr_sec = res.start;
			kvm_arm_smmu_v2_array[i].has_secondary_base = true;
		}

		if (of_dma_is_coherent(np))
			kvm_arm_smmu_v2_array[i].features |= ARM_SMMU_FEAT_COHERENT_WALK;

		i++;
	}

	return 0;

out_err:
	kvm_arm_smmu_v2_array_free();
	return ret;
}

/**
 * smmu_v2_hyp_pgt_pages - Calculate EL2 memory pool size
 *
 * Returns: Number of pages needed for EL2 page table pool
 */
static size_t smmu_v2_hyp_pgt_pages(void)
{
	/*
	 * Request a fixed reasonable amount for IOMMU page tables.
	 * For Tegra234 with 3 SMMU instances and typical GPU virtualization:
	 * - Global identity page table: ~2000 pages (8 MB)
	 * - Per-domain page tables: ~2000 pages (8 MB)
	 * - Shadow structures (SMR/S2CR/CB state): ~100 pages (400 KB)
	 * - TLB management overhead: ~100 pages (400 KB)
	 * - Growth headroom: ~4000 pages (16 MB)
	 * Total: 8192 pages (~32 MB)
	 *
	 * This is much smaller than host_s2_pgtable_pages() which includes
	 * the entire system memory, but we only need IOMMU-mapped regions.
	 */
	if (of_find_compatible_node(NULL, NULL, "arm,mmu-500") ||
	    of_find_compatible_node(NULL, NULL, "nvidia,tegra234-smmu")) {
#ifdef MODULE
		return 1; /* Rely on kernel command line for modules */
#else
		return 8192; /* Fixed 32 MB allocation */
#endif
	}
	return 0;
}

/**
 * kvm_arm_smmu_v2_init - Initialize EL2 hypervisor driver
 *
 * Called by pKVM framework during KVM initialization. Triggers EL2
 * hypervisor driver initialization via hypercall.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int kvm_arm_smmu_v2_init(void)
{
	int ret;

#ifdef MODULE
	ret = pkvm_load_el2_module(kvm_nvhe_sym(smmu_v2_init_hyp_module),
				   &pkvm_module_token);
	if (ret) {
		pr_err("Failed to load SMMUv2 IOMMU EL2 module: %d\n", ret);
		return ret;
	}
#endif

	/* Initialize EL2 hypervisor driver */
	ret = kvm_iommu_init_hyp(ksym_ref_addr_nvhe(smmu_v2_ops));
	if (ret) {
		pr_err("Failed to initialize SMMUv2 IOMMU at EL2: %d\n", ret);
		return ret;
	}

	pr_info("ARM SMMUv2 pKVM driver initialized at EL2\n");
	return 0;
}

/* pKVM driver operations structure */
struct kvm_iommu_driver kvm_smmu_v2_ops = {
	.init_driver = kvm_arm_smmu_v2_init,
};

/**
 * kvm_arm_smmu_v2_register - Register SMMUv2 driver with pKVM framework
 *
 * This function runs early (subsys_initcall) to register the SMMUv2
 * driver with the pKVM IOMMU framework before KVM initialization.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init kvm_arm_smmu_v2_register(void)
{
	size_t nr_pages;
	int ret;

	if (!is_protected_kvm_enabled())
		return 0;

	/* Calculate memory pool size for EL2 */
	nr_pages = smmu_v2_hyp_pgt_pages();
	if (!nr_pages)
		return 0; /* No SMMU hardware found */

	/* Allocate and populate SMMU array for EL2 */
	ret = kvm_arm_smmu_v2_array_alloc();
	if (ret) {
		pr_err("Failed to allocate SMMUv2 array: %d\n", ret);
		return ret;
	}

	/* Register with pKVM framework */
	ret = kvm_iommu_register_driver(&kvm_smmu_v2_ops, nr_pages);
	if (ret) {
		pr_err("Failed to register SMMUv2 driver with pKVM: %d\n", ret);
		goto out_err;
	}

	/*
	 * Store array pointers in nVHE symbols for EL2 access.
	 * These variables are in the nVHE image and won't be accessible
	 * after KVM initialization. Ownership transfers to EL2.
	 */
	for (int i = 0; i < kvm_arm_smmu_v2_count; i++)
		kvm_hyp_arm_smmu_v2_smmus[i] = &kvm_arm_smmu_v2_array[i];

	pr_info("ARM SMMUv2 pKVM driver registered (%zu instances, %zu pages)\n",
		kvm_arm_smmu_v2_count, nr_pages);

	return 0;

out_err:
	kvm_arm_smmu_v2_array_free();
	return ret;
}

/* Register early, before KVM initialization */
subsys_initcall(kvm_arm_smmu_v2_register);

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
