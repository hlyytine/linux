#! /bin/sh

GHAF_DIR=~/ghaf
LDK_DIR=~/pkvm/Linux_for_Tegra

DTBS=""

compile_dts () {
    BASENAME=$(basename "$1" .dts)
    case "$1" in
        *overlay*)
            SUFFIX=".dtbo"
            ;;
        *)
            SUFFIX=".dtb"
            ;;
    esac

    aarch64-linux-gnu-gcc                                                      \
        -E                                                                     \
        -nostdinc                                                              \
        -I ${LDK_DIR}/source/hardware/nvidia/t23x/nv-public/include/nvidia-oot \
        -I ${LDK_DIR}/source/hardware/nvidia/t23x/nv-public/include/kernel     \
        -undef -D__DTS__                                                       \
        -x assembler-with-cpp                                                  \
        $1                                                                     \
        > preprocessed.dts

    dtc -I dts -O dtb -o ${BASENAME}${SUFFIX} preprocessed.dts
    rm -f preprocessed.dts

    DTBS="${DTBS} ${BASENAME}${SUFFIX}"
}

################################# BPMP proxy #################################

# host overlay
compile_dts ${GHAF_DIR}/modules/reference/hardware/jetpack/nvidia-jetson-orin/virtualization/host/bpmp-virt-host/bpmp_host_overlay.dts

################################### GPU VM ###################################

# host overlay
compile_dts ${GHAF_DIR}/modules/microvm/sysvms/gpuvm_res/gpu_passthrough_overlay.dts

# guest device tree
compile_dts ${GHAF_DIR}/modules/microvm/sysvms/gpuvm_res/tegra234-gpuvm.dts

################################### Net VM ###################################

# host overlay
compile_dts ${GHAF_DIR}/modules/reference/hardware/jetpack/agx-ethernet-pci-passthough-overlay.dts

# guest device tree
compile_dts ${GHAF_DIR}/modules/reference/hardware/jetpack/tegra234-netvm.dts

tar cf - ${DTBS} | ssh ubuntu@192.168.101.112 'tar xvf -'
