import libfdt

from Overlay import Overlay
import ClockTree
import FdtHelper

class GuestOverlay(Overlay):
    """Guest device tree overlay generator"""

    def __init__(self, dtb_path, output_filename):
        super().__init__(dtb_path, "Guest overlay", output_filename)

    def handleProperty(self, prop):
        if prop.name == 'status':
            return FdtHelper.Property('status', 'okay')

        return prop

    def finalize_node(self, node):
        pass
