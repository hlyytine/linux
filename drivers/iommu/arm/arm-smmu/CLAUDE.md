# ARM SMMUv2 pKVM Implementation Guide

This document provides comprehensive documentation for implementing pKVM (protected KVM) support for ARM SMMUv2 on NVIDIA Tegra234 (Jetson AGX Orin).

**Parent Documentation**: See `../../../../CLAUDE.md` for general project information.

**Working Directory**: `/home/hlyytine/pkvm/Linux_for_Tegra`

**Related Paths**:
- Driver code (when implemented): `drivers/iommu/arm/arm-smmu/pkvm/`
- Kernel source: `source/kernel/linux/`
- Main project docs: `../../../../CLAUDE.md`

---

## pKVM SMMUv2 Support for Tegra234

### Overview

This section documents the architecture and implementation plan for adding pKVM (protected KVM) IOMMU support using ARM SMMUv2 on Tegra234 (Orin AGX). This enables DMA isolation for protected VMs by having the hypervisor (EL2) own and control the SMMU hardware.

**Goal**: Allow GPU and other devices to be safely passed through to protected guest VMs with guaranteed DMA isolation.

**Kernel Source**: `source/kernel/linux` - Android common kernel (android15-6.6.66_r00 or mainline 6.17+)

**Branch**: `for-android/pkvm-mainline-6.17-smmu` contains reference SMMUv3 implementation

**Target**: Tegra234 (Orin AGX) only - no support for other SoCs needed

### Architecture

#### Tegra234 SMMU Instances

Tegra234 has **three independent SMMU instances**, each controlling different sets of devices:

**1. smmu_niso1 @ 0x8000000** (+ secondary register base @ 0x7000000)
   - **Most critical for GPU virtualization**
   - **HOST1X**: Main GPU subsystem controller (coordinates all GPU engines)
   - **VIC**: Video Image Compositor
   - **NVDEC**: Video Decoder
   - **PVA0**: Programmable Vision Accelerator (+ 8 VM contexts)
   - **NVDLA0**: Deep Learning Accelerator
   - **BPMP**: Boot and Power Management Processor
   - USB controllers (XUSB_DEV, XUSB_HOST)
   - PCIe controllers (1, 2, 3, 7, 8, 10)
   - SDMMC storage controllers (1A, 4)
   - **Interrupts**: GIC_SPI 238, 242

**2. smmu_niso0 @ 0x12000000** (+ secondary register base @ 0x11000000)
   - **Critical for display and additional GPU engines**
   - **NVDISPLAY**: Main display controller
   - **NVENC**: Video Encoder
   - **OFA**: Optical Flow Accelerator
   - **NVDLA1**: Second Deep Learning Accelerator
   - **DCE**: Display Controller Engine
   - **HOST1X_CTX0-7**: Host1x context banks
   - HDA (Audio), MGBE (Multi-Gigabit Ethernet), GPCDMA, APE
   - PCIe controllers (0, 4, 5, 6, 9)
   - **Interrupts**: GIC_SPI 170, 232

**3. smmu_iso @ 0x10000000**
   - **Security-critical isolated domain**
   - **ISO_NVDISPLAY**: Secure/isolated display path (for trusted UI, DRM protected content)
   - **Interrupt**: GIC_SPI 240

**Important**: For full GPU virtualization, the pKVM driver must manage **at least smmu_niso0 and smmu_niso1**. The smmu_iso instance handles secure display paths and may be required for trusted UI scenarios.

**Multi-Instance Coordination**: Some SMMU instances have dual register bases (niso0 and niso1 each have a secondary base). Certain registers (like SMR/S2CR) must be programmed identically across both bases within the same instance. This is handled by NVIDIA's `arm-smmu-nvidia.c` platform code.

#### SMMUv2 vs SMMUv3 Key Differences

| Aspect | SMMUv2 (Tegra234) | SMMUv3 (Reference) |
|--------|-------------------|---------------------|
| Stream mapping | SMR + S2CR registers | Stream Table (flat or 2-level) |
| Command interface | Direct register writes | Command Queue (CMDQ) |
| Context allocation | Context Banks (CB) | Stream Table Entries (STE) |
| Register layout | GR0/GR1 pages, CB pages | Flat MMIO space |
| TLB invalidation | TLB_SYNC + status poll | CMDQ commands |

**Decision**: Implement parallel SMMUv2 support, do NOT refactor/consolidate with SMMUv3 code (it's still evolving, 4th patchset).

#### pKVM IOMMU Model

```
┌─────────────────────────────────────────────────┐
│  EL2 (Hypervisor)                               │
│  - Owns SMMU hardware completely                │
│  - Assigns Stream IDs dynamically               │
│  - Enforces DMA isolation                       │
│  - Validates MC SID override writes             │
└─────────────────────────────────────────────────┘
                     ▲
                     │ Hypercalls
                     │
┌─────────────────────────────────────────────────┐
│  EL1 (Host Kernel)                              │
│  - Minimal SMMU stub driver                     │
│  - MC driver (bandwidth, interconnect, etc.)    │
│  - Requests device attachments via hypercalls   │
└─────────────────────────────────────────────────┘
```

**Key Principle**: EL2 is the source of truth for all Stream ID assignments. Host cannot bypass this.

### Memory Controller (MC) Integration

#### MC's Critical Role

The Memory Controller contains **SID override registers** that map hardware client IDs to Stream IDs:

```
Hardware Client (e.g., GPU) → MC SID Override → Stream ID → SMMU → Memory
                               [0x490: 0x42]
```

**Location**: `drivers/memory/tegra/tegra234.c` - defines ~100 MC clients with their register offsets

**Security Issue**: If host can freely write MC SID override registers, it can:
- Reassign a device to use another device's Stream ID
- Bypass IOMMU isolation completely
- Access guest memory or other protected regions

#### The Solution: MC MMIO Trapping

EL2 traps the MC MMIO region and validates all SID override writes:

```c
// At EL2: MC MMIO trap handler
bool mc_sid_override_handler(u64 addr, bool is_write, u64 *val)
{
    u32 client_id = mc_offset_to_client_id(addr);
    u32 requested_sid = *val & 0xFF;

    // Validate: does this client have this SID assigned?
    if (validate_sid_for_client(client_id, requested_sid)) {
        writel(requested_sid, mc_mmio_base + addr);  // Allow
        return true;
    }

    // Security violation - deny write
    return false;
}
```

**What's Validated**:
- Client can only use the SID that EL2 assigned to it
- Cannot steal another device's SID
- Cannot use SIDs assigned to guest VMs

#### Dynamic SID Assignment (No Static Policy)

**Runtime Flow**:

1. **Host requests device attachment**:
   ```
   kvm_call_hyp(__KVM_HOST_IOMMU_ATTACH_DEV, smmu_id, domain_id, dev_id);
   ```

2. **EL2 assigns Stream ID dynamically**:
   ```c
   sid = alloc_stream_id(smmu);  // e.g., returns 0x42

   // Record: "I assigned SID 0x42 to this device for this domain"
   sid_map[0x42] = {
       .client_id = GPU,
       .assigned_sid = 0x42,
       .domain_id = guest_vm_domain,
       .active = true
   };
   ```

3. **Host MC driver programs override**:
   ```c
   // This write traps to EL2!
   writel(0x42, mc->regs + GPU_SID_OVERRIDE_OFFSET);
   ```

4. **EL2 validates and allows**:
   ```c
   // Check: did I assign SID 0x42 to GPU? Yes ✓
   // Allow write to hardware
   ```

**No static configuration files needed** - all policy is "use what you were assigned".

### Implementation Structure

#### Directory Layout

```
source/kernel/linux/drivers/iommu/arm/arm-smmu/
├── arm-smmu.c              # Existing EL1 driver (unchanged)
├── arm-smmu.h              # SMMUv2 definitions
├── arm-smmu-nvidia.c       # Tegra platform code (reference)
├── arm-smmu-kvm.c          # NEW: EL1 host stub (~250 lines)
└── pkvm/
    ├── Kbuild              # NEW: EL2 build config
    ├── arm-smmu-v2.c       # NEW: Main EL2 driver (~1000 lines)
    ├── arm-smmu-v2.h       # NEW: EL2 structures
    ├── tegra234-mc.c       # NEW: MC integration (~300 lines)
    └── tegra234-mc.h       # NEW: MC client table
```

#### What Runs Where

**At EL2** (`pkvm/arm-smmu-v2.c`):
- SMMU hardware ownership and initialization
- Stream ID allocation
- SMR/S2CR programming (stream mapping)
- Context Bank management
- TLB operations
- MMIO emulation for host accesses
- MC MMIO trapping and validation
- Uses: **io-pgtable-arm** (already EL2-ready from SMMUv3 work)

**At EL1** (`arm-smmu-kvm.c`):
- Minimal stub driver
- Parse device tree
- Allocate shadow structures
- Donate memory to EL2
- Hypercalls to EL2 for all operations

**Reused from Existing pKVM Infrastructure**:
- `arch/arm64/kvm/hyp/include/nvhe/iommu.h` - generic interface
- `arch/arm64/kvm/hyp/nvhe/iommu/iommu.c` - domain management
- `drivers/iommu/io-pgtable-arm.c` - page table operations (EL2-safe)
- Memory donation/reclaim mechanisms

### Key Data Structures

#### EL2 SMMU Device

```c
struct hyp_arm_smmu_v2_device {
    phys_addr_t mmio_addr;
    void __iomem *base;
    phys_addr_t mmio_addr_sec;      // Secondary register base (niso0, niso1 only)
    void __iomem *base_sec;
    bool has_secondary_base;
    u32 features;
    unsigned long pgsize_bitmap;

    // Context banks
    u32 num_context_banks;
    u32 num_s2_context_banks;
    DECLARE_BITMAP(context_map, ARM_SMMU_MAX_CBS);

    // Stream mapping (shadow host's config)
    struct arm_smmu_smr *smrs;     // Stream Match Registers
    struct arm_smmu_s2cr *s2crs;   // Stream-to-Context Registers
    u32 num_mapping_groups;

    // MC integration
    struct hyp_tegra_mc *mc;
};
```

#### SID Assignment Tracking

```c
struct sid_assignment {
    u32 client_id;              // TEGRA234_MEMORY_CLIENT_*
    u32 assigned_sid;           // Stream ID assigned by EL2
    pkvm_handle_t domain_id;    // Which domain owns this
    bool active;
};

// At EL2
static struct sid_assignment sid_map[256];
```

#### MC Client Information (Static)

```c
// From tegra234.c - only hardware topology info
struct mc_client_info {
    u32 client_id;              // e.g., TEGRA234_MEMORY_CLIENT_GPU
    const char *name;
    u16 sid_override_offset;    // MC register offset (e.g., 0x490)
    u16 sid_security_offset;    // Security register offset
};

static const struct mc_client_info tegra234_mc_clients[] = {
    { TEGRA234_MEMORY_CLIENT_NVDISPLAYR, "nvdisplayr", 0x490, 0x494 },
    // ... ~100 clients from drivers/memory/tegra/tegra234.c
};
```

### Implementation Status

#### Current Status: Phase 1 Complete - Hardware Initialization

**Date Completed**: 2025-10-29

Phase 1 hardware initialization is complete. The EL2 driver can now probe hardware capabilities, perform complete reset sequences, and initialize all data structures. Full ARM SMMUv2 register definitions have been imported and core initialization functions are implemented.

#### Completed Work

**1. Build System Integration** ✓
- **Kconfig** (`drivers/iommu/arm/Kconfig:75-89`)
  - Added `ARM_SMMU_V2_PKVM` configuration option
  - Dependencies: KVM, ARM_SMMU, ARCH_TEGRA
  - Selects IOMMU_IO_PGTABLE_LPAE automatically
- **Makefiles**
  - `drivers/iommu/arm/arm-smmu/Makefile` - Added pkvm/ subdirectory and arm-smmu-kvm.o
  - `drivers/iommu/arm/arm-smmu/pkvm/Makefile` - Builds arm-smmu-v2.o and tegra234-mc.o

**2. Core EL2 Driver - Phase 1 Complete** ✓
- **File**: `drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.c` (845 lines)
- **Contents**:
  - **Complete ARM SMMUv2 register definitions** (148 definitions)
    - GR0 registers: sCR0, IDR0-7, SMR, S2CR, GFAR, GFSR, TLB operations
    - GR1 registers: CBAR, CBA2R
    - CB registers: SCTLR, TTBR0/1, TCR, TCR2, MAIR, FSR, FSYNR
    - All field masks and bit positions for register programming
  - **Hardware initialization (IMPLEMENTED)**:
    - `smmu_v2_probe_device()` - Complete hardware capability probing
      - Parses ID0-ID7 registers for features, address sizes, context bank counts
      - Detects Stage-1/Stage-2 translation support
      - Handles Tegra234 walk cache erratum (forces 4KB pages)
    - `smmu_v2_reset()` - Complete hardware reset sequence
      - Clears global fault status registers
      - Resets all stream mapping groups (SMR/S2CR)
      - Disables and clears faults in all context banks
      - Invalidates all TLBs (global and non-secure)
      - Configures sCR0 with security settings
    - `smmu_v2_init()` - Initialize data structures
      - Validates donated shadow arrays from EL1
      - Initializes context bank bitmap and state
      - Initializes shadow SMR/S2CR arrays to safe fault state
      - Calls probe and reset sequence
  - MMIO emulation framework:
    - `smmu_v2_mmio_handler()` - Main trap entry point
    - `smmu_v2_handle_gr0()` - GR0 register handler stub
    - `smmu_v2_handle_gr1()` - GR1 register handler stub
    - `smmu_v2_handle_cb()` - Context bank register handler stub
  - Context bank management:
    - `smmu_v2_alloc_context_bank()` - Bitmap-based CB allocation (implemented)
    - `smmu_v2_free_context_bank()` - CB release (implemented)
    - `smmu_v2_init_context_bank()` - CB configuration stub
  - Stream mapping stubs (`smmu_v2_map_stream`, `smmu_v2_unmap_stream`)
  - TLB operation stubs:
    - `smmu_v2_tlb_sync_global()` - Basic polling implementation
    - `smmu_v2_tlb_sync_context()` - Stub
    - `smmu_v2_tlb_inv_context()` - Stub using TLBIVMID
    - `smmu_v2_tlb_inv_range()` - Stub (currently calls full context invalidation)
  - SID management:
    - `smmu_v2_assign_sid()` - Track SID assignments (implemented)
    - `smmu_v2_release_sid()` - Release SID assignments (implemented)
    - `smmu_v2_lookup_sid()` - Query SID ownership (implemented)
  - Global state arrays: `kvm_hyp_arm_smmu_v2_smmus[]`, `sid_map[]`

**3. MC Integration Skeleton** ✓
- **File**: `drivers/iommu/arm/arm-smmu/pkvm/tegra234-mc.c` (316 lines)
- **Contents**:
  - **Complete MC client table** (81 clients extracted from `drivers/memory/tegra/tegra234.c`)
    - All SID override and security register offsets
    - Client names for logging/debugging
  - MC initialization stub (`mc_init()`)
  - MMIO trap handler framework:
    - `mc_mmio_handler()` - Main MC MMIO trap entry point
    - `mc_handle_sid_override()` - SID validation logic
    - `mc_handle_sid_security()` - Security register handling stub
  - Helper functions:
    - `mc_offset_to_client()` - Map register offset to client (implemented)
    - `mc_validate_sid_for_client()` - SID assignment validation (implemented)
    - `mc_get_client_name()` - Get client name for logging (implemented)
  - Global state: `tegra234_mc`

**4. EL1 Stub Driver Skeleton** ✓
- **File**: `drivers/iommu/arm/arm-smmu/arm-smmu-kvm.c` (407 lines)
- **Contents**:
  - Data structures:
    - `struct arm_smmu_kvm_device` - EL1 device state
    - `struct arm_smmu_kvm_domain` - EL1 domain state
  - Hypercall wrappers (stubs):
    - `kvm_smmu_init_device()`, `kvm_smmu_alloc_domain()`, `kvm_smmu_free_domain()`
    - `kvm_smmu_attach_dev()`, `kvm_smmu_detach_dev()`
    - `kvm_smmu_map_pages()`, `kvm_smmu_unmap_pages()`, `kvm_smmu_iova_to_phys()`
  - IOMMU ops structure (`arm_smmu_kvm_ops`):
    - Domain operations (alloc, free, attach, detach)
    - Page mapping operations (map_pages, unmap_pages, iova_to_phys)
    - Device probe/release
  - Platform driver boilerplate:
    - `arm_smmu_kvm_device_probe()` - Device initialization
    - `arm_smmu_kvm_device_remove()` - Cleanup
    - Device tree matching: "arm,mmu-500", "nvidia,tegra234-smmu"
    - Module init/exit with pKVM detection

**5. Data Structures** ✓
- **File**: `drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.h` (281 lines, pre-existing)
- **Contents**: All data structures fully defined (see header documentation)

#### What's NOT Yet Implemented

The following areas contain stub functions marked with `TODO` comments:

**Core EL2 Driver:**
1. ✓ Hardware capability probing - Parse ID0-ID7 registers properly (COMPLETE)
2. ✓ Hardware reset sequence - Clear faults, invalidate TLBs, configure sCR0 (COMPLETE)
3. ✓ Shadow state initialization - Initialize SMR/S2CR shadow arrays (COMPLETE)
4. ✗ GR0/GR1/CB register emulation - Implement actual register handling logic
5. ✗ Context bank initialization - Program CBAR, TTBR, TCR, SCTLR for Stage-2
6. ✗ Stream mapping implementation - Configure SMR/S2CR hardware registers
7. ✗ TLB operation implementation - Proper IOVA range invalidation

**MC Integration:**
1. ✗ MC MMIO region mapping - Map as shared memory with EL2
2. ✗ Stage-2 trap configuration - Configure page tables to trap MC accesses
3. ✗ Complete SID validation logic - Wire up to actual hardware writes

**EL1 Stub Driver:**
1. ✗ Hypercall interface - Define actual hypercall numbers and ABI
2. ✗ Memory donation - Calculate required memory size and donate to EL2
3. ✗ Stream ID parsing - Extract SIDs from device tree
4. ✗ IOMMU device registration - Integrate with Linux IOMMU framework

**Integration:**
1. ✗ pKVM hypercall wiring - Connect to `__pkvm_host_iommu_*` handlers
2. ✗ Page table operations - Wire up io-pgtable-arm at EL2
3. ✗ Stage-2 page table management - Manage hypervisor page tables for SMMU MMIO trapping

#### File Locations

```
drivers/iommu/arm/arm-smmu/
├── arm-smmu-kvm.c              # EL1 stub driver (407 lines)
├── Makefile                     # Updated with pkvm/ subdir
└── pkvm/
    ├── Makefile                 # Builds arm-smmu-v2.o and tegra234-mc.o
    ├── arm-smmu-v2.h            # All data structures (280 lines)
    ├── arm-smmu-v2.c            # EL2 core driver with Phase 1 complete (845 lines)
    └── tegra234-mc.c            # MC integration skeleton (316 lines)

drivers/iommu/arm/Kconfig        # Added ARM_SMMU_V2_PKVM option
```

**Total Lines**: 1,848 lines (845 working implementation + 1,003 skeleton code)

#### Next Steps

Proceed with **Phase 2: Context Bank Management - Stage-2 Translation**:

1. Implement `smmu_v2_init_context_bank()` - configure CBAR for Stage-2-only translation
2. Program VTCR via TCR2 register for Stage-2 translation control
3. Write TTBR0 with Stage-2 page table base address
4. Enable translation by setting SCTLR.M bit

After Phase 2, continue with:
- **Phase 3**: TLB Operations (proper invalidation with timeouts)
- **Phase 4**: MMIO Emulation and Shadow State
- **Phase 5**: Stream Mapping

See **Implementation Phases** section below for detailed breakdown.

---

### SMMUv3 pKVM Reference Implementation Analysis

**Date**: 2025-10-29

A comprehensive study of the existing SMMUv3 pKVM implementation was conducted to understand practical patterns and infrastructure for implementing SMMUv2 support.

#### File Structure

**SMMUv3 pKVM Implementation** (`drivers/iommu/arm/arm-smmu-v3/`):
```
arm-smmu-v3/
├── arm-smmu-v3.c               # EL1 host driver (full SMMUv3, ~97KB)
├── arm-smmu-v3-kvm.c           # EL1 stub for pKVM (206 lines)
└── pkvm/
    ├── Kbuild                  # EL2 build configuration
    ├── arm-smmu-v3.c           # EL2 hypervisor driver (1095 lines)
    ├── arm_smmu_v3.h           # EL2 data structures (72 lines)
    ├── arm-smmu-v3-module.h    # Module integration (44 lines)
    └── io-pgtable-arm-hyp.c    # Page table allocator (67 lines) ← REUSABLE
```

**Key Insight**: `io-pgtable-arm-hyp.c` can be copied as-is for SMMUv2 implementation.

#### Critical Discovery: All Infrastructure Exists

**Hypercalls Pre-Registered** (`arch/arm64/kvm/hyp/nvhe/hyp-main.c:1836-1842`):

| Hypercall | Parameters | Purpose |
|-----------|-----------|---------|
| `__pkvm_host_iommu_alloc_domain` | iommu_id, domain_id, type | Domain creation |
| `__pkvm_host_iommu_free_domain` | domain_id | Domain deletion |
| `__pkvm_host_iommu_attach_dev` | iommu_id, domain_id, endpoint, pasid, pasid_bits, flags | Device attachment |
| `__pkvm_host_iommu_detach_dev` | iommu_id, domain_id, endpoint, pasid | Device detachment |
| `__pkvm_host_iommu_map_pages` | domain_id, iova, paddr, pgsize, pgcount, prot | Page mapping |
| `__pkvm_host_iommu_unmap_pages` | domain_id, iova, pgsize, pgcount | Page unmapping |
| `__pkvm_host_iommu_iova_to_phys` | domain_id, iova | Address translation |

**No kernel modifications needed** - drivers just implement callback operations.

#### MMIO Trapping Mechanism

**Trap Entry Flow** (`arch/arm64/kvm/hyp/nvhe/mem_protect.c:846-848`):

```
Host writes SMMU register
    ↓ [Stage-2 page fault]
handle_host_mem_abort()
    ↓ [Check if IOMMU address]
kvm_iommu_host_dabt_handler() → driver->dabt_handler()
    ↓ [Extract ESR fields]
smmu_dabt_handler(regs, esr, addr)
    ↓ [Emulate register access]
    - Read: regs->regs[rd] = readl(base + offset) & mask
    - Write: writel(regs->regs[rd] & mask, base + offset)
    ↓ [Return true if handled]
kvm_skip_host_instr() → regs->pc += 4
```

**Key Pattern**: Driver extracts instruction details from ESR (write/read, register, size), performs emulation, returns true. Framework automatically advances PC.

**ESR Fields Used**:
- `ESR_ELx_WNR`: Write/read bit
- `ESR_ELx_SRT_MASK`: Source/destination register (X0-X30)
- `ESR_ELx_SAS`: Access size (byte/half/word/double)

**Example Emulation** (`pkvm/arm-smmu-v3.c:827-987`):
```c
static bool smmu_dabt_device(struct hyp_arm_smmu_v3_device *smmu,
                             struct user_pt_regs *regs, u64 esr, u32 offset)
{
    bool is_write = esr & ESR_ELx_WNR;
    int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
    unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
    u64 val = regs->regs[rd];
    u64 mask;

    switch (offset) {
    case ARM_SMMU_IDR0:
        // Hide Stage-2 and MSI capabilities from host
        mask = IDR0_IMPLEMENTED & ~(IDR0_S2P | IDR0_VMID16 | IDR0_MSI | IDR0_HYP);
        break;
    case ARM_SMMU_CR0:
        if (is_write) {
            smmu->cr0 = val;
            if (val & CR0_SMMUEN)
                smmu_emulate_enable(smmu);  // Share stream table
        }
        mask = CR0_WRITABLE;
        break;
    }

    // Execute masked access
    if (is_write)
        writel_relaxed(val & mask, smmu->base + offset);
    else
        regs->regs[rd] = readl_relaxed(smmu->base + offset) & mask;

    return true;
}
```

#### Memory Donation Patterns

**Three Memory Sharing Modes**:

**1. DONATE (Exclusive Transfer)**

Host → EL2 (ownership transfer):
```c
// EL1: Allocate and donate
void *cmdq = __get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
phys_addr_t phys = virt_to_phys(cmdq);
__pkvm_host_donate_hyp(phys >> PAGE_SHIFT, 1 << order);
// Host can no longer access this memory!
```

EL2 → Host (cleanup):
```c
// EL2: Return ownership
__pkvm_hyp_donate_host(phys >> PAGE_SHIFT, size >> PAGE_SHIFT);
```

**Used For**:
- Command queues (SMMUv3)
- Stream tables (SMMUv3)
- Shadow SMR/S2CR arrays (SMMUv2)
- Context bank state structures (SMMUv2)

**Access Pattern**:
```c
// EL2 maps donated memory via kern_hyp_va
smmu->cmdq.base = hyp_phys_to_virt(phys);
```

**2. SHARE (Concurrent Read Access)**

```c
// EL2: Share host memory (read-only validation)
static int smmu_share_pages(phys_addr_t addr, size_t size)
{
    size_t nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

    // Mark pages as shared in stage-2
    for (i = 0; i < nr_pages; i++)
        __pkvm_host_share_hyp((addr + i * PAGE_SIZE) >> PAGE_SHIFT);

    // Pin in EL2 page tables
    return hyp_pin_shared_mem(hyp_phys_to_virt(addr),
                               hyp_phys_to_virt(addr + size));
}
```

**Used For**:
- Host stream table entries (SMMUv3 validates but doesn't modify)
- Potentially MC client configuration (SMMUv2)

**Key**: Host can read/write, EL2 can only read. EL2 validates writes via trapping.

**3. IDMAP (Identity Map in Host Stage-2)**

```c
// Map MMIO region as device memory
kvm_iommu_host_stage2_idmap(start, end, PKVM_HOST_MMIO_PROT);
```

**Protection Flags**:
- `KVM_PGTABLE_PROT_R` → `IOMMU_READ`
- `KVM_PGTABLE_PROT_W` → `IOMMU_WRITE`
- `PKVM_HOST_MMIO_PROT` → `IOMMU_MMIO`

**Note**: SMMUv3 does NOT use IDMAP - it donates entire MMIO region and traps all accesses. **Recommend same for SMMUv2**.

#### Page Table Integration (io-pgtable-arm)

**Allocator Wrapper** (`pkvm/io-pgtable-arm-hyp.c` - 67 lines):

```c
void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
                              struct io_pgtable_cfg *cfg, void *cookie)
{
    void *addr = kvm_iommu_donate_pages_atomic(get_order(size));

    if (addr && !cfg->coherent_walk)
        kvm_flush_dcache_to_poc(addr, size);  // Non-coherent cache flush

    return addr;
}

struct io_pgtable_ops *kvm_alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
                                                 struct io_pgtable_cfg *cfg,
                                                 void *cookie)
{
    struct io_pgtable *iop;

    if (fmt != ARM_64_LPAE_S2)
        return NULL;

    iop = arm_64_lpae_alloc_pgtable_s2(cfg, cookie);
    iop->cfg = *cfg;
    return &iop->ops;
}
```

**✅ Reusable as-is for SMMUv2** - just copy the file.

**TLB Callback Interface** (`pkvm/arm-smmu-v3.c:284-300`):

```c
static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
                                 size_t granule, void *cookie)
{
    // Called when unmapping non-leaf PTEs
    smmu_tlb_inv_range(iova, size, granule, false);
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
                               unsigned long iova, size_t granule, void *cookie)
{
    // Called when unmapping leaf PTEs
    smmu_tlb_inv_range(iova, granule, granule, true);
}

static const struct iommu_flush_ops smmu_tlb_ops = {
    .tlb_flush_walk = smmu_tlb_flush_walk,
    .tlb_add_page   = smmu_tlb_add_page,
};
```

**SMMUv2 Adaptation** - Use register-based TLB invalidation:

```c
static void smmu_v2_tlb_flush_walk(unsigned long iova, size_t size,
                                    size_t granule, void *cookie)
{
    for_each_smmu(smmu) {
        // Global invalidate via TLBIVMID
        writel_relaxed(VMID, smmu->base + ARM_SMMU_GR0_TLBIVMID);
        smmu_v2_tlb_sync_global(smmu);
    }
}
```

**Configuration for Tegra234**:

```c
struct io_pgtable_cfg cfg = {
    .tlb = &smmu_v2_tlb_ops,
    .pgsize_bitmap = SZ_4K,    // Tegra234: 4K only (walk cache erratum)
    .ias = 48,
    .oas = 48,
    .coherent_walk = true,
};

ops = kvm_alloc_io_pgtable_ops(ARM_64_LPAE_S2, &cfg, NULL);
```

#### Generic pKVM IOMMU Framework

**Domain Management** (`arch/arm64/kvm/hyp/nvhe/iommu/iommu.c:224-306`):

The framework provides:
- Domain handle management
- Reference counting
- Memory cache requests (via X2-X3 return values)
- Driver callback dispatch

**What Framework Handles**:
```c
int kvm_iommu_alloc_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id, int type)
{
    struct kvm_hyp_iommu_domain *domain = handle_to_domain(domain_id);

    // 1. Allocate domain structure from pool
    // 2. Call driver-specific allocator
    ret = kvm_iommu_ops->alloc_domain(iommu_id, domain, type);
    // 3. Set domain ID and initialize refcount

    return ret;
}
```

**What Driver Must Implement**:
```c
static int smmu_v2_alloc_domain(pkvm_handle_t iommu_id,
                                 struct kvm_hyp_iommu_domain *domain,
                                 int type)
{
    // Allocate driver-private state
    // Allocate page tables via kvm_alloc_io_pgtable_ops()
    // Store in domain->priv
    return 0;
}
```

**Memory Cache Protocol** - If driver returns `-ENOMEM`, framework:
1. Encodes memory request in X2-X3
2. Host refills memory pool via `__pkvm_topup_hyp_alloc_mgt_gfp()`
3. Host retries hypercall

#### What SMMUv3 Currently Implements

**✅ Implemented** (`pkvm/arm-smmu-v3.c`):
- `init` (lines 659-688): Hardware initialization, page table allocation
- `dabt_handler` (lines 989-1003): MMIO trap and register emulation
- `host_stage2_idmap` (lines 1022-1077): Identity-map host memory regions

**❌ Missing** (needs driver implementation):
- `alloc_domain`: Create IOMMU domain with page tables
- `free_domain`: Release domain resources
- `attach_dev`: Map Stream ID to domain (configure STE/SMR+S2CR)
- `detach_dev`: Unmap Stream ID
- `map_pages`: Map IOVA → PA in page tables
- `unmap_pages`: Unmap IOVA range
- `iova_to_phys`: Translate IOVA to physical address
- `iotlb_sync`: Synchronize TLB invalidations

**Note**: These operations are needed for **both SMMUv3 and SMMUv2** - not yet implemented in upstream pKVM.

#### SMMUv3 vs SMMUv2 Complexity Comparison

| Component | SMMUv3 Lines | SMMUv2 Estimate | Difference |
|-----------|--------------|-----------------|------------|
| **Command queue** | ~350 | 0 | **-350 (removed)** |
| **Stream table L1/L2** | ~300 | 0 | **-300 (replaced by SMR/S2CR)** |
| **Register emulation** | ~400 | ~300 | **-100 (simpler layout)** |
| **Context management** | 0 | ~200 | **+200 (CB allocation)** |
| **TLB operations** | ~150 | ~100 | **-50 (simpler)** |
| **Tegra234 dual-base** | 0 | ~100 | **+100 (new)** |
| **MC integration** | 0 | ~200 | **+200 (new)** |
| **Page table ops** | ~100 | ~100 | **0 (same)** |
| **Total** | **1095** | **~800** | **-295 (-27%)** |

**Key Insight**: SMMUv2 is **simpler** than SMMUv3 in core architecture, offset by Tegra234-specific features.

#### Recommended Implementation Order

Based on SMMUv3 patterns:

**Phase 1: Infrastructure (Week 1-2)**
1. Copy `io-pgtable-arm-hyp.c` to `pkvm/` directory
2. Implement `smmu_v2_init()`:
   - Hardware probe (read IDR registers)
   - Allocate page table pool via `kvm_alloc_io_pgtable_ops()`
   - Register with pKVM framework
3. Implement `smmu_v2_dabt_handler()` skeleton:
   - Find SMMU by MMIO address
   - Dispatch to register page handler (GR0/GR1/CB)

**Phase 2: Register Emulation (Week 3)**
1. GR0 handlers:
   - IDR registers (mask capabilities)
   - sCR0 (SMMU enable/disable)
   - SMR registers (shadow state)
   - S2CR registers (shadow state)
   - TLBGSYNC/TLBGSTATUS
2. GR1 handlers:
   - CBAR (context bank attributes)
   - CBA2R (VMID assignment)
3. CB handlers:
   - SCTLR (enable/disable translation)
   - TCR2/VTCR (translation control)
   - TTBR0 (page table base - shadow only, use EL2's tables)
   - MAIR (memory attributes)

**Phase 3: Device Lifecycle (Week 4-5)**
1. `smmu_v2_alloc_domain()`:
   - Allocate domain state structure
   - Create page tables via `kvm_alloc_io_pgtable_ops()`
2. `smmu_v2_attach_dev()`:
   - Allocate context bank
   - Configure SMR (stream match)
   - Configure S2CR (stream-to-context)
   - Initialize context bank (CBAR, VTCR, TTBR0, SCTLR)
   - Record SID assignment for MC validation
3. `smmu_v2_detach_dev()`:
   - Clear SMR and S2CR
   - Invalidate TLB
   - Free context bank
4. `smmu_v2_free_domain()`:
   - Release page tables
   - Free domain state

**Phase 4: Page Operations (Week 6)**
1. `smmu_v2_map_pages()` - Call `ops->map()`
2. `smmu_v2_unmap_pages()` - Call `ops->unmap()`
3. `smmu_v2_iova_to_phys()` - Call `ops->iova_to_phys()`
4. `smmu_v2_iotlb_sync()` - Register-based TLB invalidation

**Phase 5: MC Integration (Week 7)**
1. MC MMIO trap setup (unmap SID override region from host stage-2)
2. MC DABT handler implementation
3. SID validation against `sid_map[]`

**Phase 6: Tegra234-Specific (Week 8)**
1. Dual register base support (mirror writes to secondary base)
2. Walk cache erratum (force 4K pages)
3. Multi-instance coordination

#### Key Takeaways

**✅ Excellent News**:
1. **All infrastructure exists** - No kernel modifications needed
2. **Clear patterns** - SMMUv3 provides proven implementation model
3. **Simpler hardware** - SMMUv2 has less complexity than SMMUv3
4. **Reusable components** - Page table code works as-is

**⚠️ Implementation Required**:
1. Register emulation (following SMMUv3 pattern)
2. Device lifecycle operations (generic, not SMMUv3-specific)
3. MC integration (Tegra234-specific)
4. Dual-base coordination (Tegra234-specific)

**📊 Effort Estimate**: 7-9 weeks (reduced from initial 9-11 weeks due to infrastructure discovery)

**🎯 Critical Success Factor**: Follow SMMUv3 patterns - trap-and-emulate MMIO, DONATE shadow state, use generic framework for domain management.

---

### Complete SMMUv3 Implementation (pkvm-smmu-v4 Branch)

**Date**: 2025-10-29 (Updated after discovering v4 branch)

**Critical Discovery**: The `pkvm-smmu-v4` branch contains **complete, production-ready implementations** of all domain operations that were marked as "missing" in the initial analysis. This changes our understanding significantly.

**Branch**: `pkvm-smmu-v4` in `/home/hlyytine/pkvm/Linux_for_Tegra/source/kernel/linux`

#### Revolutionary Architecture: Global Identity Mapping

**Key Architectural Insight**: SMMUv3 pKVM does NOT use per-domain page tables!

Instead, it maintains **one global Stage-2 page table** that shadows the host's CPU stage-2 page table:

```c
// drivers/iommu/arm/arm-smmu-v3/pkvm/arm-smmu-v3.c:63
static struct io_pgtable *idmap_pgtable;  // Global for ALL SMMUs and domains
```

**Why This Works**:
- All devices share the same IPA→PA mapping (identity-mapped)
- Host stage-2 already defines the memory protection view
- Single VMID (0) for all devices
- Dramatically simplifies implementation (~1000 lines vs potentially 3000+)

#### "Domain Operations" Are Actually Stage-2 Operations

**There is no `alloc_domain` or `free_domain` in the traditional sense!**

Instead:

**1. Host Stage-2 Snapshot** (`arch/arm64/kvm/hyp/nvhe/iommu/iommu.c:62-78`):
```c
static int kvm_iommu_snapshot_host_stage2(void)
{
    struct kvm_pgtable_walker walker = {
        .cb = __snapshot_host_stage2,
        .flags = KVM_PGTABLE_WALK_LEAF,
    };

    // Walk entire host stage-2 page table
    ret = kvm_pgtable_walk(&host_mmu.pgt, 0, BIT(pgt->ia_bits), &walker);
    return ret;
}
```

**2. Per-Leaf Callback** (lines 40-60):
```c
static int __snapshot_host_stage2(const struct kvm_pgtable_visit_ctx *ctx, ...)
{
    u64 start = ctx->addr;
    u64 end = start + kvm_granule_size(ctx->level);
    int prot = pkvm_to_iommu_prot(kvm_pgtable_stage2_pte_prot(*ctx->ptep));

    // Map this range in IOMMU page table (identity mapping)
    kvm_iommu_ops->host_stage2_idmap(start, end, prot);
    return 0;
}
```

**3. IOMMU Mapping** (`drivers/iommu/arm/arm-smmu-v3/pkvm/arm-smmu-v3.c:971-1029`):
```c
static void smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
    if (prot) {  // Map
        if (!(prot & IOMMU_MMIO))
            prot |= IOMMU_CACHE;

        // Memory: always PAGE_SIZE (avoid block splits)
        // MMIO: largest blocks (2MB/1GB)
        ret = pgtable->ops.map_pages(&pgtable->ops, start, start,
                                     pgsize, pgcount, prot, 0, &mapped);
    } else {  // Unmap
        unmapped = pgtable->ops.unmap_pages(&pgtable->ops, start,
                                            pgsize, pgcount, NULL);
    }
}
```

**Result**: The SMMU S2 page table becomes an identity-mapped copy of host stage-2, protecting the same memory.

#### Stream Table Entry (STE) Shadowing

**Two-Level Shadow Architecture**:

```
┌────────────────────────┐
│  Host Stream Table     │ ← Shared (host writes, hyp reads)
│  (managed by Linux)    │
└────────────────────────┘
         │
         ├─ STE[0]: bypass → Hyp modifies to S2
         ├─ STE[1]: S1     → Hyp modifies to nested
         └─ ...

┌────────────────────────┐
│  Hyp Stream Table      │ ← Donated (owned by hypervisor)
│  (actual hardware)     │
└────────────────────────┘
         │
         ├─ STE[0]: S2 translation enabled
         ├─ STE[1]: Nested (S1+S2)
         └─ ...
```

**STE Transformation** (`pkvm/arm-smmu-v3.c:339-377`):

| Host Config | Hypervisor Config | Effect |
|-------------|-------------------|--------|
| Bypass (0b000) | S2 (0b010) | Add S2 translation |
| S1 (0b100) | Nested (0b110) | Add S2 to host's S1 |
| Abort (0b001) | Abort (0b001) | Keep aborted |

**Critical Code** (lines 430-482):
```c
static void smmu_reshadow_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid, bool leaf)
{
    // 1. Copy host's STE
    memcpy(target.data, host_ste_ptr->data, STRTAB_STE_DWORDS << 3);

    // 2. Modify: inject S2 translation (VTTBR, VTCR from idmap_pgtable)
    smmu_attach_stage_2(smmu, &target);

    // 3. Write using atomic update sequence:
    //    - Write dwords 1-7 (STE invalid)
    //    - CFGI_STE (flush cached copy)
    //    - Write dword 0 (make valid)
    WRITE_ONCE(hyp_ste_ptr->data[0], target.data[0]);
}
```

**Trigger**: Host writes `CFGI_STE` command → Hypervisor intercepts → Reshadows STE

#### Page Mapping Operations (Fully Implemented!)

**Map Pages** (line 1007):
```c
ret = pgtable->ops.map_pages(&pgtable->ops, start, start,
                             pgsize, pgcount, prot, 0, &mapped);
```

**Unmap Pages** (line 1019):
```c
unmapped = pgtable->ops.unmap_pages(&pgtable->ops, start,
                                    pgsize, pgcount, NULL);
```

**Both operations use `io-pgtable-arm.c` with custom allocator:**
```c
// pkvm/io-pgtable-arm-hyp.c:26-36
void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
                              struct io_pgtable_cfg *cfg, void *cookie)
{
    void *addr = kvm_iommu_donate_pages_atomic(get_order(size));

    if (addr && !cfg->coherent_walk)
        kvm_flush_dcache_to_poc(addr, size);

    return addr;
}
```

**TLB Invalidation** (lines 200-242):
```c
static void smmu_tlb_inv_range(unsigned long iova, size_t size,
                               size_t granule, bool leaf)
{
    // Invalidate on ALL SMMUs (global identity map)
    for_each_smmu(smmu) {
        // S2 TLB invalidation (IPA range)
        smmu_tlb_inv_range_smmu(smmu, &cmd_s2, iova, size, granule);

        // S1 TLB invalidation (all ASIDs with VMID=0)
        smmu_send_cmd(smmu, &cmd_s1);
    }
}
```

**Why S1 AND S2?**
- Host uses nested translation (S1+S2)
- Changing S2 affects all S1 translations
- Must flush both to maintain coherency

#### Command Queue Emulation (Lines 695-725)

**Host writes CMDQ_PROD → Hypervisor processes commands:**

```c
static void smmu_emulate_cmdq_insert(struct hyp_arm_smmu_v3_device *smmu)
{
    u64 *host_cmdq = hyp_phys_to_virt(smmu->cmdq_host.q_base);

    while (!queue_empty(&smmu->cmdq_host.llq)) {
        // Wait for space in hardware queue
        WARN_ON(smmu_wait_event(smmu, !smmu_cmdq_full(&smmu->cmdq)));

        // Read command from host (TOCTOU-safe copy)
        memcpy(cmd, &host_cmdq[idx * CMDQ_ENT_DWORDS], ...);

        // Filter and modify
        skip = smmu_filter_command(smmu, cmd);

        if (!skip)
            smmu_add_cmd_raw(smmu, cmd);  // Add to real HW queue

        queue_inc_cons(&smmu->cmdq_host.llq);
    }
}
```

**Allowed Commands** (lines 640-693):
- `CFGI_STE` → Triggers `smmu_reshadow_ste()`
- `CFGI_ALL` → Global config invalidation
- `TLBI_NH_*` → Stage-1 TLB ops (VMID=0 only)
- `CMD_SYNC` → MSI→SEV conversion
- `PREFETCH_CFG` → Passthrough

**Blocked Commands**:
- `TLBI_S12_VMALL` → Would flush all VMIDs (security hole)
- `TLBI_EL2_*` → Hypervisor-only operations

#### Memory Donation Strategy

**Donated (exclusive ownership to EL2):**
```c
// SMMU MMIO pages
__pkvm_host_donate_hyp_mmio(pfn);

// SMMU array, shadow command queues, shadow stream tables
__pkvm_host_donate_hyp(phys >> PAGE_SHIFT, nr_pages);

// Page table pages (via io-pgtable allocator)
kvm_iommu_donate_pages_atomic(order);
```

**Shared (concurrent access):**
```c
// Host command queue and stream table (read-only by hypervisor)
__pkvm_host_share_hyp(phys >> PAGE_SHIFT);
hyp_pin_shared_mem(hyp_phys_to_virt(addr), hyp_phys_to_virt(addr + size));
```

#### Key Implementation Patterns for SMMUv2

**1. Single Global Page Table**
- ✅ Reuse exactly: One `io_pgtable` for all devices
- ✅ Same host stage-2 snapshot process
- ✅ Same memory donation model

**2. Shadow State Management**
- ✅ Pattern: Host structure + Hyp structure
- ⚠️ Different: SMR/S2CR registers instead of STEs
- ⚠️ Simpler: Single register write vs 64-byte structure

**3. MMIO Trap Handling**
- ✅ Same: ESR parsing, register emulation pattern
- ⚠️ Different: No CMDQ_PROD/CMDQ_CONS registers
- ⚠️ Added: TLB invalidation register traps

**4. No Command Queue in SMMUv2**
- ✅ Simplification: No command filtering logic (-350 lines)
- ✅ Simplification: No queue management
- ⚠️ Different: Direct TLB register writes

**5. Context Bank Management (SMMUv2-specific)**
- ⚠️ New: CB allocation bitmap
- ⚠️ New: Per-CB TTBR/TCR configuration
- ⚠️ New: CB exhaustion handling

#### Initialization Flow

**EL1 (arm-smmu-v3-kvm.c:129-158)**:
```c
core_initcall(kvm_arm_smmu_v3_register)
    ├─ Parse device tree
    ├─ Allocate SMMU array + shadow queues + shadow stream tables
    ├─ kvm_iommu_register_driver(&smmu_ops)
    └─ Store pointers in nVHE section
```

**EL2 (arm-smmu-v3.c:599-628)**:
```c
kvm_iommu_init()
    ├─ smmu_init()
    │   ├─ Take ownership of SMMU array
    │   ├─ For each SMMU:
    │   │   ├─ Donate MMIO pages
    │   │   ├─ smmu_probe() - Read IDR registers
    │   │   ├─ smmu_init_cmdq() - Donate shadow queue
    │   │   ├─ smmu_init_strtab() - Donate shadow stream table
    │   │   └─ smmu_abort_gbpa() - Set GBPA=ABORT
    │   └─ smmu_init_pgt() - Create global identity page table
    └─ kvm_iommu_snapshot_host_stage2() - Populate identity mappings
```

#### Critical Insights for SMMUv2

**What Makes This Architecture Brilliant**:
1. **No per-domain complexity** - Single global page table
2. **Automatic memory protection** - Mirrors host stage-2
3. **Simple lifecycle** - No domain alloc/free needed
4. **Efficient TLB management** - Single VMID for all devices

**Simplifications for SMMUv2**:
1. **No command queue** → ~350 lines removed
2. **Simpler stream mapping** → SMR+S2CR vs 64-byte STE
3. **Fewer registers** → ~20 global + ~15 per-CB vs 50+ SMMUv3

**Added Complexity for Tegra234**:
1. **MC integration** → ~200 lines for SID validation
2. **Dual register bases** → ~100 lines for niso0/niso1 coordination
3. **Context bank management** → ~200 lines for CB allocation

**Revised Estimate**:
- **Core SMMUv2 driver**: ~1000 lines (vs 1037 for SMMUv3)
- **MC integration**: ~250 lines
- **Total**: ~1250 lines

**Time Estimate**: 6-8 weeks (down from 7-9 weeks due to better understanding)

---

### Implementation Phases

#### Phase 1: Core SMMUv2 EL2 Driver (2-3 weeks)

**File**: `drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.c` (~800 lines, simplified from ~1000 with Stage-2-only)

**Components**:
1. **Probe and initialization** (~200 lines):
   - `smmu_v2_hyp_probe()` - read SMMU capabilities from IDR registers
   - `smmu_v2_hw_init()` - configure SMMU at EL2
   - Parse features (coherent walk, stage-2 support, etc.)

2. **MMIO emulation** (~300 lines):
   - `smmu_v2_mmio_handler()` - trap handler for host SMMU accesses
   - Emulate GR0/GR1 register reads/writes
   - Shadow SMR/S2CR programming
   - Trap context bank configuration

3. **Context Bank management** (~250 lines):
   - `smmu_v2_alloc_context_bank()` - allocate CB for domain
   - `smmu_v2_init_context_bank()` - configure CB with Stage-2 translation
   - Guest devices: Stage-2 only (IPA → PA translation by EL2)
   - Host devices: Stage-2 bypass or identity mapping
   - `smmu_v2_free_context_bank()` - release CB

4. **Stream mapping** (~200 lines):
   - `smmu_v2_map_stream()` - configure SMR + S2CR
   - Shadow host's stream mapping requests
   - Assign context banks based on domain type (guest vs host)

5. **TLB operations** (~150 lines):
   - `smmu_v2_tlb_inv_context()` - context-based invalidation
   - `smmu_v2_tlb_inv_range()` - IOVA range invalidation
   - `smmu_v2_tlb_sync()` - synchronize TLB operations

**Dependencies**: Reuse io-pgtable-arm for all page table operations (already EL2-ready).

#### Phase 2: Tegra234 MC Integration (1-2 weeks)

**File**: `drivers/iommu/arm/arm-smmu/pkvm/tegra234-mc.c` (~300 lines)

**Components**:
1. **MC MMIO trapping** (~100 lines):
   - `mc_mmio_trap_init()` - setup MC region trapping in stage-2
   - Parse MC device tree node for MMIO address/size
   - Map as shared memory with EL2
   - Configure stage-2 to trap all MC accesses

2. **SID override validation** (~100 lines):
   - `mc_sid_override_handler()` - validate SID writes
   - `mc_validate_sid()` - check against sid_map[]
   - Handle security register reads
   - Inject abort on invalid writes

3. **Client table** (~50 lines):
   - Static data from `drivers/memory/tegra/tegra234.c`
   - ~100 MC clients with register offsets
   - Helper functions to map offsets → client IDs

4. **Multi-instance support** (~50 lines):
   - Tegra234 has 3 SMMU instances: smmu_niso1 (0x8000000), smmu_niso0 (0x12000000), smmu_iso (0x10000000)
   - Some instances have dual register bases (niso0 and niso1 have secondary bases)
   - Mirror logic from `arm-smmu-nvidia.c` for dual-base coordination
   - Coordinate writes across register bases within the same instance

**Tegra234-Specific Handling**:
- Walk cache erratum workaround (disable large pages)
- Multi-instance SMMU coordination
- MC SID override security checks

#### Phase 3: Host Stub Driver (1 week)

**File**: `drivers/iommu/arm/arm-smmu/arm-smmu-kvm.c` (~250 lines)

**Purpose**: Minimal EL1 driver that coordinates with EL2

```c
static int arm_smmu_kvm_probe(struct platform_device *pdev)
{
    // Parse DT: MMIO regions, interrupts, features
    // Allocate SMR/S2CR shadow arrays
    // Allocate context bank structures
    // Donate memory to EL2
    // Register via kvm_iommu_init() hypercall
}

static struct kvm_iommu_ops arm_smmu_v2_ops = {
    .init = smmu_v2_host_init,
    .attach_dev = smmu_v2_attach_dev,
    .detach_dev = smmu_v2_detach_dev,
    .map_pages = smmu_v2_map_pages,
    .unmap_pages = smmu_v2_unmap_pages,
    .iova_to_phys = smmu_v2_iova_to_phys,
    .iotlb_sync = smmu_v2_iotlb_sync,
};
```

All operations map to hypercalls to EL2.

#### Phase 4: Build Integration (3 days)

**Kconfig**:
```kconfig
config ARM_SMMU_V2_PKVM
    bool "pKVM support for ARM SMMUv2 (Tegra234)"
    depends on KVM && ARM_SMMU && ARCH_TEGRA_234_SOC
    select IOMMU_IO_PGTABLE_LPAE
    help
      Enable pKVM IOMMU virtualization for Tegra234 SMMUv2.
      Allows DMA isolation for protected VMs on NVIDIA Orin AGX.
```

**Makefile**:
```makefile
# drivers/iommu/arm/arm-smmu/Makefile
obj-$(CONFIG_ARM_SMMU_V2_PKVM) += arm-smmu-kvm.o
obj-$(CONFIG_ARM_SMMU_V2_PKVM) += pkvm/
```

### Testing Strategy

#### Unit Tests
- Shadow state consistency (SMR/S2CR)
- MMIO emulation correctness
- TLB operation sequencing
- MC SID validation logic

#### Integration Tests
- Device assignment to guest VMs
- DMA isolation verification
- MC SID override interception
- Multi-instance SMMU coordination

#### Platform-Specific Tests (Orin AGX)
- GPU passthrough validation
- Display with host, GPU with guest
- Tegra234 erratum workaround verification
- Resume path (MC re-programs SIDs)

### Timeline

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| Phase 1: Core SMMUv2 EL2 | 3-4 weeks | Working EL2 driver |
| Phase 2: MC integration | 1-2 weeks | SID validation, trapping |
| Phase 3: Host stub | 1 week | EL1 driver |
| Phase 4: Build integration | 3 days | Kconfig, Makefiles |
| Testing | 2 weeks | Validated on Orin |
| **Total** | **7-9 weeks** | Production-ready |

### Key Technical Decisions

1. **No SMMUv2/v3 code consolidation**: SMMUv3 support is evolving (4th patchset), keep implementations separate
2. **MC MMIO trapping over hypercalls**: Cleaner, handles resume path automatically, no MC driver modifications needed
3. **Dynamic SID assignment**: No static policy files, all validation is "use what you were assigned"
4. **Reuse io-pgtable-arm**: Already factored out for EL2 by SMMUv3 work, proven solution
5. **Tegra234 only**: No support for other SoCs (Tegra186, Tegra194, etc.)

### Important Notes

- **MC driver stays at EL1**: No changes needed, operates normally except SID writes are trapped
- **Resume path**: MC's `tegra186_mc_resume()` re-programs all SIDs, these writes are trapped and validated by EL2
- **Security model**: EL2 is sole authority for Stream ID assignments, host cannot bypass
- **Hypervisor MMIO**: MC MMIO region shared between EL1 and EL2 (host can read, writes are trapped)

### References

- `drivers/iommu/arm/arm-smmu-v3/pkvm/` - SMMUv3 reference implementation
- `drivers/iommu/arm/arm-smmu/arm-smmu-nvidia.c` - Tegra multi-instance logic
- `drivers/memory/tegra/tegra234.c` - MC client definitions
- `drivers/memory/tegra/tegra186.c` - MC SID override programming
- `arch/arm64/kvm/hyp/include/nvhe/iommu.h` - pKVM IOMMU interface

### Detailed Implementation Design

This section provides register-level implementation details for the SMMUv2 pKVM driver. For high-level overview, see the sections above.

#### 1. Hypercall Interface and Memory Donation Flow

**Hypercall ABI**

The system reuses existing pKVM IOMMU hypercalls defined in `arch/arm64/kvm/hyp/nvhe/hyp-main.c`:

```c
// Domain management
__pkvm_host_iommu_alloc_domain(iommu_id, domain_id, type)
__pkvm_host_iommu_free_domain(domain_id)

// Device attachment
__pkvm_host_iommu_attach_dev(iommu_id, domain_id, endpoint_id, pasid, pasid_bits, flags)
__pkvm_host_iommu_detach_dev(iommu_id, domain_id, endpoint_id, pasid)

// Page table operations
__pkvm_host_iommu_map_pages(domain_id, iova, paddr, pgsize, pgcount, prot, *mapped)
__pkvm_host_iommu_unmap_pages(domain_id, iova, pgsize, pgcount)
__pkvm_host_iommu_iova_to_phys(domain_id, iova)
```

**Parameter Semantics for SMMUv2:**
- `iommu_id`: Index into `kvm_hyp_arm_smmu_v2_smmus[]` array (0, 1, or 2 for Tegra234's 3 SMMU instances: smmu_niso1, smmu_niso0, smmu_iso)
- `endpoint_id`: Stream ID (0-255)
- `pasid`: Not used for SMMUv2 (always 0)
- `domain_id`: Handle to `struct kvm_hyp_iommu_domain`

**Memory Donation Types**

Three types of memory sharing are used:

**A. DONATE (exclusive ownership transfer to EL2):**
```c
// At EL1: Host donates SMR/S2CR shadow arrays
__pkvm_host_donate_hyp(phys >> PAGE_SHIFT, size >> PAGE_SHIFT);

// At EL2: Reclaim back to host on cleanup
__pkvm_hyp_donate_host(phys >> PAGE_SHIFT, size >> PAGE_SHIFT);
```

**B. SHARE (concurrent access, EL2 validates host writes):**
```c
// At EL1: Share MC MMIO region with EL2
for (i = 0; i < nr_pages; i++)
    __pkvm_host_share_hyp((addr + i * PAGE_SIZE) >> PAGE_SHIFT);
hyp_pin_shared_mem(hyp_va_start, hyp_va_end);  // Pin in EL2 page tables

// At EL2: Unshare on cleanup
hyp_unpin_shared_mem(hyp_va_start, hyp_va_end);
for (i = 0; i < nr_pages; i++)
    __pkvm_host_unshare_hyp((addr + i * PAGE_SIZE) >> PAGE_SHIFT);
```

**C. IDMAP (identity-map SMMU MMIO into host stage-2):**
```c
// At EL2: Allow host to access SMMU MMIO (for emulation)
kvm_iommu_host_stage2_idmap(smmu_mmio_base, smmu_mmio_base + size,
                             KVM_PGTABLE_PROT_RW | KVM_PGTABLE_PROT_DEVICE);
```

**Initialization Sequence:**

```c
// EL1: drivers/iommu/arm/arm-smmu/arm-smmu-kvm.c
static int arm_smmu_v2_kvm_probe(struct platform_device *pdev)
{
    // 1. Parse device tree (MMIO base, interrupts, num_mapping_groups)
    struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    smmu->mmio_addr = res->start;
    smmu->mmio_size = resource_size(res);

    // 2. Allocate shadow structures (SMR/S2CR arrays, context bank metadata)
    size_t smr_size = num_smrgs * sizeof(struct arm_smmu_smr);
    smmu->smrs = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                          get_order(smr_size));

    size_t s2cr_size = num_smrgs * sizeof(struct arm_smmu_s2cr);
    smmu->s2crs = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                           get_order(s2cr_size));

    // 3. Donate shadow arrays to EL2
    __pkvm_host_donate_hyp(virt_to_phys(smmu->smrs) >> PAGE_SHIFT,
                           PAGE_ALIGN(smr_size) >> PAGE_SHIFT);
    __pkvm_host_donate_hyp(virt_to_phys(smmu->s2crs) >> PAGE_SHIFT,
                           PAGE_ALIGN(s2cr_size) >> PAGE_SHIFT);

    // 4. Share MC MMIO region (for SID override trapping)
    struct resource *mc_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mc");
    mc_mmio_phys = mc_res->start;
    mc_mmio_size = resource_size(mc_res);

    for (i = 0; i < (mc_mmio_size >> PAGE_SHIFT); i++)
        __pkvm_host_share_hyp((mc_mmio_phys + i * PAGE_SIZE) >> PAGE_SHIFT);

    // 5. Register with EL2 (passes addresses to hypervisor)
    return kvm_call_hyp_nvhe(__pkvm_iommu_init, hyp_ops);  // Triggers smmu_v2_init() at EL2
}
```

**Memory Layout After Initialization:**

```
┌─────────────────────────────────────────────────────────┐
│ SMMU_NISO1 MMIO (0x8000000, 16MB)                       │
│  - Primary base for smmu_niso1 (HOST1X, VIC, NVDEC)    │
│  - Owned by EL2                                         │
│  - Identity-mapped in host S2 for emulation             │
│  - Direct writes from host trap to EL2                  │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ SMMU_NISO1 Secondary (0x7000000, 16MB)                  │
│  - Secondary register base for smmu_niso1               │
│  - Some registers mirrored to primary                   │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ SMMU_ISO MMIO (0x10000000, 16MB)                        │
│  - Isolated SMMU for secure display (ISO_NVDISPLAY)    │
│  - Owned by EL2                                         │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ SMMU_NISO0 MMIO (0x12000000, 16MB)                      │
│  - Primary base for smmu_niso0 (NVDISPLAY, NVENC, OFA) │
│  - Owned by EL2                                         │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ SMMU_NISO0 Secondary (0x11000000, 16MB)                 │
│  - Secondary register base for smmu_niso0               │
│  - Some registers mirrored to primary                   │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ MC MMIO (0x2c00000, 1MB)                                │
│  - Owned by host (for bandwidth mgmt)                   │
│  - Shared with EL2 (concurrent access)                  │
│  - SID override writes trap to EL2                      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ SMR/S2CR Shadow Arrays (per SMMU instance)              │
│  - Donated to EL2 (exclusive ownership)                 │
│  - Host cannot access after donation                    │
│  - One set per SMMU instance (3 total)                  │
└─────────────────────────────────────────────────────────┘
```

#### 2. SMMUv2 Hardware Initialization Sequence

**EL2 Probe and Feature Detection**

```c
// drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.c
static int smmu_v2_probe_device(struct hyp_arm_smmu_v2_device *smmu)
{
    void __iomem *gr0_base = smmu->base;
    u32 id0, id1, id2;

    // 1. Read ID registers (capabilities)
    id0 = readl_relaxed(gr0_base + ARM_SMMU_GR0_ID0);
    id1 = readl_relaxed(gr0_base + ARM_SMMU_GR0_ID1);
    id2 = readl_relaxed(gr0_base + ARM_SMMU_GR0_ID2);

    // 2. Extract capabilities
    smmu->num_mapping_groups = FIELD_GET(ARM_SMMU_ID0_NUMSMRG, id0) + 1;
    smmu->num_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMCB, id1) + 1;
    smmu->num_s2_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMS2CB, id1) + 1;

    if (id0 & ARM_SMMU_ID0_S1TS)
        smmu->features |= ARM_SMMU_FEAT_TRANS_S1;
    if (id0 & ARM_SMMU_ID0_S2TS)
        smmu->features |= ARM_SMMU_FEAT_TRANS_S2;
    if (id0 & ARM_SMMU_ID0_NTS)
        smmu->features |= ARM_SMMU_FEAT_TRANS_NESTED;
    if (id0 & ARM_SMMU_ID0_CTTW)
        smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;

    // 3. Determine page size support
    if (id2 & ARM_SMMU_ID2_PTFS_4K)
        smmu->pgsize_bitmap |= SZ_4K;
    if (id2 & ARM_SMMU_ID2_PTFS_16K)
        smmu->pgsize_bitmap |= SZ_16K;
    if (id2 & ARM_SMMU_ID2_PTFS_64K)
        smmu->pgsize_bitmap |= SZ_64K;

    // Tegra234 erratum: disable large pages (walk cache bug)
    smmu->pgsize_bitmap = SZ_4K;

    // 4. Extract address sizes
    smmu->ias = FIELD_GET(ARM_SMMU_ID2_IAS, id2);  // Input address bits
    smmu->oas = FIELD_GET(ARM_SMMU_ID2_OAS, id2);  // Output address bits

    return 0;
}
```

**Hardware Reset and Configuration**

```c
static int smmu_v2_reset(struct hyp_arm_smmu_v2_device *smmu)
{
    void __iomem *gr0_base = smmu->base;
    u32 scr0, sacr;
    int ret;

    // 1. Disable SMMU globally (clear CLIENTPD, set all streams to fault)
    scr0 = ARM_SMMU_sCR0_CLIENTPD;
    writel_relaxed(scr0, gr0_base + ARM_SMMU_GR0_sCR0);

    // 2. Wait for Global TLB sync (ensure all in-flight transactions complete)
    ret = smmu_v2_tlb_sync_global(smmu);
    if (ret)
        return ret;

    // 3. Invalidate all TLB entries
    writel_relaxed(0, gr0_base + ARM_SMMU_GR0_TLBIALLH);      // Hyp mode TLB
    writel_relaxed(0, gr0_base + ARM_SMMU_GR0_TLBIALLNSNH);   // Non-secure NH

    ret = smmu_v2_tlb_sync_global(smmu);
    if (ret)
        return ret;

    // 4. Clear all stream mapping registers (invalidate all SMR/S2CR)
    for (i = 0; i < smmu->num_mapping_groups; i++) {
        writel_relaxed(0, gr0_base + ARM_SMMU_GR0_SMR(i));
        writel_relaxed(FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_FAULT),
                       gr0_base + ARM_SMMU_GR0_S2CR(i));
    }

    // 5. Configure global behavior (GBPA = Global Bypass Attribute)
    //    Set to ABORT (deny all unmapped streams)
    writel_relaxed(GBPA_ABORT, gr0_base + ARM_SMMU_GBPA);

    // 6. Enable SMMU features
    scr0 = 0;
    if (smmu->features & ARM_SMMU_FEAT_TRANS_NESTED)
        scr0 |= ARM_SMMU_sCR0_VMIDPNE;  // Enable VMID partitioning

    scr0 |= ARM_SMMU_sCR0_USFCFG;   // Fault on unsupported format
    scr0 |= ARM_SMMU_sCR0_GCFGFRE | ARM_SMMU_sCR0_GCFGFIE;  // Global fault reporting
    scr0 |= ARM_SMMU_sCR0_GFRE | ARM_SMMU_sCR0_GFIE;        // Global fault interrupts

    writel_relaxed(scr0, gr0_base + ARM_SMMU_GR0_sCR0);

    // 7. Enable SMMU globally (clear CLIENTPD)
    scr0 &= ~ARM_SMMU_sCR0_CLIENTPD;
    writel_relaxed(scr0, gr0_base + ARM_SMMU_GR0_sCR0);

    return 0;
}
```

**Context Bank Allocation and Configuration**

```c
static int smmu_v2_init_context_bank(struct hyp_arm_smmu_v2_device *smmu,
                                     struct kvm_hyp_iommu_domain *domain,
                                     u8 cb_idx)
{
    void __iomem *gr1_base = smmu->base + (1 << smmu->pgshift);  // GR1 page
    void __iomem *cb_base = smmu->base + ((cb_idx + 2) << smmu->pgshift);  // CB page
    struct io_pgtable_cfg *pgtbl_cfg = &domain->pgtbl_cfg;
    u32 cbar, vtcr, sctlr;
    bool is_guest_domain = (domain->vmid != 0);

    // 1. Configure CBAR (Context Bank Attribute Register)
    if (is_guest_domain) {
        // Guest devices: Stage-2 translation only (IPA → PA)
        cbar = FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_TRANS);
    } else {
        // Host devices: Stage-2 bypass (optional: use CBAR_TYPE_S2_TRANS with identity map)
        cbar = FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_BYPASS);
    }
    cbar |= FIELD_PREP(ARM_SMMU_CBAR_IRPTNDX, 0);  // Interrupt index
    cbar |= FIELD_PREP(ARM_SMMU_CBAR_VMID, domain->vmid);
    writel_relaxed(cbar, gr1_base + ARM_SMMU_GR1_CBAR(cb_idx));

    // 2. Configure CBA2R (extended attributes)
    u32 cba2r = 0;
    if (smmu->features & ARM_SMMU_FEAT_VMID16)
        cba2r |= FIELD_PREP(ARM_SMMU_CBA2R_VMID16, domain->vmid >> 8);
    cba2r |= ARM_SMMU_CBA2R_VA64;  // 64-bit virtual addressing
    writel_relaxed(cba2r, gr1_base + ARM_SMMU_GR1_CBA2R(cb_idx));

    if (is_guest_domain) {
        // 3. Configure Stage-2 VTCR (Translation Control Register)
        // Note: In Stage-2-only mode, TCR2 is used as VTCR
        vtcr = ARM_SMMU_VTCR_RES1;
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_PS, pgtbl_cfg->oas);     // Output address size
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_TG0, pgtbl_cfg->tg);     // Granule size
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_SH0, pgtbl_cfg->sh);     // Shareability
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_ORGN0, pgtbl_cfg->orgn); // Outer cacheability
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_IRGN0, pgtbl_cfg->irgn); // Inner cacheability
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_SL0, pgtbl_cfg->sl);     // Start level
        vtcr |= FIELD_PREP(ARM_SMMU_VTCR_T0SZ, 64 - domain->ias); // Input address size
        writel_relaxed(vtcr, cb_base + ARM_SMMU_CB_TCR2);

        // 4. Write Stage-2 page table base (Guest IPA → PA translation)
        writeq_relaxed(domain->s2_pgd_phys, cb_base + ARM_SMMU_CB_TTBR0);

        // 5. Configure MAIR (Memory Attribute Indirection Register)
        writeq_relaxed(pgtbl_cfg->mair, cb_base + ARM_SMMU_CB_S1_MAIR0);

        // 6. Enable context bank
        sctlr = ARM_SMMU_SCTLR_M;           // Enable MMU
        sctlr |= ARM_SMMU_SCTLR_TRE;        // TEX remap enable
        sctlr |= ARM_SMMU_SCTLR_AFE;        // Access flag enable
        sctlr |= ARM_SMMU_SCTLR_CFIE;       // Context fault interrupt enable
        sctlr |= ARM_SMMU_SCTLR_CFRE;       // Context fault report enable
        writel_relaxed(sctlr, cb_base + ARM_SMMU_CB_SCTLR);
    }
    // else: bypass mode, no further configuration needed

    return 0;
}
```

#### 3. MMIO Emulation Strategy and Shadow State Management

**MMIO Trap Handler Architecture**

The host stage-2 page tables map SMMU MMIO with `KVM_PGTABLE_PROT_RW | KVM_PGTABLE_PROT_DEVICE`, but trap all accesses via data abort handler:

```c
// arch/arm64/kvm/hyp/nvhe/iommu/iommu.c (modified)
bool kvm_iommu_host_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
    // Check if fault is in SMMU MMIO region
    if (kvm_iommu_ops && kvm_iommu_ops->dabt_handler)
        return kvm_iommu_ops->dabt_handler(regs, esr, addr);

    return false;
}

// drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.c
static bool smmu_v2_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
    struct hyp_arm_smmu_v2_device *smmu;
    bool is_write = esr & ESR_ELx_WNR;
    u64 val;
    int ret;

    // 1. Find which SMMU instance
    smmu = smmu_v2_find_by_mmio_addr(addr);
    if (!smmu)
        return false;

    // 2. Dispatch to register-specific handlers
    u32 offset = addr - smmu->mmio_addr;

    if (offset >= ARM_SMMU_GR0 && offset < ARM_SMMU_GR0 + SZ_64K)
        ret = smmu_v2_handle_gr0(smmu, offset, is_write, &val);
    else if (offset >= ARM_SMMU_GR1 && offset < ARM_SMMU_GR1 + SZ_64K)
        ret = smmu_v2_handle_gr1(smmu, offset, is_write, &val);
    else if (offset >= ARM_SMMU_CB_BASE)
        ret = smmu_v2_handle_cb(smmu, offset, is_write, &val);
    else
        return false;  // Invalid access

    if (!ret) {
        // 3. Complete instruction (advance PC, write result to register)
        if (is_write)
            val = cpu_reg(regs, (esr >> ESR_ELx_SYS64_ISS_RT_SHIFT) & 0x1f);
        else
            cpu_reg(regs, (esr >> ESR_ELx_SYS64_ISS_RT_SHIFT) & 0x1f) = val;

        regs->pc += 4;  // Advance past faulting instruction
    }

    return !ret;
}
```

**Shadow State Structures**

```c
struct hyp_arm_smmu_v2_device {
    // Hardware state
    phys_addr_t mmio_addr;
    void __iomem *base;
    phys_addr_t mmio_addr_sec;      // Secondary register base (if present)
    void __iomem *base_sec;
    bool has_secondary_base;        // True for niso0 and niso1, false for iso
    u32 features;

    // Shadow configuration (what host *thinks* it programmed)
    struct arm_smmu_smr *smrs_shadow;      // Host's view of SMRs
    struct arm_smmu_s2cr *s2crs_shadow;    // Host's view of S2CRs

    // Actual hardware state (what EL2 programmed)
    struct arm_smmu_smr *smrs_hw;          // Real SMR values
    struct arm_smmu_s2cr *s2crs_hw;        // Real S2CR values (with nesting)

    // Context banks
    u32 num_context_banks;
    DECLARE_BITMAP(context_map, ARM_SMMU_MAX_CBS);
    struct smmu_v2_cb_state {
        pkvm_handle_t domain_id;
        u32 cbar;                           // Context bank attributes
        u64 ttbr0;                          // Stage-1 page table base (host)
        u64 ttbr0_s2;                       // Stage-2 page table base (hyp)
        u32 tcr, vtcr;
        u32 sctlr;
        u32 mair[2];
    } cb_state[ARM_SMMU_MAX_CBS];
};
```

**Stream Mapping Emulation (SMR/S2CR)**

Key Principle: EL2 shadows all stream mappings and assigns appropriate context banks:

```c
static int smmu_v2_handle_s2cr(struct hyp_arm_smmu_v2_device *smmu,
                               u32 offset, bool is_write, u64 *val)
{
    u32 idx = (offset - ARM_SMMU_GR0_S2CR(0)) >> 2;
    u32 host_val, hw_val;
    u8 host_cbndx, hyp_cbndx;

    if (idx >= smmu->num_mapping_groups)
        return -EINVAL;

    if (is_write) {
        host_val = *val;
        smmu->s2crs_shadow[idx].val = host_val;

        // Extract host's intended context bank
        host_cbndx = FIELD_GET(ARM_SMMU_S2CR_CBNDX, host_val);

        // Translate: host CB index → hyp CB index (if domain is protected)
        hyp_cbndx = smmu_v2_translate_cbndx(smmu, host_cbndx);

        // For protected guest domains, CB is already configured as Stage-2
        // No additional enforcement needed - CBAR.TYPE already set appropriately
        hw_val = host_val & ~ARM_SMMU_S2CR_CBNDX;
        hw_val |= FIELD_PREP(ARM_SMMU_S2CR_CBNDX, hyp_cbndx);

        // Write modified value to hardware
        writel_relaxed(hw_val, smmu->base + offset);
        smmu->s2crs_hw[idx].val = hw_val;

        // Tegra234: Mirror to sibling
        if (smmu->sibling)
            writel_relaxed(hw_val, smmu->sibling->base + offset);
    } else {
        // Return shadow value (what host thinks it wrote)
        *val = smmu->s2crs_shadow[idx].val;
    }

    return 0;
}
```

#### 4. Memory Controller (MC) Integration and SID Validation

**MC MMIO Trapping Architecture**

The Memory Controller owns the SID override registers that map hardware clients to Stream IDs. EL2 must validate all SID writes to prevent bypass attacks:

```
Hardware Flow:
GPU (client 0x42) → MC SID Override Reg (0x490) → Stream ID (0x5A) → SMMU → Memory
                            ↑
                         Trapped by EL2
```

**Trap Setup:**

```c
// drivers/iommu/arm/arm-smmu/pkvm/tegra234-mc.c
static int mc_trap_init(struct hyp_tegra_mc *mc)
{
    phys_addr_t mc_base = mc->mmio_addr;
    size_t mc_size = mc->mmio_size;

    // 1. Map MC MMIO into EL2 page tables (shared with host)
    int ret = hyp_pin_shared_mem(hyp_phys_to_virt(mc_base),
                                   hyp_phys_to_virt(mc_base + mc_size));
    if (ret)
        return ret;

    // 2. Configure host stage-2 to trap SID override register writes
    //    Map as RO+DEVICE (reads pass-through, writes trap)
    kvm_iommu_host_stage2_idmap(mc_base, mc_base + mc_size,
                                 KVM_PGTABLE_PROT_R | KVM_PGTABLE_PROT_DEVICE);

    // 3. Parse MC client table from device tree (optional, or use static table)
    mc->num_clients = ARRAY_SIZE(tegra234_mc_clients);
    mc->clients = tegra234_mc_clients;

    return 0;
}
```

**SID Assignment Tracking**

```c
// Per-client SID assignment tracking
struct sid_assignment {
    u32 client_id;              // TEGRA234_MEMORY_CLIENT_* (from dt-bindings)
    u32 assigned_sid;           // Stream ID assigned by EL2 to this client
    pkvm_handle_t domain_id;    // Which domain owns this SID
    bool active;                // Is this assignment currently active?
};

static struct sid_assignment sid_map[256];  // Max 256 Stream IDs

// Called during device attach
static int mc_assign_sid(u32 client_id, u32 sid, pkvm_handle_t domain_id)
{
    if (sid >= 256)
        return -EINVAL;

    // Record assignment
    sid_map[sid].client_id = client_id;
    sid_map[sid].assigned_sid = sid;
    sid_map[sid].domain_id = domain_id;
    sid_map[sid].active = true;

    return 0;
}
```

**MC MMIO Trap Handler**

```c
// drivers/iommu/arm/arm-smmu/pkvm/tegra234-mc.c
static bool mc_sid_override_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
    struct hyp_tegra_mc *mc = &tegra234_mc;
    bool is_write = esr & ESR_ELx_WNR;
    u32 offset = addr - mc->mmio_addr;
    u32 val;

    // 1. Reads are always allowed (pass-through to hardware)
    if (!is_write) {
        val = readl_relaxed(mc->base + offset);
        cpu_reg(regs, (esr >> ESR_ELx_SYS64_ISS_RT_SHIFT) & 0x1f) = val;
        regs->pc += 4;
        return true;
    }

    // 2. Writes: validate SID assignment
    val = cpu_reg(regs, (esr >> ESR_ELx_SYS64_ISS_RT_SHIFT) & 0x1f);
    u32 requested_sid = val & MC_SID_STREAMID_OVERRIDE_MASK;  // Lower 8 bits

    // 3. Map MC register offset → client ID
    const struct mc_client_info *client = mc_offset_to_client(mc, offset);
    if (!client) {
        // Not a SID override register, allow write
        goto allow_write;
    }

    // 4. Validate: is this SID assigned to this client?
    if (!mc_validate_sid_for_client(client->client_id, requested_sid)) {
        // SECURITY VIOLATION: client trying to use unassigned SID
        hyp_err("MC: Client %s (0x%x) attempted to use unauthorized SID 0x%x\n",
                client->name, client->client_id, requested_sid);

        // Inject data abort back to host
        inject_undef64(regs);
        return true;
    }

allow_write:
    // 5. Validation passed, allow write to hardware
    writel_relaxed(val, mc->base + offset);

    // 6. Also write security register (enable override)
    if (client && client->sid_security_offset) {
        u32 sec_val = readl_relaxed(mc->base + client->sid_security_offset);
        sec_val |= MC_SID_STREAMID_SECURITY_OVERRIDE;
        writel_relaxed(sec_val, mc->base + client->sid_security_offset);
    }

    regs->pc += 4;
    return true;
}
```

**Resume Path Handling**

When the system resumes from suspend, the MC driver reprograms all SID overrides. These writes automatically trap to EL2 and are validated:

```c
// Host: drivers/memory/tegra/tegra186.c::tegra186_mc_resume()
// (unchanged - existing code)
static int tegra186_mc_resume(struct tegra_mc *mc)
{
    for (i = 0; i < mc->soc->num_clients; i++) {
        const struct tegra_mc_client *client = &mc->soc->clients[i];

        // This write traps to EL2!
        tegra186_mc_client_sid_override(mc, client, client->sid);
        //  └─> writel(sid, mc->regs + client->regs.sid.override)
        //        └─> EL2 mc_sid_override_handler() validates
    }

    return 0;
}
```

**EL2 behavior during resume:**
- Host's SID writes trap to `mc_sid_override_handler()`
- EL2 validates each write against `sid_map[]`
- If client was assigned this SID during boot/device attach → allow
- If client tries to steal another device's SID → deny (inject fault)

**No resume-specific code needed at EL2** - validation is automatic!

#### 5. Error Handling and Recovery Mechanisms

**Hardware Error Detection**

```c
// Global fault handler (called from IRQ or DABT handler)
static void smmu_v2_handle_global_fault(struct hyp_arm_smmu_v2_device *smmu)
{
    void __iomem *gr0_base = smmu->base;
    u32 gfsr = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSR);

    if (gfsr & ARM_SMMU_sGFSR_USF) {
        // Unidentified Stream Fault: unmapped Stream ID tried to access memory
        u32 gfsynr0 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR0);
        u32 gfsynr1 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR1);
        u32 gfsynr2 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR2);

        u32 sid = FIELD_GET(GFSYNR1_STREAMID, gfsynr1);

        hyp_err("SMMU: Unidentified stream fault - SID 0x%x attempted access\n", sid);

        // Clear fault
        writel_relaxed(gfsr, gr0_base + ARM_SMMU_GR0_sGFSR);
    }
}

// Context bank fault handler
static void smmu_v2_handle_context_fault(struct hyp_arm_smmu_v2_device *smmu,
                                         u8 cb_idx)
{
    void __iomem *cb_base = smmu->base + ((cb_idx + 2) << smmu->pgshift);
    u32 fsr = readl_relaxed(cb_base + ARM_SMMU_CB_FSR);

    if (fsr & ARM_SMMU_CB_FSR_FAULT) {
        u64 far = readq_relaxed(cb_base + ARM_SMMU_CB_FAR);
        u32 fsynr0 = readl_relaxed(cb_base + ARM_SMMU_CB_FSYNR0);

        bool is_write = fsynr0 & ARM_SMMU_CB_FSYNR0_WNR;
        u32 sid = FIELD_GET(ARM_SMMU_CB_FSYNR0_S1CBNDX, fsynr0);

        hyp_err("SMMU CB%d fault: VA=0x%llx, %s, SID=0x%x, FSR=0x%x\n",
                cb_idx, far, is_write ? "write" : "read", sid, fsr);

        // Notify guest VM (if this is a protected domain)
        pkvm_handle_t domain_id = smmu->cb_state[cb_idx].domain_id;
        if (domain_id) {
            // Inject fault into guest
            kvm_iommu_inject_fault(domain_id, far, is_write);
        }

        // Clear fault
        writel_relaxed(fsr, cb_base + ARM_SMMU_CB_FSR);
    }
}
```

**TLB Synchronization Errors**

```c
static int smmu_v2_tlb_sync_global(struct hyp_arm_smmu_v2_device *smmu)
{
    void __iomem *gr0_base = smmu->base;
    u32 status;
    u64 timeout = pkvm_time_get() + ARM_SMMU_POLL_TIMEOUT_US;

    writel_relaxed(0, gr0_base + ARM_SMMU_GR0_sTLBGSYNC);

    // Poll for completion
    while (pkvm_time_get() < timeout) {
        status = readl_relaxed(gr0_base + ARM_SMMU_GR0_sTLBGSTATUS);
        if (!(status & ARM_SMMU_sTLBGSTATUS_GSACTIVE))
            return 0;  // Sync complete
    }

    // Timeout: hardware error
    hyp_err("SMMU: TLB sync timeout (GSACTIVE still set)\n");

    // Recovery: force disable/re-enable SMMU
    smmu_v2_emergency_reset(smmu);

    return -ETIMEDOUT;
}
```

#### 6. Implementation Priorities and Call Flows

**Critical Path: Device Attachment Flow**

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 1. HOST: GPU driver requests IOMMU domain attachment                      │
│    iommu_attach_device(domain, &pdev->dev)                                 │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────────────┐
│ 2. EL1 STUB: Translate to hypercall                                        │
│    kvm_iommu_attach_dev(iommu_id=0, domain_id=42, endpoint_id=0x5A,        │
│                         pasid=0, flags=0)                                  │
│    └─> kvm_call_hyp_nvhe(__pkvm_host_iommu_attach_dev, ...)               │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────────────┐
│ 3. EL2 GENERIC: Validate and dispatch                                      │
│    kvm_iommu_attach_dev() @ arch/arm64/kvm/hyp/nvhe/iommu/iommu.c         │
│    └─> kvm_iommu_ops->attach_dev(...)                                     │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────────────┐
│ 4. EL2 SMMU: Configure hardware                                            │
│    smmu_v2_attach_dev() @ pkvm/arm-smmu-v2.c                               │
│    ├─> Allocate context bank (CB)                                          │
│    ├─> Configure SMR: Stream Match Reg (SID = 0x5A)                        │
│    ├─> Configure S2CR: map SID → CB                                        │
│    ├─> Init CB: CBAR (Stage-2 mode), VTCR, TTBR0 (S2 PT), enable MMU      │
│    └─> Record assignment in sid_map[0x5A]                                  │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────────────┐
│ 5. EL2 MC: Record SID assignment                                           │
│    mc_assign_sid(client_id=GPU, sid=0x5A, domain_id=42)                    │
│    └─> sid_map[0x5A] = { .client_id=GPU, .domain_id=42, .active=true }    │
└────────────────────────────────────────────────────────────────────────────┘

Later: GPU driver programs MC SID override
┌────────────────────────────────────────────────────────────────────────────┐
│ 6. HOST: GPU initialization                                                │
│    writel(0x5A, mc->regs + GPU_SID_OVERRIDE_REG)  ← TRAPS TO EL2!         │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────────────┐
│ 7. EL2 MC: Validate write                                                  │
│    mc_sid_override_handler(addr=GPU_SID_OVERRIDE_REG, val=0x5A)            │
│    ├─> Map offset → client_id = GPU                                        │
│    ├─> Check: sid_map[0x5A].client_id == GPU? ✓ YES                        │
│    ├─> Allow write to MC hardware                                          │
│    └─> writel(0x5A, mc_mmio + GPU_SID_OVERRIDE_REG)                        │
└────────────────────────────────────────────────────────────────────────────┘

Result: GPU → MC (SID=0x5A) → SMMU (CB with Stage-2 translation) → Memory ✓
```

**Enforcement Points**

Three layers of protection:

1. **SMMU MMIO Trapping**
   - All host writes to SMMU registers trap to EL2
   - EL2 configures context banks with Stage-2 translation for protected domains
   - Host cannot disable IOMMU or modify Stage-2 page tables

2. **MC SID Override Validation**
   - Host cannot program arbitrary SIDs into MC
   - Each client can only use SIDs assigned by EL2
   - Prevents device impersonation attacks

3. **Consistency Validation**
   - EL2 periodically checks hardware state matches shadow state
   - Detects and recovers from unexpected register modifications
   - Ensures no protected domain is in bypass mode

**Tegra234-Specific: Multi-Instance and Dual Register Bases**

Tegra234 has **3 SMMU instances**, and some instances have **dual register bases**:

1. **smmu_niso1**: Primary @ 0x8000000, Secondary @ 0x7000000
2. **smmu_iso**: Single base @ 0x10000000 (no secondary)
3. **smmu_niso0**: Primary @ 0x12000000, Secondary @ 0x11000000

For instances with dual bases, certain registers (SMR, S2CR) must be programmed **identically** to both bases. This is a Tegra234 hardware requirement, not a coordination between different SMMU instances.

```c
struct hyp_arm_smmu_v2_device {
    phys_addr_t mmio_addr;          // Primary register base
    void __iomem *base;
    phys_addr_t mmio_addr_sec;      // Secondary register base (if present)
    void __iomem *base_sec;
    bool has_secondary_base;        // True for niso0 and niso1, false for iso
};

// Initialization (parse device tree for both register regions)
static int smmu_v2_init_instance(struct hyp_arm_smmu_v2_device *smmu)
{
    // Map primary base
    smmu->base = hyp_ioremap(smmu->mmio_addr, SZ_16M);

    // Map secondary base if present (from DT reg property)
    if (smmu->mmio_addr_sec) {
        smmu->base_sec = hyp_ioremap(smmu->mmio_addr_sec, SZ_16M);
        smmu->has_secondary_base = true;
    }
}

// SMR/S2CR writes mirrored to secondary base (within same instance)
static int smmu_v2_handle_smr(struct hyp_arm_smmu_v2_device *smmu, u32 offset, u64 val)
{
    // Write to primary base
    writel_relaxed(val, smmu->base + offset);

    // Mirror to secondary base if present
    if (smmu->has_secondary_base)
        writel_relaxed(val, smmu->base_sec + offset);

    return 0;
}
```

**Device Tree Parsing:**
```c
// drivers/iommu/arm/arm-smmu/arm-smmu-kvm.c
static int arm_smmu_v2_kvm_probe(struct platform_device *pdev)
{
    // Parse reg property: can have 1 or 2 regions
    struct resource *res0 = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    struct resource *res1 = platform_get_resource(pdev, IORESOURCE_MEM, 1);

    smmu->mmio_addr = res0->start;

    if (res1) {
        smmu->mmio_addr_sec = res1->start;  // Secondary base
        smmu->has_secondary_base = true;
    }
}
```

#### 7. Testing Strategy

**Unit Tests (EL2 Code)**

```c
// Test shadow state consistency
void test_smr_shadow_state(void)
{
    struct hyp_arm_smmu_v2_device smmu;

    // Simulate host writing SMR
    smmu_v2_handle_smr(&smmu, ARM_SMMU_GR0_SMR(5), true, 0x80000042);

    // Verify shadow updated
    assert(smmu.smrs_shadow[5].val == 0x80000042);

    // Verify hardware written
    u32 hw_val = readl(smmu.base + ARM_SMMU_GR0_SMR(5));
    assert(hw_val == 0x80000042);
}

// Test nested translation enforcement
void test_s2cr_nesting_enforcement(void)
{
    struct hyp_arm_smmu_v2_device smmu;

    // Mark CB as protected
    smmu.cb_state[3].domain_id = 42;

    // Simulate host trying to set bypass mode
    u32 bypass_val = FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_BYPASS) |
                     FIELD_PREP(ARM_SMMU_S2CR_CBNDX, 3);

    smmu_v2_handle_s2cr(&smmu, ARM_SMMU_GR0_S2CR(10), true, bypass_val);

    // Verify EL2 forced translation mode
    u32 hw_val = readl(smmu.base + ARM_SMMU_GR0_S2CR(10));
    assert(FIELD_GET(ARM_SMMU_S2CR_TYPE, hw_val) == S2CR_TYPE_TRANS);
}
```

**Integration Tests**

```bash
# Test 1: GPU passthrough to guest VM
echo "Testing GPU passthrough..."
/sys/devices/platform/gpu → unbind from host
qemu-system-aarch64 -device vfio-pci,host=gpu ...
# Verify GPU works in guest, DMA isolation enforced

# Test 2: MC SID override validation
echo "Testing MC SID validation..."
# Attempt to program GPU's SID to display client → should fail
echo 0x5A > /sys/kernel/debug/tegra_mc/client_sid_override/nvdisplayr
# dmesg should show: "MC: Client nvdisplayr attempted to use unauthorized SID 0x5A"

# Test 3: Resume from suspend
echo "Testing suspend/resume..."
echo mem > /sys/power/state
# After resume, verify all SID assignments still valid
cat /sys/kernel/debug/smmu/sid_assignments
```

**Security Tests**

```c
// Test: Host cannot bypass IOMMU by directly writing SMMU registers
void security_test_smmu_bypass_attempt(void)
{
    // Host tries to write S2CR directly (bypassing hypercall)
    // This should trap to EL2 and be emulated/validated

    // Host writes bypass mode
    void __iomem *smmu_base = ioremap(SMMU_BASE, SZ_128K);
    writel(FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_BYPASS),
           smmu_base + ARM_SMMU_GR0_S2CR(10));

    // EL2 should have trapped this and enforced translation mode
    // Read back to verify
    u32 actual = readl(smmu_base + ARM_SMMU_GR0_S2CR(10));
    assert(FIELD_GET(ARM_SMMU_S2CR_TYPE, actual) == S2CR_TYPE_TRANS);
}

// Test: Host cannot steal another device's SID
void security_test_sid_theft_attempt(void)
{
    // GPU has SID 0x5A, try to program Display to use it
    void __iomem *mc_base = ioremap(MC_BASE, SZ_1M);

    writel(0x5A, mc_base + NVDISPLAYR_SID_OVERRIDE);
    // This should trap to EL2 and fail validation

    // Verify MC register wasn't actually written
    u32 actual = readl(mc_base + NVDISPLAYR_SID_OVERRIDE);
    assert(actual != 0x5A);  // Should still have original SID
}
```

#### 8. File Structure Summary

**Files to Create:**

```
drivers/iommu/arm/arm-smmu/
├── arm-smmu-kvm.c              # NEW: EL1 stub driver (~250 lines)
│                               #      - Device tree parsing
│                               #      - Memory donation
│                               #      - Hypercall wrappers
├── Kconfig                     # MODIFIED: Add ARM_SMMU_V2_PKVM option
├── Makefile                    # MODIFIED: Build arm-smmu-kvm.c and pkvm/
└── pkvm/
    ├── Kbuild                  # NEW: EL2 build configuration
    ├── arm-smmu-v2.c           # NEW: Main EL2 driver (~1200 lines)
    │                           #      - Hardware init (smmu_v2_probe, smmu_v2_reset)
    │                           #      - MMIO emulation (smmu_v2_dabt_handler)
    │                           #      - Context bank mgmt (smmu_v2_init_context_bank)
    │                           #      - TLB operations (smmu_v2_tlb_sync_global)
    ├── arm-smmu-v2.h           # NEW: EL2 data structures (~150 lines)
    │                           #      - struct hyp_arm_smmu_v2_device
    │                           #      - struct smmu_v2_cb_state
    ├── tegra234-mc.c           # NEW: MC integration (~350 lines)
    │                           #      - MC MMIO trapping (mc_trap_init)
    │                           #      - SID validation (mc_sid_override_handler)
    │                           #      - Client table management
    └── tegra234-mc.h           # NEW: MC client definitions (~300 lines)
                                #      - static tegra234_mc_clients[] table
                                #      - struct mc_client_info
```

**Reused Infrastructure (no changes needed):**
- `arch/arm64/kvm/hyp/include/nvhe/iommu.h` - Generic IOMMU interface
- `arch/arm64/kvm/hyp/nvhe/iommu/iommu.c` - Domain management
- `drivers/iommu/io-pgtable-arm.c` - Page table operations
- `arch/arm64/kvm/hyp/nvhe/hyp-main.c` - Hypercall dispatch

**Total Implementation:**
- **~2250 lines** of new code
- **7-9 weeks** estimated timeline
- **4 major phases** (Core SMMUv2, MC integration, EL1 stub, Build/Test)

## Pre-Implementation Analysis and Research Findings

### Overview

This section documents comprehensive research conducted to validate the feasibility of implementing pKVM SMMUv2 support for Tegra234. It covers hardware analysis, infrastructure capabilities, critical concerns, and design decisions that inform the implementation strategy.

**Research Completed**: January 2025
**Methodology**: Analysis of existing SMMUv2 driver code, SMMUv3 pKVM reference implementation, pKVM infrastructure, and Tegra234 Memory Controller driver.

### Hardware Interface Analysis Summary

#### SMMUv2 Register Organization

The SMMU is organized into three main register spaces:

**GR0 (Global Register Page 0)**:
- Base: `smmu->base + (0 << smmu->pgshift)`
- Contains: Configuration, capabilities, stream mapping (SMR/S2CR)
- Key registers: sCR0, sGFSR, sTLBGSYNC, sTLBGSTATUS, SMR(n), S2CR(n), ID0-ID7

**GR1 (Global Register Page 1)**:
- Base: `smmu->base + (1 << smmu->pgshift)`
- Contains: Context bank attributes
- Key registers: CBAR(n), CBA2R(n)

**CB (Context Bank Pages)**:
- Base: `smmu->base + ((cb_idx + 2) << smmu->pgshift)`
- Contains: Per-context translation control
- Key registers: SCTLR, TCR, TCR2/VTCR, TTBR0/1, MAIR0/1, FSR, FAR, FSYNR0

**Page Shift**: Determined by ID1.PAGESIZE bit (12 for 4KB, 16 for 64KB)

#### Critical Register Details

**sCR0 (System Control Register 0)** - Global SMMU control:
```c
#define ARM_SMMU_sCR0_CLIENTPD    BIT(0)   // 1=disable SMMU
#define ARM_SMMU_sCR0_GFRE        BIT(1)   // Global fault reporting enable
#define ARM_SMMU_sCR0_GFIE        BIT(2)   // Global fault interrupt enable
#define ARM_SMMU_sCR0_USFCFG      BIT(10)  // Unmatched stream fault config
#define ARM_SMMU_sCR0_VMIDPNE     BIT(11)  // VMID partitioning enable (for nesting)
#define ARM_SMMU_sCR0_VMID16EN    BIT(31)  // 16-bit VMID enable
```

**SMR (Stream Match Register)** - Stream ID matching:
```c
#define ARM_SMMU_SMR_VALID        BIT(31)     // Entry is valid
#define ARM_SMMU_SMR_MASK         GENMASK(31, 16) // Stream ID mask
#define ARM_SMMU_SMR_ID           GENMASK(15, 0)  // Stream ID to match
```

**S2CR (Stream-to-Context Register)** - Maps streams to context banks:
```c
#define ARM_SMMU_S2CR_TYPE        GENMASK(17, 16) // 0=TRANS, 1=BYPASS, 2=FAULT
#define ARM_SMMU_S2CR_CBNDX       GENMASK(7, 0)   // Context Bank Index
```

**CBAR (Context Bank Attribute Register)** - Defines translation mode:
```c
enum arm_smmu_cbar_type {
    CBAR_TYPE_S2_TRANS,             // 0 = Stage-2 only
    CBAR_TYPE_S1_TRANS_S2_BYPASS,   // 1 = Stage-1 with bypass
    CBAR_TYPE_S1_TRANS_S2_FAULT,    // 2 = Stage-1 with fault
    CBAR_TYPE_S1_TRANS_S2_TRANS,    // 3 = Nested (Stage-1 + Stage-2) ← For pKVM
};
```

**TCR2/VTCR** - Stage-2 translation control (when nested mode enabled):
```c
#define ARM_SMMU_VTCR_RES1        BIT(31)          // Reserved (must be 1)
#define ARM_SMMU_VTCR_PS          GENMASK(18, 16)  // Physical Address Size
#define ARM_SMMU_VTCR_SL0         GENMASK(7, 6)    // Start Level
#define ARM_SMMU_VTCR_T0SZ        GENMASK(5, 0)    // Size offset
```

#### Hardware Initialization Sequence

From `arm_smmu_device_reset()`:

1. **Clear global fault status**: Write sGFSR to acknowledge any pending faults
2. **Reset all stream mapping**: Initialize all SMR/S2CR to safe state (FAULT mode)
3. **Disable all context banks**: Clear SCTLR.M for all CBs
4. **Invalidate all TLBs**: Write TLBIALLH, TLBIALLNSNH, sync
5. **Configure sCR0**: Enable faults, VMID partitioning, clear CLIENTPD
6. **Platform-specific quirks**: Apply Tegra234 walk cache workaround (4K pages only)
7. **Final TLB sync and enable**: sTLBGSYNC, enable SMMU

#### TLB Synchronization

**Global TLB Sync**:
```c
// 1. Trigger sync
writel(0, smmu->base + ARM_SMMU_GR0_sTLBGSYNC);

// 2. Poll for completion (with exponential backoff)
while (timeout) {
    u32 status = readl(smmu->base + ARM_SMMU_GR0_sTLBGSTATUS);
    if (!(status & ARM_SMMU_sTLBGSTATUS_GSACTIVE))
        break;  // Complete
    delay *= 2;
}
```

**Context Bank TLB Sync**:
```c
// Per-CB sync at CB page offset
writel(0, cb_base + ARM_SMMU_CB_TLBSYNC);
while (readl(cb_base + ARM_SMMU_CB_TLBSTATUS) & ACTIVE);
```

#### Tegra234-Specific Hardware Quirks

**1. Multi-Instance Dual Register Bases**:

Tegra234 has 3 SMMU instances, two with dual register bases:

| Instance | Primary Base | Secondary Base | Affected Registers |
|----------|--------------|----------------|-------------------|
| smmu_niso1 | 0x8000000 | 0x7000000 | SMR, S2CR, CB config |
| smmu_iso | 0x10000000 | (none) | N/A |
| smmu_niso0 | 0x12000000 | 0x11000000 | SMR, S2CR, CB config |

**Critical**: SMR and S2CR registers must be written **identically to both bases** for niso0 and niso1.

**Implementation Pattern**:
```c
static void nvidia_smmu_write_reg(smmu, page, offset, val) {
    for (i = 0; i < nvidia->num_instances; i++)
        writel(val, nvidia->bases[i] + page + offset);
}
```

**2. Walk Cache Erratum**:

Tegra234 and Tegra194 have a hardware bug where walk cache index calculation differs between translation and invalidation. This causes page faults when PMD entries are released.

**Workaround**: Disable large page mappings (force 4K pages only):
```c
smmu->pgsize_bitmap = SZ_4K;  // Not SZ_2M or SZ_1G
```

**Impact**: Reduced TLB efficiency but ensures correctness.

#### Findings for pKVM Implementation

✅ **Strengths**:
- Register definitions complete and well-documented in `arm-smmu.h`
- Hardware initialization sequence clear
- TLB synchronization mechanisms understood
- Tegra234 quirks already handled in `arm-smmu-nvidia.c`

⚠️ **Gaps**:
- **Nested translation (CBAR_TYPE_S1_TRANS_S2_TRANS) never used** in existing driver
- No VTCR programming examples in codebase
- Unclear how to specify stage-2 page table base in nested mode
- No documentation of dual-PT setup for nested translation

**Critical Unknown**: In nested mode, how does the hardware specify the stage-2 page table base? Possibilities:
1. TTBR1 register (TTBR0=S1 PT, TTBR1=S2 PT)?
2. Separate VTTBR register?
3. Global stage-2 PT configured in GR0/GR1?

**Action Required**: Obtain ARM SMMUv2 Architecture Specification or NVIDIA Tegra234 TRM to clarify nested translation programming.

---

### SMMUv3 pKVM Reference Implementation Analysis

#### Directory Structure

```
drivers/iommu/arm/arm-smmu-v3/
├── arm-smmu-v3.c               # EL1 host driver (~97KB)
├── arm-smmu-v3-kvm.c           # EL1 stub for pKVM (206 lines)
└── pkvm/
    ├── Kbuild                  # EL2 build configuration
    ├── arm-smmu-v3.c           # EL2 hypervisor driver (1095 lines)
    ├── arm_smmu_v3.h           # EL2 data structures (72 lines)
    └── io-pgtable-arm-hyp.c    # Page table wrapper (67 lines)
```

**Note**: A stub header `drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.h` (280 lines) already exists with well-designed data structures, but no implementation files yet.

#### Key Design Patterns Learned

**1. Memory Donation Flow (Three Types)**:

**DONATE (Exclusive Ownership Transfer)**:
```c
// At EL1: Allocate and donate to EL2
void *shadow_array = __get_free_pages(GFP_KERNEL, order);
__pkvm_host_donate_hyp(virt_to_phys(shadow_array) >> PAGE_SHIFT, nr_pages);
// Host loses access completely!

// At EL2: Access donated memory
void *hyp_va = hyp_phys_to_virt(phys_addr);
```
- Used for: Stream tables, command queues, shadow SMR/S2CR arrays
- Host cannot access after donation

**SHARE (Concurrent Access)**:
```c
// At EL1: Share with EL2
for (i = 0; i < nr_pages; i++)
    __pkvm_host_share_hyp((addr + i * PAGE_SIZE) >> PAGE_SHIFT);

// At EL2: Pin to make accessible
hyp_pin_shared_mem(hyp_va_start, hyp_va_end);
```
- Used for: Host's stream table entries (EL2 reads and validates)
- Both host and EL2 can access simultaneously

**IDMAP (Identity-Map in Host Stage-2)**:
```c
// At EL2: Allow host to access MMIO for emulation
kvm_iommu_host_stage2_idmap(smmu_mmio_base, smmu_mmio_base + size,
                             KVM_PGTABLE_PROT_RW | KVM_PGTABLE_PROT_DEVICE);
```
- Used for: SMMU MMIO regions (trap-and-emulate)

**2. MMIO Emulation Architecture**:

```c
// Entry point (arch/arm64/kvm/hyp/nvhe/mem_protect.c)
void handle_host_mem_abort(struct kvm_cpu_context *host_ctxt) {
    u64 addr = read_sysreg_el2(SYS_FAR);
    u64 esr = read_sysreg_el2(SYS_ESR);

    // CRITICAL: IOMMU handler called FIRST for non-memory regions
    if (is_dabt(esr) && !addr_is_memory(addr) &&
        kvm_iommu_host_dabt_handler(&host_ctxt->regs, esr, addr))
        return;  // Handled
    // ... standard page fault handling ...
}

// Driver-specific handler (pkvm/arm-smmu-v3.c)
static bool smmu_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr) {
    bool is_write = esr & ESR_ELx_WNR;
    u32 rt = (esr >> ESR_ELx_SYS64_ISS_RT_SHIFT) & 0x1f;

    // Emulate register access
    if (is_write) {
        u64 val = cpu_reg(regs, rt);
        // Validate and write to hardware
    } else {
        cpu_reg(regs, rt) = readl(smmu->base + offset);
    }

    kvm_skip_host_instr();  // Advance PC by 4 bytes
    return true;
}
```

**3. Shadow State Management**:

SMMUv3 shadows stream table entries to enforce nested translation:
```c
static void smmu_reshadow_ste(smmu, sid) {
    // 1. Read host's STE (via shared memory)
    smmu_copy_from_host(smmu, &target, host_ste_ptr, 64);

    // 2. Modify to add stage-2 translation
    smmu_attach_stage_2(smmu, &target);

    // 3. Write to hardware atomically
    for (i = 1; i < STRTAB_STE_DWORDS; i++)
        WRITE_ONCE(hyp_ste_ptr->data[i], target.data[i]);
    smmu_send_cmd(smmu, &cfgi_cmd);  // Invalidate
    WRITE_ONCE(hyp_ste_ptr->data[0], target.data[0]);  // Activate
}
```

**SMMUv2 Equivalent** (to be implemented):
```c
static int smmu_v2_handle_s2cr_write(smmu, idx, host_val) {
    // Record what host wrote (shadow state)
    smmu->s2crs_shadow[idx].val = host_val;

    // Extract host's intended CB
    u8 host_cbndx = FIELD_GET(ARM_SMMU_S2CR_CBNDX, host_val);

    // Force nested translation for protected domains
    if (is_protected_domain(host_cbndx)) {
        u32 hw_val = host_val & ~ARM_SMMU_S2CR_TYPE;
        hw_val |= FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_TRANS);
        writel(hw_val, smmu->base + offset);
        smmu->s2crs_hw[idx].val = hw_val;  // Real hardware state
    } else {
        writel(host_val, smmu->base + offset);  // Pass-through
    }
}
```

**4. Register Masking Pattern**:

```c
static bool smmu_emulate_register(smmu, offset, is_write, val) {
    u64 mask = 0;

    switch (offset) {
    case ARM_SMMU_IDR0:
        // Hide stage-2 support from host
        mask = read_only & ~(IDR0_S2P | IDR0_VMID16);
        break;
    case ARM_SMMU_CR0:
        // Allow full access
        mask = read_write;
        if (is_write) track_enable_state_change(smmu, val);
        break;
    }

    if (is_write)
        writel(val & mask, smmu->base + offset);
    else
        *val = readl(smmu->base + offset) & mask;
}
```

#### Implementation Status: SMMUv3 vs Required

| Operation | SMMUv3 Status | Required for SMMUv2 |
|-----------|---------------|---------------------|
| `init` | ✅ Implemented | ✅ Must implement |
| `host_stage2_idmap` | ✅ Implemented | ✅ Must implement |
| `dabt_handler` | ✅ Implemented | ✅ Must implement |
| `alloc_domain` | ❌ **Missing** | ✅ **Must implement** |
| `free_domain` | ❌ **Missing** | ✅ **Must implement** |
| `attach_dev` | ❌ **Missing** | ✅ **Must implement** |
| `detach_dev` | ❌ **Missing** | ✅ **Must implement** |
| `map_pages` | ❌ **Missing** | ✅ **Must implement** |
| `unmap_pages` | ❌ **Missing** | ✅ **Must implement** |
| `iova_to_phys` | ❌ **Missing** | ✅ **Must implement** |
| `iotlb_sync` | ❌ **Missing** | ✅ **Must implement** |

**Critical Finding**: SMMUv3 reference implementation is **incomplete** - it only provides initialization and MMIO emulation. The full device lifecycle (domain management, attachment, page table operations) must be implemented from scratch.

**Good News**: The generic pKVM IOMMU framework (`arch/arm64/kvm/hyp/nvhe/iommu/iommu.c`) provides domain allocation infrastructure and memory pools. We only need to implement SMMUv2-specific parts.

#### Architectural Differences: SMMUv2 vs SMMUv3

| Aspect | SMMUv2 (Tegra234) | SMMUv3 (Reference) | Impact on Implementation |
|--------|-------------------|---------------------|-------------------------|
| **Stream Mapping** | SMR + S2CR registers | Stream Table (memory) | ✅ Simpler - fewer entries to shadow |
| **Configuration** | Direct register writes | Stream Table Entries | ✅ Easier MMIO emulation |
| **Command Interface** | Register writes | Command Queue | ✅ No command filtering needed |
| **TLB Invalidation** | Direct register writes | CMDQ commands | ✅ Simpler synchronization |
| **Nesting Mode** | CBAR.TYPE=3 | STE.Config | ⚠️ Documentation gap |
| **Multi-Instance** | Tegra234-specific | Standard | ⚠️ Additional complexity |

**Overall**: SMMUv2 is **simpler** than SMMUv3 in most aspects, except for Tegra234's multi-instance coordination.

#### Reusable Components from SMMUv3

**Can Copy Directly**:
1. `io-pgtable-arm-hyp.c` (67 lines) - Page table allocator wrapper
2. Hypercall registration pattern
3. MMIO emulation dispatch structure
4. Memory donation/sharing patterns

**Cannot Reuse** (different hardware):
1. Stream table management (SMMUv2 uses registers)
2. Command queue handling (SMMUv2 has none)
3. Register layouts (completely different)

---

### pKVM Infrastructure Capabilities and Limitations

#### Memory Donation and Sharing API

**1. Exclusive Donation (Host → EL2)**:
```c
int __pkvm_host_donate_hyp(u64 pfn, u64 nr_pages)
```
- Transfers exclusive ownership to EL2
- Host loses all access (unmapped from host stage-2)
- EL2 can read/write/execute
- **Use for**: SMR/S2CR shadow arrays, page table memory

**Constraints**:
- Page-aligned addresses and sizes required
- Atomic operation (all pages or none)
- Cannot re-donate without first reclaiming
- State validation enforced (must be PKVM_PAGE_OWNED)

**2. Shared Memory (Concurrent Access)**:
```c
int __pkvm_host_share_hyp(u64 pfn)  // Single page!
int hyp_pin_shared_mem(void *from, void *to)
```
- Host retains ownership but shares with EL2
- Both can access simultaneously
- **Use for**: MC MMIO region (host needs bandwidth mgmt, EL2 validates SID writes)

**Critical**: Must call `hyp_pin_shared_mem()` after sharing to actually map in EL2!

**3. Reclaim (EL2 → Host)**:
```c
int __pkvm_hyp_donate_host(u64 pfn, u64 nr_pages)
```
- Reverse of donation
- Used during cleanup/teardown

**4. Unshare**:
```c
hyp_unpin_shared_mem(void *from, void *to)
__pkvm_host_unshare_hyp(u64 pfn)
```
- Must unpin before unsharing
- Reference counting (multiple pins allowed)

#### MMIO Trapping Setup

**Host Stage-2 Identity Mapping**:
```c
void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
                                 enum kvm_pgtable_prot prot)
```

**Protection Flags**:
- `KVM_PGTABLE_PROT_R` - Read permission
- `KVM_PGTABLE_PROT_W` - Write permission
- `KVM_PGTABLE_PROT_X` - Execute permission
- `KVM_PGTABLE_PROT_DEVICE` - Device memory (uncached)

**Trapping Strategies**:

**Option A: Fully Unmapped** (recommended for SMMU/MC):
- Don't call `kvm_iommu_host_stage2_idmap()` at all
- All accesses generate data aborts → EL2 handler
- Full emulation control

**Option B: Read-Only Mapping**:
- Map with `KVM_PGTABLE_PROT_R | KVM_PGTABLE_PROT_DEVICE`
- Reads pass through, writes trap
- **LIMITATION**: Page-granular only! Cannot trap individual registers within a page.

**Critical Limitation**: pKVM's stage-2 page tables can only trap at **4KB page granularity**. Cannot selectively trap individual registers within a page.

**Implication for MC**: Since MC MMIO (0x02c00000, 64KB) contains both SID override registers AND bandwidth/QoS registers, we must either:
1. Unmap entirely → emulate all accesses (recommended)
2. Map RW → no trapping (security hole)

**Decision**: Unmap entire MC MMIO region for full emulation control.

#### Data Abort Handler Flow

```
Hardware Access (e.g., writel(val, mc_mmio + offset))
    ↓
Data Abort (Stage-2 Translation/Permission Fault)
    ↓
handle_trap() → handle_host_mem_abort()
    ↓
Check: is_dabt() && !addr_is_memory(addr)?
    ↓ Yes
kvm_iommu_host_dabt_handler()  ← IOMMU handler called FIRST
    ↓
kvm_iommu_ops->dabt_handler()  ← Driver-specific handler
    ↓
smmu_v2_dabt_handler() or mc_dabt_handler()
    ↓
Emulate access:
  - Extract: is_write, target_register (rt), value
  - Validate and forward to hardware OR return shadow value
  - Advance PC: regs->pc += 4 (or kvm_skip_host_instr())
    ↓
Return to host (next instruction)
```

**Instruction Emulation Helpers**:
```c
bool is_write = esr & ESR_ELx_WNR;
u32 rt = (esr >> ESR_ELx_SYS64_ISS_RT_SHIFT) & 0x1f;

if (is_write) {
    u64 val = cpu_reg(regs, rt);  // Read from X0-X30
    // Validate and write
} else {
    u64 val = readl(hardware_address);
    cpu_reg(regs, rt) = val;  // Write to X0-X30
}
regs->pc += 4;  // Advance to next instruction
```

#### Hypercall Interface

**Registration at EL2**:
```c
static struct kvm_iommu_ops smmu_v2_ops = {
    .init = smmu_v2_init,
    .attach_dev = smmu_v2_attach_dev,
    .detach_dev = smmu_v2_detach_dev,
    .dabt_handler = smmu_v2_dabt_handler,
    // ... other operations
};

int kvm_iommu_init(void *pool_base, size_t nr_pages, &smmu_v2_ops);
```

**Invocation from EL1**:
```c
#include <asm/kvm_asm.h>
#include <linux/arm-smccc.h>

static int host_iommu_attach_dev(u32 iommu_id, u32 domain_id, u32 sid) {
    struct arm_smccc_res res;
    arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(__pkvm_host_iommu_attach_dev),
                      iommu_id, domain_id, sid, 0, 0, 0, 0, &res);
    return res.a1;  // Return value in X1, not X0!
}
```

**Pre-Registered Hypercalls** (already in `arch/arm64/kvm/hyp/nvhe/hyp-main.c`):
- `__pkvm_host_iommu_alloc_domain`
- `__pkvm_host_iommu_free_domain`
- `__pkvm_host_iommu_attach_dev`
- `__pkvm_host_iommu_detach_dev`
- `__pkvm_host_iommu_map_pages`
- `__pkvm_host_iommu_unmap_pages`
- `__pkvm_host_iommu_iova_to_phys`

**No new hypercalls needed** - all infrastructure exists!

#### Page Table Operations

**EL2 Page Table Wrapper** (reuse from SMMUv3):
```c
// drivers/iommu/arm/arm-smmu/pkvm/io-pgtable-arm-hyp.c (copy as-is)
void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp, struct io_pgtable_cfg *cfg) {
    return kvm_iommu_donate_pages_atomic(get_order(size));
}

void __arm_lpae_free_pages(void *addr, size_t size, struct io_pgtable_cfg *cfg) {
    kvm_iommu_reclaim_pages_atomic(addr);
}
```

**Usage**:
```c
struct io_pgtable_cfg cfg = {
    .pgsize_bitmap = SZ_4K,  // Tegra234: 4K only!
    .ias = 48,
    .oas = 48,
    .coherent_walk = true,
};

struct io_pgtable *pgtable = kvm_alloc_io_pgtable_ops(ARM_64_LPAE_S2, &cfg, NULL);
pgtable->ops.map_pages(&pgtable->ops, iova, paddr, pgsize, pgcount, prot, 0, &mapped);
```

#### Critical Limitations

**1. Interrupt Handling: EL2 CANNOT Handle Device Interrupts** ❗❗❗

**Finding**: pKVM's EL2 has **no capability** to handle device interrupts. The GIC remains under host EL1 control.

**Evidence**:
- SMMUv3 pKVM driver **disables** all interrupt registers (returns 0)
- No IRQ routing to EL2 in pKVM architecture
- EL2 only handles **synchronous exceptions** (data aborts, instruction aborts, syscall traps)

**Impact for SMMU**:
- SMMU global faults (GIC_SPI 238, 242, 240, 170, 232) cannot be handled at EL2
- Context bank faults for protected VMs cannot trigger immediate response
- No asynchronous fault detection

**Mitigation Options**:

**Option A: Polling-Based Detection** (used by SMMUv3):
```c
static void smmu_v2_check_faults(struct hyp_arm_smmu_v2_device *smmu) {
    u32 gfsr = readl(smmu->base + ARM_SMMU_GR0_sGFSR);
    if (gfsr & ARM_SMMU_sGFSR_USF) {
        u32 sid = readl(smmu->base + ARM_SMMU_GR0_sGFSYNR1);
        // Log fault, clear status
        writel(gfsr, smmu->base + ARM_SMMU_GR0_sGFSR);
    }
}
// Call after: attach/detach, TLB sync, before returning from MMIO emulation
```
- ✅ Works within pKVM constraints
- ❌ Delayed fault detection (not immediate)
- ❌ Periodic polling would be expensive

**Option B: Host Interrupt → EL2 Notification**:
```c
// Host handles IRQ, notifies EL2 via new hypercall
static irqreturn_t smmu_fault_irq(int irq, void *dev) {
    u32 gfsr = readl(smmu->base + ARM_SMMU_GR0_sGFSR);
    arm_smccc_1_1_hvc(__pkvm_smmu_fault_notify, smmu_id, gfsr, ...);
    writel(gfsr, smmu->base + ARM_SMMU_GR0_sGFSR);
    return IRQ_HANDLED;
}
```
- ✅ Faster fault detection
- ❌ Requires new hypercall registration
- ❌ Security concern: host could inject fake faults

**Recommended**: Use **Option A** for initial implementation. Guest VMs can poll their own fault status via virtio-iommu interface.

**2. MMIO Trapping: Page-Granular Only**

**Limitation**: Stage-2 page tables can only trap at 4KB granularity. Cannot selectively trap individual registers within a page.

**Impact**:
- MC MMIO (0x02c00000, 64KB) contains SID override registers + bandwidth/QoS registers
- Cannot trap only SID registers while allowing QoS registers to pass through

**Solution**: Unmap entire MMIO regions from host stage-2 → all accesses trap → selective emulation in DABT handler.

**Performance**: MC accesses are infrequent (~93 writes on resume, negligible during runtime).

**3. Memory Donation: Page-Aligned Only**

**Constraint**: All donation operations require page-aligned addresses and sizes.

**Impact**: Even small structures (e.g., 128-entry SMR array = 512 bytes) require full page donation.

**Not a problem**: SMMU structures are typically page-sized or larger anyway.

**4. Hypercall Parameters: Limited to 7 Registers**

**Constraint**: Only X1-X7 available for parameters (X0 used for function ID).

**Workaround**: For complex structures, use shared memory instead of passing parameters directly.

**5. Locking Requirements**

**Rule**: Always lock `host_mmu.lock` before `pkvm_pgd_lock` to avoid deadlock.

**Example**:
```c
host_lock_component();
hyp_lock_component();
// ... modify stage-2 page tables ...
hyp_unlock_component();
host_unlock_component();
```

---

### Memory Controller Integration Analysis

#### MC Client Table Overview

**Total Clients**: 93 clients defined in `drivers/memory/tegra/tegra234.c`

**GPU Virtualization-Critical Clients**:

| Client Name | Client ID | SID | Override Offset | Security Offset | SMMU Instance |
|-------------|-----------|-----|----------------|-----------------|---------------|
| **host1xdmar** | 0x16 | 0x27 | 0x0b0 | 0x0b4 | smmu_niso1 |
| **vicsrd/wr** | 0x6c/0x6d | 0x34 | 0x360/0x368 | 0x364/0x36c | smmu_niso1 |
| **nvdecsrd/wr** | 0x78/0x79 | 0x29 | 0x3c0/0x3c8 | 0x3c4/0x3cc | smmu_niso1 |
| **nvdisplayr** | 0x92 | 0x01 | 0x490 | 0x494 | smmu_iso/niso0 |
| **nvencsrd/wr** | 0x1c/0x2b | 0x24 | 0xe0/0x158 | 0xe4/0x15c | smmu_niso0 |
| **dla0rdb/wrb** | 0x2c/0x2e | 0x2b | 0x160/0x170 | 0x164/0x174 | smmu_niso1 |
| **dla1rdb** | 0x2f | 0x23 | 0x178 | 0x17c | smmu_niso0 |

**Data Structure**:
```c
struct tegra_mc_client {
    unsigned int id;           // Client ID (e.g., 0x92 for nvdisplayr)
    const char *name;
    unsigned int sid;          // Default Stream ID
    struct {
        struct {
            unsigned int override;   // SID override register offset
            unsigned int security;   // SID security register offset
        } sid;
    } regs;
};
```

#### SID Override Register Layout

**MC Base Address**: `0x02c00000` (64KB SID region)

**Register Bit Fields**:
```c
#define MC_SID_STREAMID_OVERRIDE_MASK         GENMASK(7, 0)   // SID value
#define MC_SID_STREAMID_SECURITY_OVERRIDE     BIT(8)          // Override enable
#define MC_SID_STREAMID_SECURITY_WRITE_ACCESS_DISABLED BIT(16) // Write lock
```

**Programming Sequence** (`drivers/memory/tegra/tegra186.c:85-124`):
```c
static void tegra186_mc_client_sid_override(mc, client, sid) {
    u32 value;

    // 1. Check/enable security override bit
    value = readl(mc->regs + client->regs.sid.security);
    if (!(value & MC_SID_STREAMID_SECURITY_OVERRIDE)) {
        if (value & MC_SID_STREAMID_SECURITY_WRITE_ACCESS_DISABLED)
            return;  // Locked by secure firmware
        value |= MC_SID_STREAMID_SECURITY_OVERRIDE;
        writel(value, mc->regs + client->regs.sid.security);
    }

    // 2. Write SID override
    writel(sid, mc->regs + client->regs.sid.override);
}
```

#### When SID Programming Occurs

**A. Device Probe Time** (`tegra186_mc_probe_device`):
- Called when IOMMU registers a device
- Reads device's stream ID from device tree
- Programs MC SID override register

**B. Resume from Suspend** (`tegra186_mc_resume`) ⚠️:
```c
static int tegra186_mc_resume(struct tegra_mc *mc) {
    // UNCONDITIONALLY reprogram ALL 93 clients!
    for (i = 0; i < mc->soc->num_clients; i++)
        tegra186_mc_client_sid_override(mc, &clients[i], clients[i].sid);
    return 0;
}
```

**Critical**: During resume, **all SID overrides** are reprogrammed to their default values, regardless of whether the device is active or owned by a guest VM.

**Implication for pKVM**: ~93 trapped writes on every resume. Each write must be validated against EL2's SID assignment tracking.

#### MMIO Regions

**SID Override Region** (must trap for pKVM):
- Base: `0x02c00000`
- Size: `0x10000` (64KB)
- Contains: SID override and security registers for all 93 clients

**Other MC Regions** (should NOT trap):
- Broadcast: `0x02c10000` - `0x02c1ffff`
- Channels MC0-MC3: `0x02c20000` - `0x02c5ffff`
- Channels MC4-MC7: `0x02b80000` - `0x02bbffff`
- Channels MC8-MC15: `0x01700000` - `0x0177ffff`

**These contain bandwidth/QoS registers** that host needs for memory management.

#### Trapping Strategy for pKVM

**Recommended: Unmap Entire SID Region**:
```c
// Don't map MC SID region (0x02c00000-0x02c0ffff) in host stage-2
// All accesses generate data aborts → EL2 handler

// Allow other MC regions (bandwidth, QoS):
kvm_iommu_host_stage2_idmap(0x02c10000, 0x02d00000,  // Broadcast + channels
                             KVM_PGTABLE_PROT_RW | KVM_PGTABLE_PROT_DEVICE);
```

**MC DABT Handler at EL2**:
```c
bool mc_sid_override_handler(struct user_pt_regs *regs, u64 esr, u64 addr) {
    u32 offset = addr - MC_SID_REGION_BASE;
    bool is_write = esr & ESR_ELx_WNR;

    // Allow all reads (pass-through to hardware)
    if (!is_write) {
        u32 val = readl(mc_base + offset);
        cpu_reg(regs, rt) = val;
        regs->pc += 4;
        return true;
    }

    // Validate writes
    u32 val = cpu_reg(regs, rt);
    u32 requested_sid = val & MC_SID_STREAMID_OVERRIDE_MASK;

    const struct mc_client_info *client = mc_offset_to_client(offset);
    if (!client) {
        // Not a SID register, allow write
        writel(val, mc_base + offset);
        regs->pc += 4;
        return true;
    }

    // Check: is this SID assigned to this client?
    if (!validate_sid_for_client(client->id, requested_sid)) {
        // SECURITY VIOLATION: client trying to use unauthorized SID
        inject_undef64(regs);  // Abort
        return true;
    }

    // Validation passed - allow write
    writel(val, mc_base + offset);
    // Also write security register (enable override)
    writel(MC_SID_STREAMID_SECURITY_OVERRIDE, mc_base + client->sid_security_offset);
    regs->pc += 4;
    return true;
}
```

**SID Assignment Tracking** (at EL2):
```c
struct sid_assignment {
    u32 client_id;              // TEGRA234_MEMORY_CLIENT_*
    u32 assigned_sid;           // Stream ID assigned by EL2
    pkvm_handle_t domain_id;    // Owning domain
    bool active;
};

static struct sid_assignment sid_map[256];  // Max 256 Stream IDs

// Called during device attach
static int mc_assign_sid(u32 client_id, u32 sid, pkvm_handle_t domain_id) {
    sid_map[sid] = {
        .client_id = client_id,
        .assigned_sid = sid,
        .domain_id = domain_id,
        .active = true
    };
}
```

#### Resume Path Handling

**Challenge**: Host MC driver unconditionally reprograms all 93 SIDs during resume.

**EL2 Behavior**:
1. Each SID override write traps to EL2
2. EL2 validates: `sid_map[requested_sid].client_id == this_client?`
3. If yes → allow write to hardware
4. If no → deny (inject fault)

**Performance Impact**:
- 93 trapped writes × ~500 cycles/trap = ~46,500 cycles
- At 2GHz = **~23 microseconds** overhead per resume
- **Acceptable** (resume takes milliseconds total)

**Resume Safety**: EL2's SID assignments must persist across suspend/resume (stored in memory that survives suspend).

#### Implementation Requirements

**1. Static Client Table** (~300 lines):
```c
// drivers/iommu/arm/arm-smmu/pkvm/tegra234-mc.h
static const struct mc_client_info tegra234_mc_clients[] = {
    { TEGRA234_MEMORY_CLIENT_HOST1XDMAR, "host1xdmar", 0x0b0, 0x0b4 },
    { TEGRA234_MEMORY_CLIENT_VICSRD, "vicsrd", 0x360, 0x364 },
    // ... all 93 clients
};
```

**2. MC MMIO Trap Setup** (~50 lines):
```c
static int mc_trap_init(struct hyp_tegra_mc *mc) {
    // Leave MC SID region unmapped (traps all accesses)
    // Map other MC regions as RW (bandwidth mgmt)
}
```

**3. MC DABT Handler** (~150 lines):
```c
bool mc_sid_override_handler(regs, esr, addr);
const struct mc_client_info *mc_offset_to_client(u32 offset);
bool validate_sid_for_client(u32 client_id, u32 sid);
```

**Total**: ~500 lines for MC integration.

---

### Critical Concerns and Open Questions

#### Concern #1: Interrupt Handling Limitation

**Issue**: EL2 cannot handle device interrupts (no GIC access at EL2).

**Impact**:
- SMMU fault interrupts (GIC_SPI 238, 242, 240, 170, 232) cannot be handled at EL2
- No immediate notification of translation faults
- Guest VM faults not immediately visible

**Severity**: ⚠️ High

**Mitigation**:
- **Polling-based fault detection** (check FSR/FSYNR after operations)
- Guest VM polls its own fault status via virtio-iommu
- Host interrupt handler could notify EL2 via hypercall (requires new hypercall)

**Decision Required**: Is polling-based fault detection acceptable for MVP?

#### Concern #2: MMIO Trapping Granularity

**Issue**: Stage-2 page tables can only trap at 4KB page granularity.

**Impact**:
- MC MMIO (64KB) contains SID override registers + bandwidth/QoS registers
- Cannot selectively trap only SID registers

**Severity**: ⚠️ Medium

**Mitigation**:
- **Unmap entire MC SID region** → emulate all accesses
- Performance overhead: ~23μs per resume (acceptable)

**Decision**: Use full MC MMIO emulation for SID region.

#### Concern #3: ~~Nested Translation Documentation Gap~~ (RESOLVED)

**Previous Issue**: Concern about nested translation programming requirements.

**Resolution**: **Nested translation is NOT required for GPU passthrough!**

**Simplified Approach**:
- SMMUv2 doesn't support hardware nested translation (Stage-1 + Stage-2 simultaneously)
- For guest devices: Use **Stage-2 only** translation (IPA → PA)
  - CBAR.TYPE = CBAR_TYPE_S2_TRANS
  - VTCR programmed via TCR2 register
  - TTBR0 points to Stage-2 page table (guest IPA → PA mapping)
- For host devices: Use **Stage-2 bypass** or identity mapping
  - CBAR.TYPE = CBAR_TYPE_S2_BYPASS

**Why This Works**:
- Guest device drivers issue DMA to IPA addresses (unaware of SMMU)
- SMMU translates IPA → PA using EL2-controlled page tables
- No Stage-1 complexity, no dual page table management
- Simpler than SMMUv3's nested approach

**Severity**: ✅ **Resolved** (not a concern)

**Implementation Impact**: Significantly simplified - no documentation gap, no experimental programming needed

#### Concern #4: SMMUv3 Reference Incompleteness

**Issue**: SMMUv3 pKVM implementation only provides init/idmap/dabt_handler. Missing domain lifecycle operations.

**Impact**: Cannot use SMMUv3 as complete reference for device attachment, page table management, TLB sync.

**Severity**: ⚠️ Low

**Mitigation**: Implement from first principles using generic pKVM IOMMU framework. Not a blocker - just more work.

#### Concern #5: Multi-Instance Coordination

**Issue**: Tegra234 has 3 SMMU instances, two with dual register bases.

**Impact**: SMR/S2CR writes must be mirrored to both primary and secondary bases.

**Severity**: ⚠️ Low

**Mitigation**: Already solved in `arm-smmu-nvidia.c`. Copy the pattern:
```c
static inline void smmu_writel(smmu, page, offset, val) {
    writel(val, smmu->base + page + offset);
    if (smmu->has_secondary_base)
        writel(val, smmu->base_sec + page + offset);
}
```

#### Concern #6: Resume Path Overhead

**Issue**: 93 MC SID override writes on every resume.

**Impact**: ~23μs overhead per resume.

**Severity**: ⚠️ Very Low

**Mitigation**: Accept the overhead (negligible compared to resume time). No action needed.

---

### Design Decisions and Rationale

#### Decision Matrix

| Decision | Rationale | Trade-offs | Alternatives Considered |
|----------|-----------|------------|------------------------|
| **Polling-based fault detection** | EL2 cannot handle interrupts | Delayed fault detection | Host IRQ → hypercall (complex, security risk) |
| **Full MC MMIO emulation** | Page-granular trapping limitation | 23μs/resume overhead | Partial trapping (not possible), ignore MC (security hole) |
| **Unmap SMMU MMIO entirely** | Clean trap-and-emulate model | All accesses trap | Partial mapping (complex, error-prone) |
| **Reuse io-pgtable-arm** | Already EL2-ready from SMMUv3 work | None | Custom PT code (reinventing wheel) |
| **Dynamic SID assignment** | No static policy files needed | More runtime validation | Static SID config files (rigid, hard to maintain) |
| **Stage-2 only (no nesting)** | Simpler implementation, no unknowns | SMMUv2 limitation becomes advantage | Nested translation (not supported in HW) |
| **Shadow SMR/S2CR state** | Track context bank assignments | Extra memory (512 bytes × 3 instances) | Direct hardware writes (tracking loss) |
| **Single SID per device** | Simplifies validation | Limits flexibility | Multiple SIDs (complex tracking) |
| **Accept resume overhead** | 23μs is negligible | None | Cache SID assignments (complex, minimal gain) |

#### Architectural Principles

1. **Security First**: EL2 is authoritative for all Stream ID assignments
2. **Zero Trust**: Validate every MC SID override write
3. **Fail Secure**: Deny unknown SID assignments (don't guess)
4. **Simplicity**: Use polling over complex interrupt forwarding
5. **Reuse**: Leverage existing pKVM infrastructure (io-pgtable, hypercalls, memory pools)
6. **Compatibility**: Minimize changes to host drivers (MC driver unchanged)

---

### Implementation Readiness Assessment

#### Readiness Checklist

| Component | Status | Confidence | Blocker? |
|-----------|--------|------------|----------|
| **Register definitions** | ✅ Complete | High | No |
| **Hardware initialization** | ✅ Understood | High | No |
| **Memory donation/sharing** | ✅ Patterns clear | High | No |
| **MC integration strategy** | ✅ Defined | High | No |
| **TLB operations** | ✅ Understood | High | No |
| **Multi-instance coordination** | ✅ Reusable | Medium | No |
| **Interrupt handling workaround** | ⚠️ Polling-based | Medium | No (acceptable) |
| **Stage-2 translation programming** | ✅ **Well-documented** | **High** | **No** |
| **Page table operations** | ✅ Reusable | High | No |
| **MMIO emulation patterns** | ✅ Clear | High | No |

**Overall Assessment**: **95% ready**. Stage-2-only approach eliminates previous documentation gap.

#### Risk Assessment

**High Risk**:
- ✅ **None remaining** - Previous nested translation concern resolved by using Stage-2-only approach

**Medium Risk**:
1. **Polling-based fault detection** - Delayed fault notifications
   - **Mitigation**: Acceptable for MVP, can add host IRQ → hypercall later

2. **MC MMIO emulation performance** - 23μs per resume
   - **Mitigation**: Overhead is negligible in resume context

**Low Risk**:
1. **SMMUv3 reference incompleteness** - Missing domain operations
   - **Mitigation**: Implement from scratch using framework

---

### Recommended Approach and Next Steps

#### ~~Phase 0: Documentation Acquisition~~ (NO LONGER NEEDED)

**Previous Requirement**: ARM SMMUv2 spec for nested translation - **OBSOLETE**

**Resolution**: Stage-2-only approach uses well-documented SMMUv2 features (CBAR_TYPE_S2_TRANS, VTCR via TCR2, TTBR0). No spec needed.

**Optional**: Review Tegra234 SMMU errata (if available) for any additional workarounds beyond walk cache issue.

#### Phase 1: Core EL2 Driver (2-3 weeks)

**Week 1-2: Basic Infrastructure**:
- Create `drivers/iommu/arm/arm-smmu/pkvm/arm-smmu-v2.c` (~400 lines)
  - Hardware probe (`smmu_v2_probe_device`) - read ID registers
  - Hardware reset (`smmu_v2_reset`) - initialize to safe state
  - Multi-instance setup (parse DT for dual register bases)
- Copy `io-pgtable-arm-hyp.c` from SMMUv3 (67 lines)
- Build system integration (Kconfig, Makefile, Kbuild)

**Week 3: Context Bank Management**:
- Context bank allocation (`smmu_v2_alloc_context_bank`) - bitmap tracking
- **Stage-2 translation setup** (`smmu_v2_init_s2_cb`) - **Straightforward**
  - Guest domains: CBAR.TYPE = CBAR_TYPE_S2_TRANS
  - Host domains: CBAR.TYPE = CBAR_TYPE_S2_BYPASS
  - Configure VTCR via TCR2 (well-documented fields)
  - Write TTBR0 with Stage-2 page table base
- Enable context bank (SCTLR.M = 1)

**Week 4: TLB Operations**:
- Global TLB sync (`smmu_v2_tlb_sync_global`)
- Context TLB sync (`smmu_v2_tlb_sync_context`)
- TLB invalidation (by ASID, by VA range)
- Multi-instance TLB sync (poll all bases)

**Deliverable**: Working EL2 driver that can initialize SMMU hardware.

#### Phase 2: MMIO Emulation and Shadow State (1-2 weeks)

**Week 4-5: Shadow State Management**:
- Allocate SMR/S2CR shadow arrays (donate to EL2)
- Shadow tracking structures
- SMR write emulation (`smmu_v2_handle_smr`)
- S2CR write emulation (`smmu_v2_handle_s2cr`) - context bank assignment

**Week 6: MMIO Trap Handler**:
- Register DABT handler (`smmu_v2_dabt_handler`)
- Dispatch to register-specific handlers
- GR0/GR1/CB register emulation
- Instruction completion (advance PC)

**Deliverable**: SMMU register accesses from host trap to EL2 and are emulated correctly.

#### Phase 3: MC Integration (1-2 weeks)

**Week 7: MC MMIO Trapping**:
- Create `tegra234-mc.c` (~350 lines)
- Static MC client table (93 clients with register offsets)
- MC MMIO trap setup (unmap SID region)
- MC DABT handler (`mc_sid_override_handler`)
- SID assignment tracking (`sid_map[]`)
- Validation logic (`validate_sid_for_client`)

**Week 8: Integration Testing**:
- Test SID override validation
- Test resume path (93 trapped writes)
- Test security (SID theft prevention)

**Deliverable**: MC SID override writes are validated and enforced by EL2.

#### Phase 4: EL1 Stub Driver (1 week)

**Week 9: Host Stub**:
- Create `arm-smmu-kvm.c` (~250 lines)
- Device tree parsing (MMIO bases, interrupts, capabilities)
- Memory donation (SMR/S2CR arrays, MC MMIO)
- Hypercall wrappers (attach/detach/map/unmap)
- Registration with pKVM IOMMU framework

**Deliverable**: Host can invoke EL2 SMMU operations via hypercalls.

#### Phase 5: Domain Operations (1-2 weeks)

**Week 10-11: Full IOMMU Ops**:
- `smmu_v2_alloc_domain` - allocate page tables
- `smmu_v2_attach_dev` - configure CB, map stream ID
- `smmu_v2_detach_dev` - unmap stream, free CB
- `smmu_v2_map_pages` - IOVA→PA mapping via io-pgtable
- `smmu_v2_unmap_pages` - unmap IOVA range
- `smmu_v2_iova_to_phys` - address translation
- `smmu_v2_iotlb_sync` - TLB invalidation

**Deliverable**: Complete IOMMU operations functional.

#### Phase 6: Testing and Validation (2 weeks)

**Week 12: Unit Testing**:
- Shadow state consistency tests
- TLB sync correctness
- MC SID validation tests
- Multi-instance coordination tests

**Week 13: Integration Testing**:
- Single device attachment (VIC or NVDEC)
- DMA isolation verification
- Fault handling (polling-based)
- Suspend/resume with SID persistence

**Deliverable**: Validated implementation ready for GPU passthrough testing.

#### Total Timeline: **6-8 weeks**

**Improvement**: 3 weeks faster than original estimate due to Stage-2-only simplification.

**Critical Path**: None - all blocking concerns resolved.

---

### Questions for Design Review

Before proceeding with implementation, please clarify:

1. ~~**Nested Translation Documentation**~~ - **RESOLVED**: Stage-2-only approach eliminates this requirement.

2. **Interrupt Handling**: Is polling-based fault detection acceptable for the initial implementation, or do you require immediate interrupt-based handling?

3. **MC MMIO Performance**: Is 23μs resume overhead acceptable for full MC MMIO emulation? (Alternative: accept security risk of not trapping SID overrides)

4. **SMMU Instance Scope**: Should we implement all 3 SMMU instances (niso0, niso1, iso) initially, or start with just smmu_niso1 (HOST1X/GPU engines)?

5. **Testing Incremental**: Do you have a way to test incrementally (e.g., single device passthrough), or does the entire implementation need to be complete before first hardware testing?

6. ~~**Fallback Plan**~~ - **RESOLVED**: No experimental approach needed - Stage-2-only uses standard SMMUv2 features.

### Conclusion

**Feasibility**: Implementation is **highly feasible** within pKVM's constraints, with all major concerns resolved.

**Estimated Effort**: **6-8 weeks** for full implementation and testing (3 weeks faster than original estimate).

**Main Benefits of Stage-2-Only Approach**:
1. ✅ No nested translation complexity or documentation gaps
2. ✅ Simpler implementation (fewer lines of code)
3. ✅ Well-documented SMMUv2 features (CBAR_TYPE_S2_TRANS, VTCR)
4. ✅ No experimental programming needed
5. ✅ SMMUv2's limitation becomes an advantage

**Remaining Risks**: Only low-medium risks remain (polling-based faults, MC performance overhead) - all acceptable with clear mitigations.

**Recommendation**: **Proceed with implementation immediately**. All blocking concerns have been resolved. Stage-2-only approach is simpler, faster, and eliminates all critical unknowns.

