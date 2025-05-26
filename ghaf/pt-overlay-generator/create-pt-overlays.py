#! /usr/bin/python

from pathlib import PurePosixPath

import libfdt
import FdtHelper

from HostOverlay import HostOverlay
from GuestOverlay import GuestOverlay

#path_prefix = "../arch/arm64/boot/dts/nvidia/"
#dtb_path = path_prefix + "tegra234-p3737-0000+p3701-0000.dtb"

dtb_path = "tegra234-p3737-0000+p3701-0000-nv.dtb"


dtb_filename = PurePosixPath(dtb_path)
stem = dtb_filename.stem
host_dtso_filename = dtb_filename.with_name(f"{stem}-host").with_suffix(".dtso")
guest_dtso_filename = dtb_filename.with_name(f"{stem}-guest").with_suffix(".dtso")

host_overlay = HostOverlay(dtb_path, host_dtso_filename)

passthroughNodeNames = ['serial@3100000', 'serial@3140000']

allNodes = list(host_overlay.root)
passthroughNodes = [node for node in allNodes if node.name in passthroughNodeNames]

# TODO: add error checks

for node in passthroughNodes:
    node.walk_props(host_overlay)

for node in passthroughNodes:
    node.finalize_node(host_overlay)

bpmp_host_proxy_node = host_overlay.generate_bpmp_node()

host_overlay.generate_header()

host_overlay.generate_fragment_for_nodes('/bus@0', passthroughNodes)
host_overlay.generate_fragment_for_nodes('/', [bpmp_host_proxy_node])
host_overlay.generate_footer()

guest_overlay = GuestOverlay(dtb_path, guest_dtso_filename)
allNodes = list(guest_overlay.root)
passthroughNodes = [node for node in allNodes if node.name in passthroughNodeNames]

for node in passthroughNodes:
    node.walk_props(guest_overlay)

for node in passthroughNodes:
    node.finalize_node(guest_overlay)

guest_overlay.generate_header()
guest_overlay.generate_fragment_for_nodes('/bus@0', passthroughNodes)
guest_overlay.generate_footer()
