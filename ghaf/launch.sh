#! /bin/sh

if test "x${DTB}" = "x"; then
  DTB=tegra234-gpuvm.dtb
fi

EARLYCON=""
#EARLYCON="earlycon=pl011,0x9000000,115200n8"

exec /home/ubuntu/qemu/build/qemu-system-aarch64.wrapper \
	`# minicom already uses ^A, change monitor escape to ^T` \
	-echr 0x14 \
	-nographic \
	-machine virt,accel=kvm \
	-cpu host \
	-smp 4 \
	-m 6000 \
	-no-reboot \
	-kernel /home/ubuntu/Image \
	-dtb ${DTB} \
	-drive file=rootfs-guest.qcow2,if=none,id=drive0,format=qcow2 \
        -device virtio-blk-pci,drive=drive0,packed=on \
        -netdev user,id=net0,hostfwd=tcp::2222-:22 \
        -device virtio-net-pci,netdev=net0 \
	-append "rootwait root=/dev/vda quiet loglevel=2 "${EARLYCON}" clk_ignore_unused pd_ignore_unused" \
	`# enable multiplexer on stdio to have both guest and monitor` \
	-chardev stdio,id=mon,mux=on,signal=off \
	`# enable monitor` \
	-mon chardev=mon,mode=readline \
	-serial chardev:mon \
	`# passthrough devices` \
	-device vfio-platform,host=70000000.vm_hs_p,mmio-base=0x70000000 \
	-device vfio-platform,host=80000000.vm_cma_p,mmio-base=0x80000000 \
	-device vfio-platform,host=100000000.vm_cma_vram_p,mmio-base=0x100000000 \
	-device vfio-platform,host=17000000.gpu \
	-device vfio-platform,host=13e00000.host1x_pt \
	-device vfio-platform,host=15340000.vic \
	-device vfio-platform,host=15480000.nvdec \
	-device vfio-platform,host=15540000.nvjpg \
	-device vfio-platform,host=d800000.dce \
	-device vfio-platform,host=13800000.display
