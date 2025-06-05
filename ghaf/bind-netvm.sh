#! /bin/sh

echo vfio-platform > /sys/bus/platform/devices/b0000000.vm_cma_net_p/driver_override

echo b0000000.vm_cma_net_p > /sys/bus/platform/drivers/vfio-platform/bind
