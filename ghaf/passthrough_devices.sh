# add your passthrough devices here

# DO NOT add any passthrough devices after this line,
# or delicate address mapping in QEMU will get messed up

# this is needed for host/guest virtio buffers
PASSTHROUGH_DEVICES="80000000.vm_cma_p,mmio-base=0x80000000 ${PASSTHROUGH_DEVICES}"
