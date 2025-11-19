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
#include <asm/kvm_hyp.h>

/* Shared EL1/EL2 data structures */
#include "pkvm/arm-smmu-v2-shared.h"

/* Feature flags */
#define ARM_SMMU_FEAT_COHERENT_WALK	BIT(3)

/* External EL2 symbols */
extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_v2_ops);
extern struct hyp_arm_smmu_v2_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v2_smmus);
#define kvm_hyp_arm_smmu_v2_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v2_smmus)
extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v2_count);
#define kvm_hyp_arm_smmu_v2_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v2_count)

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

	/*
	 * Parse "iommus" property to find the SMMU.
	 * Note: Stream ID population is handled by of_xlate callback,
	 * which runs automatically during device configuration.
	 */
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

/*
 * Stream ID Population and Early Boot Timing
 * ==========================================
 *
 * This driver uses the standard .of_xlate callback to populate Stream IDs from
 * device tree, following the same pattern as the standard arm-smmu driver.
 *
 * The Challenge:
 * --------------
 * We register very early (subsys_initcall_sync at ~0.11s) to bind to SMMU devices
 * before the standard arm-smmu driver (which uses module_init at ~3.6s). However,
 * when iommu_device_register() is called this early:
 *
 * 1. It tries to configure ALL devices on system buses via bus_iommu_probe()
 * 2. Some devices have dependencies that aren't ready yet at 0.11s
 * 3. These devices return -EPROBE_DEFER from dma_configure()
 * 4. The error propagates back to iommu_device_register()
 *
 * The Solution:
 * -------------
 * We accept -EPROBE_DEFER as a partial success:
 * - The IOMMU is successfully registered (driver binds to SMMU devices)
 * - Some devices couldn't be configured immediately (expected at early boot)
 * - The bus notifier will automatically retry configuration when devices become ready
 *
 * Why This Works:
 * ---------------
 * 1. When a device driver probes later (e.g., MMC at ~6s), it triggers:
 *    - BUS_NOTIFY_ADD_DEVICE notification
 *    - iommu_bus_notifier() calls iommu_probe_device()
 *    - dma_configure() → of_iommu_configure() → of_xlate()
 *    - Stream ID gets populated in fwspec
 *
 * 2. The device finds its Stream ID via tegra_dev_iommu_get_stream_id()
 * 3. Device initialization succeeds (e.g., MMC mounts rootfs)
 *
 * Implementation Details:
 * -----------------------
 * - of_xlate callback: Calls iommu_fwspec_add_ids() to populate Stream IDs
 * - Error handling: Accept -EPROBE_DEFER from iommu_device_register()
 * - Bus notifier: Automatically retries device configuration when ready
 *
 * This is the standard IOMMU driver pattern, adapted for early registration.
 *
 * Alternative Approach: Early MC Driver Initialization
 * =====================================================
 *
 * An alternative architectural approach would be to have the Memory Controller
 * (MC) driver initialize early and populate Stream IDs before IOMMU framework
 * device probing begins. This section documents this approach for future
 * implementation consideration.
 *
 * Motivation:
 * -----------
 * In NVIDIA's non-pKVM implementation (arm-smmu-nvidia.c + tegra186.c), the MC
 * driver programs SID override registers in the probe_finalize callback, which
 * happens AFTER device attachment. However, one could imagine splitting MC
 * functionality:
 *
 * 1. Early init (subsys_initcall_sync): Parse device tree, populate fwspec
 * 2. Late init (probe_finalize): Program MC SID override registers
 *
 * Hypothetical Implementation:
 * ---------------------------
 * static int __init tegra_mc_early_init(void)
 * {
 *     // For each device in device tree with "iommus" property:
 *     // 1. Parse Stream ID from "iommus" property
 *     // 2. Find corresponding IOMMU device (must be registered first!)
 *     // 3. Call iommu_fwspec_init() + iommu_fwspec_add_ids()
 *     // 4. Device now has Stream ID before bus probing begins
 * }
 * subsys_initcall_sync(tegra_mc_early_init);
 *
 * Advantages:
 * -----------
 * - More explicit control over Stream ID population timing
 * - Clearer separation: MC owns SID assignment, SMMU owns translation
 * - Potentially simpler to reason about (no bus notifier retry needed)
 *
 * Challenges:
 * -----------
 * 1. **iommu->ready flag**: Even with early fwspec population, the
 *    iommu->ready flag (iommu.c:2886) is only set AFTER bus_iommu_probe()
 *    succeeds. Early fwspec init would still hit -EPROBE_DEFER!
 *
 * 2. **Ordering dependencies**: MC early init must run AFTER SMMU registration
 *    (needs valid iommu_ops), but BEFORE device probing. Fragile timing.
 *
 * 3. **Duplication**: The of_xlate callback would become redundant, but
 *    removing it breaks standard Linux IOMMU patterns.
 *
 * 4. **Device tree walking**: MC driver would need to walk ALL devices in DT
 *    looking for "iommus" properties, duplicating IOMMU framework logic.
 *
 * Why Current Approach Was Chosen:
 * ---------------------------------
 * 1. **Standard Linux pattern**: of_xlate is how ALL mainline IOMMU drivers
 *    populate Stream IDs (arm-smmu, arm-smmu-v3, intel-iommu, amd-iommu)
 *
 * 2. **No duplication**: IOMMU framework already walks device tree and calls
 *    of_xlate at the right time. No need to duplicate this logic.
 *
 * 3. **Bus notifier handles retry**: The -EPROBE_DEFER retry mechanism is
 *    standard Linux behavior, well-tested, and works correctly.
 *
 * 4. **Same as non-pKVM**: The standard arm-smmu driver uses of_xlate. The
 *    ONLY difference is our early registration timing (0.11s vs 3.6s).
 *
 * 5. **Maintainability**: Following standard patterns makes the code easier
 *    to understand and maintain for kernel developers familiar with IOMMU.
 *
 * Future Consideration:
 * --------------------
 * If the current approach proves insufficient (e.g., if -EPROBE_DEFER causes
 * issues beyond expected behavior), the alternative approach could be
 * implemented. Key changes would be:
 *
 * 1. Add tegra_mc_early_init() in drivers/memory/tegra/tegra234.c
 * 2. Walk device tree for devices with "iommus" property
 * 3. Call iommu_fwspec_init() + iommu_fwspec_add_ids() for each device
 * 4. Ensure ordering: SMMU register → MC early init → device probing
 * 5. Keep of_xlate callback as fallback (for devices not in MC's list)
 *
 * Testing should verify:
 * - MMC devices get Stream IDs and mount rootfs successfully
 * - No increase in -EPROBE_DEFER beyond current implementation
 * - Suspend/resume works (MC reprograms SID overrides correctly)
 */

/**
 * arm_smmu_kvm_of_xlate - Populate Stream ID from device tree
 * @dev: Device to configure
 * @args: Device tree arguments (args[0] = Stream ID)
 *
 * Called by IOMMU framework when parsing device tree "iommus" property.
 * Extracts the Stream ID and populates the device's fwspec.
 *
 * Note: May be called very early (during iommu_device_register) when some
 * device dependencies aren't ready. This is expected - the bus notifier will
 * retry configuration when devices become ready.
 */
static int arm_smmu_kvm_of_xlate(struct device *dev,
				  const struct of_phandle_args *args)
{
	u32 sid = 0;

	if (args->args_count > 0)
		sid = args->args[0];

	return iommu_fwspec_add_ids(dev, &sid, 1);
}

static const struct iommu_ops arm_smmu_kvm_ops = {
	.domain_alloc_paging	= arm_smmu_kvm_domain_alloc,
	.probe_device		= arm_smmu_kvm_probe_device,
	.release_device		= arm_smmu_kvm_release_device,
	.of_xlate		= arm_smmu_kvm_of_xlate,
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

	dev_info(dev, "arm-smmu-kvm: Probing device at %pR\n",
		 platform_get_resource(pdev, IORESOURCE_MEM, 0));

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

	/*
	 * CRITICAL: Set driver data BEFORE iommu_device_register()!
	 * iommu_device_register() can trigger probe_device() callbacks
	 * for devices that are probing simultaneously at early boot.
	 * probe_device() needs to access the SMMU via platform_get_drvdata().
	 */
	platform_set_drvdata(pdev, smmu);

	ret = iommu_device_register(&smmu->iommu, &arm_smmu_kvm_ops, dev);
	if (ret && ret != -EPROBE_DEFER) {
		/*
		 * Fatal error - fail probe.
		 * Note: -EPROBE_DEFER is expected at early boot (0.11s) when
		 * some device dependencies aren't ready yet. Accept it as
		 * partial success - the bus notifier will retry device
		 * configuration when devices become ready.
		 */
		dev_err(dev, "failed to register IOMMU device: %d\n", ret);
		goto err_sysfs_remove;
	}

	if (ret == -EPROBE_DEFER) {
		dev_info(dev, "IOMMU registered with deferred device probing (expected at early boot)\n");
	}

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

	pr_info("SMMUv2: kvm_arm_smmu_v2_array_alloc() called\n");

	/* Count SMMU instances in device tree */
	kvm_arm_smmu_v2_count = 0;
	for_each_compatible_node(np, NULL, "arm,mmu-500")
		kvm_arm_smmu_v2_count++;
	for_each_compatible_node(np, NULL, "nvidia,tegra234-smmu")
		kvm_arm_smmu_v2_count++;

	pr_info("SMMUv2: Found %zu SMMU instances in device tree\n", kvm_arm_smmu_v2_count);

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

		pr_info("SMMUv2: Parsing arm,mmu-500 node %s (index %d)\n", np->full_name, i);

		ret = of_address_to_resource(np, 0, &res);
		if (ret) {
			pr_err("SMMUv2: of_address_to_resource failed for %s: %d\n", np->full_name, ret);
			goto out_err;
		}

		kvm_arm_smmu_v2_array[i].id = i;  /* SMMU instance ID */
		kvm_arm_smmu_v2_array[i].mmio_addr = res.start;
		kvm_arm_smmu_v2_array[i].mmio_size = resource_size(&res);

		pr_info("SMMUv2: SMMU[%d] at PA %llx, size %llx\n", i,
			(u64)kvm_arm_smmu_v2_array[i].mmio_addr,
			(u64)kvm_arm_smmu_v2_array[i].mmio_size);

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

		pr_info("SMMUv2: Parsing nvidia,tegra234-smmu node %s (index %d)\n", np->full_name, i);

		ret = of_address_to_resource(np, 0, &res);
		if (ret) {
			pr_err("SMMUv2: of_address_to_resource failed for %s: %d\n", np->full_name, ret);
			goto out_err;
		}

		kvm_arm_smmu_v2_array[i].id = i;  /* SMMU instance ID */
		kvm_arm_smmu_v2_array[i].mmio_addr = res.start;
		kvm_arm_smmu_v2_array[i].mmio_size = resource_size(&res);

		pr_info("SMMUv2: SMMU[%d] at PA %llx, size %llx\n", i,
			(u64)kvm_arm_smmu_v2_array[i].mmio_addr,
			(u64)kvm_arm_smmu_v2_array[i].mmio_size);

		/* Check for secondary register base */
		if (!of_address_to_resource(np, 1, &res)) {
			kvm_arm_smmu_v2_array[i].mmio_addr_sec = res.start;
			kvm_arm_smmu_v2_array[i].has_secondary_base = true;
		}

		if (of_dma_is_coherent(np))
			kvm_arm_smmu_v2_array[i].features |= ARM_SMMU_FEAT_COHERENT_WALK;

		i++;
	}

	/*
	 * Shadow arrays will be allocated by EL2 during smmu_v2_init().
	 * We don't allocate them here to avoid the complexity of memory donation.
	 */
	for (i = 0; i < kvm_arm_smmu_v2_count; i++) {
		kvm_arm_smmu_v2_array[i].smrs_shadow = NULL;
		kvm_arm_smmu_v2_array[i].s2crs_shadow = NULL;
		kvm_arm_smmu_v2_array[i].smrs_hw = NULL;
		kvm_arm_smmu_v2_array[i].s2crs_hw = NULL;

		pr_info("SMMUv2: SMMU[%d] initialized (shadow arrays will be allocated by EL2)\n", i);
	}

	pr_info("SMMUv2: Successfully allocated array for %zu SMMU instances\n", kvm_arm_smmu_v2_count);
	pr_info("SMMUv2: Array physical address: %llx\n", (u64)virt_to_phys(kvm_arm_smmu_v2_array));

	return 0;

out_err:
	pr_err("SMMUv2: Array allocation failed with error %d\n", ret);
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

	pr_info("SMMUv2: kvm_arm_smmu_v2_register() called\n");

	if (!is_protected_kvm_enabled()) {
		pr_info("SMMUv2: pKVM not enabled, skipping registration\n");
		return 0;
	}

	pr_info("SMMUv2: pKVM is enabled, proceeding with registration\n");

	/* Calculate memory pool size for EL2 */
	nr_pages = smmu_v2_hyp_pgt_pages();
	pr_info("SMMUv2: Calculated %zu pages needed for EL2 page tables\n", nr_pages);
	if (!nr_pages) {
		pr_info("SMMUv2: No SMMU hardware found in device tree\n");
		return 0; /* No SMMU hardware found */
	}

	/* Allocate and populate SMMU array for EL2 */
	pr_info("SMMUv2: Calling kvm_arm_smmu_v2_array_alloc()...\n");
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
	 * Store array base and count in nVHE symbols for EL2 access.
	 * These variables are in the nVHE image and accessible by EL2.
	 * EL2 will convert kernel VA to hyp VA using kern_hyp_va().
	 */
	kvm_hyp_arm_smmu_v2_smmus = kvm_arm_smmu_v2_array;
	kvm_hyp_arm_smmu_v2_count = kvm_arm_smmu_v2_count;

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
	int ret;

	pr_info("arm-smmu-kvm: Initializing platform driver\n");

	/* Only load if running under pKVM */
	if (!is_protected_kvm_enabled()) {
		pr_info("arm-smmu-kvm: pKVM not enabled, skipping\n");
		return -ENODEV;
	}

	pr_info("arm-smmu-kvm: pKVM enabled, registering platform driver\n");
	ret = platform_driver_register(&arm_smmu_kvm_driver);
	if (ret)
		pr_err("arm-smmu-kvm: Failed to register platform driver: %d\n", ret);
	else
		pr_info("arm-smmu-kvm: Platform driver registered successfully\n");

	return ret;
}
/*
 * Register at subsys_initcall_sync level (very early, before device probing) to
 * ensure this driver binds to SMMU devices before the standard arm-smmu driver.
 * Both drivers have the same compatible string ("nvidia,tegra234-smmu"), so
 * whichever registers first will bind to the devices.
 *
 * Init levels (earliest to latest):
 *   early_initcall -> core_initcall -> postcore_initcall ->
 *   arch_initcall -> subsys_initcall -> fs_initcall ->
 *   device_initcall -> late_initcall -> module_init
 *
 * We use subsys_initcall_sync() to register before device probing begins.
 * Stream IDs are populated in probe_device() instead of of_xlate to avoid
 * -EPROBE_DEFER issues during iommu_device_register().
 */
subsys_initcall_sync(arm_smmu_kvm_init);

static void __exit arm_smmu_kvm_exit(void)
{
	platform_driver_unregister(&arm_smmu_kvm_driver);
}
module_exit(arm_smmu_kvm_exit);

MODULE_DESCRIPTION("ARM SMMUv2 EL1 stub driver for pKVM (Tegra234)");
MODULE_AUTHOR("NVIDIA Corporation");
MODULE_LICENSE("GPL");
