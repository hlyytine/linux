import libfdt

from Overlay import Overlay
import ClockTree
import FdtHelper

class HostOverlay(Overlay):
    """Host device tree overlay generator"""

    def __init__(self, dtb_path, output_filename):
        super().__init__(dtb_path, "Host overlay", output_filename)

        self.required_resets = []
        self.required_clocks = []


    def handleProperty(self, prop):
        if prop.name == 'compatible':
            return FdtHelper.Property('compatible', 'nvidia,dummy')

        if prop.name == 'resets':
            resets = prop.as_uint32_list()
            # FIXME: we'll just assume bpmp's phandle is 0x03
            bpmp_phandle = resets.pop(0)
            if bpmp_phandle != 0x03:
                raise ValueError('BPMP phandle %d is not 0x03' % bpmp_phandle)
            self.required_resets.extend(resets)
            return None

        if prop.name == 'clocks':
            clocks = prop.as_uint32_list()
            # FIXME: we'll just assume bpmp's phandle is 0x03
            bpmp_phandle = clocks.pop(0)
            if bpmp_phandle != 0x03:
                raise ValueError('BPMP phandle %d is not 0x03' % bpmp_phandle)
            self.required_clocks.extend(clocks)
            return None

        if prop.name == 'status':
            return FdtHelper.Property('status', 'okay')

        return None

    def finalize_node(self, node):
        # kind of hack to pass it as string
        node.props['iommus'] = FdtHelper.LiteralProperty('iommus', '<&smmu_niso0 TEGRA234_SID_PASSTHROUGH>')
        node.props['dma-coherent'] = FdtHelper.LiteralProperty('dma-coherent')

    def generate_bpmp_node(self):
        self.required_resets = list(dict.fromkeys(self.required_resets))
        self.required_clocks = list(dict.fromkeys(self.required_clocks))

        bpmp_proxy_node = FdtHelper.Node('bpmp_host_proxy')

        guest_bpmp = FdtHelper.Node('guest_bpmp')
        bpmp_proxy_node.children.append(guest_bpmp)

        guest_bpmp.add_property_u32_list('allowed-clocks', self.required_clocks)
        guest_bpmp.add_property_u32_list('allowed-resets', self.required_resets)

        # TODO: add allowed power domains

        bpmp_proxy_node.add_property('compatible', 'nvidia,bpmp-host-proxy')
        bpmp_proxy_node.add_property('status', 'okay')

        self.root.children.append(bpmp_proxy_node)

        return bpmp_proxy_node
