#! /bin/sh

#EARLYCON=""
EARLYCON="earlycon=pl011,0x9000000,115200n8"

exec /home/ubuntu/qemu/build/qemu-system-aarch64.wrapper \
	`# minicom already uses ^A, change monitor escape to ^T` \
	-echr 0x14 \
	-nographic \
	-trace events=trace-events-vfio,file=trace-vfio.log \
	-machine virt,gic-version=3,accel=kvm \
	-cpu host \
	-m 1024 \
	-object memory-backend-memfd,id=mem0,size=1024M,share=on \
	-numa node,memdev=mem0 \
	-no-reboot \
	-dtb tegra234-netvm.dtb \
	-kernel /home/ubuntu/Image \
	-device virtio-iommu-pci \
	-drive file=rootfs-guest.qcow2,if=none,id=drive0,format=qcow2 \
        -device virtio-blk-pci,drive=drive0,packed=on \
	  -netdev user,id=net0,hostfwd=tcp::2223-:22 \
	    -device virtio-net-pci,netdev=net0 \
	-append "rootwait root=/dev/vda quiet loglevel=3 "${EARLYCON}" clk_ignore_unused pd_ignore_unused" \
	`# enable multiplexer on stdio to have both guest and monitor` \
	-chardev stdio,id=mon,mux=on,signal=off \
	`# enable monitor` \
	-mon chardev=mon,mode=readline \
	-serial chardev:mon \
	`# passthrough devices` \
	-device vfio-pci,host="0001:01:00.0" \
	-device vfio-platform,host=b0000000.vm_cma_net_p,mmio-base=0xb0000000
