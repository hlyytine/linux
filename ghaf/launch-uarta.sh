#! /bin/sh

if test "x${DTB}" = "x"; then
  DTB=tegra234-gpuvm.dtb
fi

#echo vfio-platform > /sys/bus/platform/devices/60000000.vm_hs_p/driver_override
#echo vfio-platform > /sys/bus/platform/devices/80000000.vm_cma_p/driver_override
#echo vfio-platform > /sys/bus/platform/devices/100000000.vm_cma_vram_p/driver_override

#echo 60000000.vm_hs_p > /sys/bus/platform/drivers/vfio-platform/bind
#echo 80000000.vm_cma_p > /sys/bus/platform/drivers/vfio-platform/bind
#echo 100000000.vm_cma_vram_p > /sys/bus/platform/drivers/vfio-platform/bind

#EARLYCON=""
EARLYCON="earlycon=pl011,0x9000000,115200n8"

#KERNEL_LOG_OPTS="loglevel=7 debug"
KERNEL_LOG_OPTS="loglevel=9 debug"

echo "Using ${DTB}"

#	-append "rootwait root=/dev/vda console=hvc0 "${EARLYCON}" clk_ignore_unused pd_ignore_unused initcall_debug log_buf_len=512k" 

/home/ubuntu/qemu/build/qemu-system-aarch64 \
	-d guest_errors -D logfile \
	`# minicom already uses ^A, change monitor escape to ^T` \
	-echr 0x14 \
	-nographic \
	-machine virt,accel=kvm \
	-cpu host \
	-smp 4 \
	-m 6000 \
	-no-reboot \
	-kernel /home/ubuntu/Image \
	-drive file=rootfs-guest.qcow2,if=none,id=drive0,format=qcow2 \
        -device virtio-blk-pci,drive=drive0,packed=on \
	`# -machine dumpdtb=qemu.dtb` \
	`# -dtb qemu.dtb`  \
	-dtb ${DTB} \
	-append "rootwait root=/dev/vda console=hvc0 "${EARLYCON}" console=ttyS0,115200n8 clk_ignore_unused pd_ignore_unused" \
	`# enable multiplexer on stdio to have both guest and monitor` \
	-chardev stdio,id=mon,mux=on,signal=off \
	`# enable monitor` \
	-mon chardev=mon,mode=readline \
	-serial chardev:mon \
	`# virtio-console` \
	-device virtio-serial-pci${GUEST_DEV_ARGS_APPEND} \
	-device virtconsole,chardev=mon,id=console0,name=qemu.gpuvm \
	`# -global virtio-pci.disable-legacy=on` \
	`# -global virtio-pci.iommu_platform=on` \
	-device vfio-platform,host=80000000.vm_cma_p,mmio-base=0x80000000 \
	-device vfio-platform,host=3100000.serial



#	-device vfio-pci,host=0001:01:00.0


#	-device vfio-platform,host=60000000.vm_hs_p,mmio-base=0x60000000 \

#	-device vfio-platform,host=100000000.vm_cma_vram_p,mmio-base=0x100000000 \
#	-device vfio-platform,host=17000000.gpu \
#	-device vfio-platform,host=13e00000.host1x_pt \
#	-device vfio-platform,host=15340000.vic \
#	-device vfio-platform,host=15480000.nvdec \
#	-device vfio-platform,host=15540000.nvjpg \
#	-device vfio-platform,host=d800000.dce \
#	-device vfio-platform,host=13800000.display

