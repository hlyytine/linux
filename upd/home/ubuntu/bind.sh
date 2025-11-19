#! /bin/sh

PASSTHROUGH_DEVICES=""

source passthrough_devices.sh

for dev_mmiobase in ${PASSTHROUGH_DEVICES}; do
  dev=`echo ${dev_mmiobase} | cut -f1 -d,`
  echo vfio-platform > /sys/bus/platform/devices/${dev}/driver_override
  echo ${dev} > /sys/bus/platform/drivers/vfio-platform/bind
done
