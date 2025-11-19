# add your passthrough devices here

# UART A
PASSTHROUGH_DEVICES="${PASSTHROUGH_DEVICES} 3100000.serial"

# DO NOT add any passthrough devices after this line,
# or delicate address mapping in QEMU will get messed up

# this is needed for host/guest virtio buffers
PASSTHROUGH_DEVICES=\
	`# shared memory between host and guest starts here` \
	`# virtio ` \
	80000000.vm_cma_p,mmio-base=0x80000000 \
	`# ---- shared memory between host and guest ----` \
	${PASSTHROUGH_DEVICES}
