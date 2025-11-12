.. SPDX-License-Identifier: GPL-2.0

==================
pKVM DMA Isolation
==================

:Author: Android KVM <android-kvm@google.com>

Introduction
============

pKVM (pkvm.rst) supports running virtual machines that are not accessible by
the host kernel.
This is enforced by the hypervisor running in EL2 and configuring the CPU
stage-2 MMU to unmap these VMs.

However, with DMA capable devices, they access memory without going through
the CPU stage-2 MMU, which means that they can access hypervisor or VMs
memory which defeats the hypervisor isolation.

IOMMU (Input Output Memory Management Unit) can be used to restrict devices
view of the memory to protect the hypervisor and VMs.

In this document, IOMMUs refers to the IP block managing device DMA which
can be translating or not, to specifically refer to non-translating IOMMU we
use the term MPU (Memory Protection Unit).

This document is only about the host kernel IOMMU management any VM/guest
is out of scope.

HW requirements
===============

To support pKVM, IOMMUs MUST fullfill some HW requirements:

1- IOMMUs MUST be present for each DMA-capable device. The IOMMU must enforce
   memory access permissions and can be non-translating.

2- IOMMUs SHOULD operate at a granularity equal to CPU page size.

3- IOMMU MUST cover the entire input range, and the output range must cover all
   of physical memory.

4- If not all memory range is covered, all accesses not covered by the IOMMU
   MUST be denied.

5- IOMMU MMIO MUST be protected against DMA access.

6- When invalidating hardware structures of an IOMMU, the IOMMU MUST provide
   software with a mechanism to wait for the completion of all DMA transactions
   processed using the old contents of those structures.

FW requirements
===============

The SMMUs MUST reset to a state of blocking DMA, this can be achieved by
saving/restoring some of the MMIO space (ex GBPA on SMMUv3), see "SMMUv3"
section for more.


Hypervisor IOMMU drivers
========================
To manage IOMMUs from the hypervisor, a driver is needed to run in EL2.

Driver init
-----------
A kernel driver must register the hypervisor driver with KVM at boot before
the kernel de-privileges, for built-in drivers this can be done by core_initcall
while for modules, this must be done from the module_init(), and the module must
be loaded from the kernel command line ``kvm-arm.protected_modules=module_name``

To register a driver with KVM, the kernel driver would call:
- ``kvm_iommu_register_driver(struct kvm_iommu_driver *kernel_ops, size_t nr_pages)``

Where kernel_ops is "kvm_iommu_driver" including various operations needed by
KVM to interact with the kernel (EL1) part of the driver.

nr_pages is explained next in "Memory Allocation"

Later, when the hypervisor is ready, the callback "init_driver" will be called.
This is expected to do any probe/init needed while the kernel is still trusted
(as sharing global data with hyp).

At the end of this callback
``kvm_iommu_init_hyp(struct kvm_iommu_ops* hyp_ops, pkvm_handle_t *drv_id)``
should be called to init the hypervisor part of the driver.
Where the "hyp_ops" is the pointer to the struct in the hypervisor driver, this
include all the main callbacks needed for run-time to interact with the driver.
And "drv_id" is the returned driver id, as it is possible to add register multiple
drivers in the hypervisor.
This ID will be used in some of the hypercalls (``alloc_domain``, ``set_identity``)

Memory Allocation
-----------------
The hypervisor manages 2 pools of memory where drivers can use:

1- Atomic pool: is allocated early at boot from the pKVM carveout, memory can
be allocated/freed using kvm_iommu_donate_pages_atomic/reclaim_pages_atomic
from the hypervisor driver.

Size of this carveout is decided based on:
- CONFIG_IOMMU_POOL_PAGES has the default value of this pool
- kvm-arm.hyp_iommu_pages: Kernel command line can override this value

At driver init, "nr_pages" is passed from the driver after probing the HW,
and if it's larger that what the hypervisor has already allocated it will fail.

This is typically used from contexts that can't fail memory allocations (ATOMIC).

2- On-demand pool: is allocated on demand, the hypervisor driver can use
kvm_iommu_donate_pages/reclaim_page to allocate from this pool. This must be
called only from context of IOMMU HVCs.

When :c:func:`kvm_iommu_donate_pages()` fails to allocate, it automatically encodes
a request which is returned from the HVC. (see hyp_reqs_smccc_encode)
Then the caller can check the return and topup the allocator and repeat the
HVC, the allocator can be topped up using __pkvm_topup_hyp_alloc_mgt_gfp()

All HVCs have wrapper functions in "arch/arm64/kvm/iommu.c" which checks
returns values of HVCs and topup the allocator if needed.

So, this can be used transparently when using the KVM IOMMU provided
abstractions.

This is typically used for HW related allocations (page table pages)

3- Heap: For small allocations (few bytes), the hypervisor support a heap
which can be used to allocate a few bytes using hyp_alloc/_free.
This pool is also on-demand and topped up similarly to case #2 and
automatically checked on HVC return from HVC wrappers.

This is typically used for data structure allocations.

Runtime
-------
There are 2 main designs that can be supported, pKVM provide 2 example drivers
for both implementations (see "SMMUv3" section).

**1) Separate page tables**

If HW supports 2 stages of translation (as SMMUv3), stage-2 can be managed by
the hypervisor (similar to the CPU).
A hypervisor IOMMU driver can create a shadow page table of the CPU stage-2,
which would be configured for all devices.

This can be done the following:

- During driver init in the hypervisor, kvm_iommu_snapshot_host_stage2()
  would be called to shadow the CPU stage-2 page table in the IOMMU.

- At the run time, host_stage2_idmap will be called from hyp_ops each
  time page state is changed, and the driver is expected is update the
  page-table to match the CPU.

For IPs as SMMUv3, to manage stage-2, it would be typically use trap & emulate
to configure the stage-2 page table for the SMMUv3.

To implement trap & emulate of MMIO, it can be unmapped from the host kernel
using :c:func:`___pkvm_host_donate_hyp()`.

Then, the driver can handle page faults using the hyp ops "dabt_handler".

It is possible to use other IPs than SMMUv3, some other simple MPUs might
have a completely separate programming interface that can be managed by
the hypervisor. In that case their page tables can be programmed in the
device without needed to use trap & emulate.


Power management
----------------

Unfortunately, due to the lack of deployed standards for power management,
the hypervisor can't have a standard way also to do power management.

However, the hyeprvisor provides the concept of power_domains which we
provide one implementation for it based on HVCs.

IOMMU can register using  :c:func:`pkvm_init_power_domain(power_domain, ops)`

Where ``power_domain`` contains the power domain configuration, and ops are
the on off calls backs.
For the HVC power domain, only device ID is needed.

Then from the kernel driver, ideally from the run-time PM kernel callbacks,
it can call, KVM functions :c:func:`pkvm_iommu_suspend(device_id)`` and
:c:func:`pkvm_iommu_resume(device_id)`` which ends up calling the ops
implemented by the hypervisor driver.

``CONFIG_ARM_SMMU_V3_PKVM_PV`` implements this interface, so you can use it
as an example.

**IMPORTANT**: Delegating the power management to the kernel which is untrusted
might put the system at risk, to be able to use the HVC power management,
devices **MUST** reset to a blocking state as mentined in "FW requirements"

**2) Para-virtual IOMMU**

If the IOMMU doesn't support dual stage of translation or doesn't have a
separate MPU for the hypervisor, the hypervisor has to manage the single
stage of the IOMMU provides a paravirtual-interface to the kernel.

This is also useful for DMA performance as a single stage will be used
instead of dual stages. But it will add extra overhead on IOMMU map/unmap
calls.

To use the para-virtual interface, the driver has to implement the hyp_ops:
- alloc/free_domain: Similar to the kernel, allocates free translation regime
- attach/detach_dev: Add/Remove device to a domain
- map/unmap_pages: Map/Unmap pages in a domain

The kernel driver will mostly forward those ops to the hypervisor, wrappers
for those exist in kvm/iommu.c

**VERY IMPORTANT**
When drivers use a para-virtual interface to map addresses (memory or MMIO)
in the IOMMU, that would make this memory accessible by DMA, which needs to
be tracked by the hypervisor to prevent donating this memory to VMs or to
itself.
That means that each time the **before** the driver maps memory in the IOMMU
it **MUST** call :c:func:`__pkvm_host_use_dma()`` to track this memory in
the hypervisor.
And each time the driver unmaps an address from the IOMMU, it **MUST** call
:c:func:`__pkvm_host_unuse_dma()`` **after** the pages have been unmapped
**and** it's TLB entries were invalidated.

You can check the ``CONFIG_ARM_SMMU_V3_PKVM_PV`` as an example.


SMMUv3
======
We provide both implementations as mentioned above

Pleanse that **both** driver provided doesn't do any save/restore of the SMMUs
state across power on/off.
And it assumes that the HW/FW will do that.
Otherwise, at least the SMMU must reset to blocking and the SW can do the restore
of state, but that to be implemented by the vendor.

Driver also assume that caches are clean upon power on.

Trap & Emulate dual translation
-------------------------------

Can be enabled using ``CONFIG_ARM_SMMU_V3_PKVM``

This config will also need the main kernel driver to be enabled
``CONFIG_ARM_SMMU_V3``

Para-virtual single-stage translation
-------------------------------------
Can be enabled using ``CONFIG_ARM_SMMU_V3_PKVM_PV``

Only **one** of those drivers must be enabled.

To use the dual-translation driver or the identity domain in the PV
driver, the kernel command line ``kvm-arm.hyp_iommu_pages`` must be
set to be able to make the driver populate the identity stage 2.
The value of this is typically close to value returned from
:c:func:`host_s2_pgtable_pages()` on your system.
Some extra pages might be required if your SMMU supports other dynamically
allocated HW structures (as L2 STE).

Other IOMMUs
============

A template empty driver exists that you can use to start a new driver,
``CONFIG_PKVM_IOMMU_TEMPLATE```

Based on the IOMMU design, you can check one of the SMMUv3 provided driver
implementation for any unclear points.

FAQ
===

Some common problems:

1- Hypervisor driver cause system crash (as SERROR) while accessing MMIO

That means that the hypervisor driver thinks the device is one while it is
not. Please check the "Power Management" section to do proper integration
between the kernel and the hypervisor.

2- DMA is fails because the IOMMU is disabled

That might be due to power management, where the hypervisor thinks the kernel
has powered off the device while it is still one. Please check the
"Power Management" section to do proper integration
between the kernel and the hypervisor.

3- SMMUv3 driver is missing some features

We try to maintain as much of feature parity with upstream driver (besides some
cases not commonly use in Android as ACPI, IOPF...)

Typically, vendors fork the driver and add any extra logic, otherwise please feel
free to reach out for any requests.
