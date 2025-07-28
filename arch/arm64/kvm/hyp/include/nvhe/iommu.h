/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <asm/kvm_host.h>
#include <asm/kvm_pgtable.h>

struct kvm_iommu_ops {
	int (*init)(void);
	void (*host_stage2_idmap)(phys_addr_t start, phys_addr_t end, int prot);
	int (*attach_dev)(pkvm_handle_t iommu, pkvm_handle_t domain, pkvm_handle_t dev,
			  u32 pasid, u32 pasid_bits, unsigned long flags);
	int (*detach_dev)(pkvm_handle_t iommu, pkvm_handle_t domain, pkvm_handle_t dev,
			  u32 pasid);
	bool (*dabt_handler)(struct user_pt_regs *regs, u64 esr, u64 addr);
};

int kvm_iommu_init(void *pool_base, size_t nr_pages);

void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot);
void *kvm_iommu_donate_pages_atomic(u8 order);
void kvm_iommu_reclaim_pages_atomic(void *ptr);
/* Hypercall handlers */
int kvm_iommu_alloc_domain(pkvm_handle_t domain_id, int type);
int kvm_iommu_free_domain(pkvm_handle_t domain_id);
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits,
			 unsigned long flags);
int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid);

int kvm_iommu_map_pages(pkvm_handle_t domain_id,
			unsigned long iova, phys_addr_t paddr, size_t pgsize,
			size_t pgcount, int prot, unsigned long *mapped);
size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id, unsigned long iova,
			     size_t pgsize, size_t pgcount);
phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova);
bool kvm_iommu_host_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr);
#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
