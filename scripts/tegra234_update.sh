#! /bin/sh

mkdir -p upd/boot/dtb
mkdir -p upd/home/ubuntu

# host files
cp arch/arm64/boot/Image upd/boot/Image
cp arch/arm64/boot/dts/nvidia/tegra234-p3737-0000+p3701-0000.dtb upd/boot/dtb/tegra234-p3737-0000+p3701-0000.dtb
cp arch/arm64/boot/dts/nvidia/tegra234-host-bpmp-proxy.dtbo upd/boot/tegra234-host-bpmp-proxy.dtbo
cp arch/arm64/boot/dts/nvidia/tegra234-host-uarta-passthrough.dtbo upd/boot/tegra234-host-uarta-passthrough.dtbo

# guest files
cp arch/arm64/boot/Image upd/home/ubuntu/Image
cp arch/arm64/boot/dts/nvidia/tegra234-qemu-4cpus-4gb.dtb upd/home/ubuntu/tegra234-qemu-4cpus-4gb.dtb

cp ghaf/bind.sh upd/home/ubuntu/bind.sh
cp ghaf/launch.sh upd/home/ubuntu/launch.sh
cp ghaf/passthrough_devices.sh upd/home/ubuntu/passthrough_devices.sh

# UART A
cp ghaf/stty-ttyTHS1.service upd/home/ubuntu/stty-ttyTHS1.service

tar -C upd -cf - . | ssh ubuntu@192.168.101.112 'cat > update.tar'
ssh ubuntu@192.168.101.112 'touch do_update'
