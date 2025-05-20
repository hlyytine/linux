#! /bin/sh

GPUVM=
UARTA=1

echo vfio-platform > /sys/bus/platform/devices/80000000.vm_cma_p/driver_override

if test "x${GPUVM}" != "x"; then
  echo vfio-platform > /sys/bus/platform/devices/17000000.gpu/driver_override
  echo vfio-platform > /sys/bus/platform/devices/13e00000.host1x_pt/driver_override
  echo vfio-platform > /sys/bus/platform/devices/15340000.vic/driver_override
  echo vfio-platform > /sys/bus/platform/devices/15480000.nvdec/driver_override
  echo vfio-platform > /sys/bus/platform/devices/15540000.nvjpg/driver_override
  echo vfio-platform > /sys/bus/platform/devices/d800000.dce/driver_override
  echo vfio-platform > /sys/bus/platform/devices/13800000.display/driver_override
  echo vfio-platform > /sys/bus/platform/devices/100000000.vm_cma_vram_p/driver_override
  echo vfio-platform > /sys/bus/platform/devices/60000000.vm_hs_p/driver_override
fi

if test "x${UARTA}" != "x"; then
  echo vfio-platform > /sys/bus/platform/devices/3100000.serial/driver_override
fi

echo 80000000.vm_cma_p > /sys/bus/platform/drivers/vfio-platform/bind

if test "x${GPUVM}" != "x"; then

  echo 17000000.gpu > /sys/bus/platform/drivers/vfio-platform/bind
  echo 13e00000.host1x_pt > /sys/bus/platform/drivers/vfio-platform/bind
  echo 15340000.vic > /sys/bus/platform/drivers/vfio-platform/bind
  echo 15480000.nvdec > /sys/bus/platform/drivers/vfio-platform/bind
  echo 15540000.nvjpg > /sys/bus/platform/drivers/vfio-platform/bind
  echo d800000.dce > /sys/bus/platform/drivers/vfio-platform/bind
  echo 13800000.display > /sys/bus/platform/drivers/vfio-platform/bind
  echo 100000000.vm_cma_vram_p > /sys/bus/platform/drivers/vfio-platform/bind
  echo 60000000.vm_hs_p > /sys/bus/platform/drivers/vfio-platform/bind
fi

if test "x${UARTA}" != "x"; then
  echo 3100000.serial > /sys/bus/platform/drivers/vfio-platform/bind
fi

