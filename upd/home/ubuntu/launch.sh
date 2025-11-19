#! /bin/sh

if test "x${DTB}" = "x"; then
  DTB=tegra234-qemu-4cpus-4gb.dtb
fi

EARLYCON="earlycon=pl011,0x9000000,115200n8"

QEMU_PASSTHROUGH_DEVICES=""
PASSTHROUGH_DEVICES=""

source passthrough_devices.sh

for dev_mmiobase in ${PASSTHROUGH_DEVICES}; do
	QEMU_PASSTHROUGH_DEVICES=\
		${QEMU_PASSTHROUGH_DEVICES} \
		 -device vfio-platform,host=${dev_mmiobase}
done

exec /home/ubuntu/qemu/build/qemu-system-aarch64 \
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
	-append "rootwait root=/dev/vda console=hvc0 "${EARLYCON}" clk_ignore_unused pd_ignore_unused" \
	`# enable multiplexer on stdio to have both guest and monitor` \
	-chardev stdio,id=mon,mux=on,signal=off \
	`# enable monitor` \
	-mon chardev=mon,mode=readline \
	-serial chardev:mon \
	`# virtio-console` \
	-device virtio-serial-pci \
	-device virtconsole,chardev=mon,id=console0,name=guestvm \
	`# passthrough devices` \
	${QEMU_PASSTHROUGH_DEVICES}
