# Build QEMU on NVIDIA Orin AGX

## Install build dependencies

    sudo apt update
    sudo apt dist-upgrade
    sudo apt install -y \
        python3-tomli \
        python3-venv

    sudo apt-get install -y \
        debhelper-compat \
        python3 \
        ninja-build \
        meson \
        texinfo \
        python3-sphinx \
        python3-sphinx-rtd-theme \
        libaio-dev \
        libjack-dev \
        libpulse-dev \
        libasound2-dev \
        libbrlapi-dev \
        libcap-ng-dev \
        libcurl4-gnutls-dev \
        libfdt-dev \
        libfuse3-dev \
        gnutls-dev \
        libgtk-3-dev \
        libvte-2.91-dev \
        libiscsi-dev \
        libncurses-dev \
        libvirglrenderer-dev \
        libepoxy-dev \
        libdrm-dev \
        libgbm-dev \
        libnuma-dev \
        libcacard-dev \
        libpixman-1-dev \
        librbd-dev \
        libglusterfs-dev \
        libsasl2-dev \
        libsdl2-dev \
        libseccomp-dev \
        libslirp-dev \
        libspice-server-dev \
        librdmacm-dev \
        libibverbs-dev \
        libibumad-dev \
        liburing-dev \
        libusb-1.0-0-dev \
        libusbredirparser-dev \
        libssh-dev \
        libzstd-dev \
        nettle-dev \
        uuid-dev \
        xfslibs-dev \
        zlib1g-dev \
        libudev-dev \
        libjpeg-dev \
        libpng-dev \
        libpmem-dev

## Check out sources

    git clone https://github.com/hlyytine/qemu.git
    cd qemu
    git checkout gpuvm
    git submodule update --init --recursive

## Build QEMU

    ./configure --target-list=aarch64-softmmu
    make -j`nproc`

## Adding UART A getty to guest

    mkdir mnt
    sudo modprobe nbd
    sudo ./qemu/build/qemu-nbd -c /dev/nbd0 rootfs-guest.qcow2
    sudo mount /dev/nbd0 mnt
    sudo install -o root -g root -m 0644 stty-ttyTHS1.service mnt/etc/systemd/system/stty-ttyTHS1.service
    sudo mnt/etc/systemd/system/serial-getty@ttyTHS1.service.wants
    sudo ln -s /etc/systemd/system/stty-ttyTHS1.service mnt/etc/systemd/system/serial-getty@ttyTHS1.service.wants
    sudo umount mnt
    sudo ./qemu/build/qemu-nbd -d /dev/nbd0

