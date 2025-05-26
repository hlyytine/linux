import io
import libfdt

import FdtHelper

class Overlay(object):
    """Device tree overlay generator"""

    def __init__(self, dtb_path, overlay_name, output_filename):
        self.overlay_name = overlay_name

        self.num_fragments = 0
        self.outf = open(output_filename, 'w')

        with open(dtb_path, "rb") as f:
            self.dtb = f.read()

        fdt = libfdt.Fdt(self.dtb)

        self.root = FdtHelper.FdtNode(fdt, 0)

    def generate_header(self):
        print("""/dts-v1/;
/plugin/;

#include <dt-bindings/clock/tegra234-clock.h>
#include <dt-bindings/reset/tegra234-reset.h>
#include <dt-bindings/power/tegra234-powergate.h>
#include <dt-bindings/memory/tegra234-mc.h>
#include <dt-bindings/interrupt-controller/irq.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

/ {
        overlay-name = "%s";
        compatible = "nvidia,tegra234";""" % self.overlay_name, file=self.outf)

    def start_fragment(self):
        return io.StringIO()

    def generate_fragment_for_nodes(self, target_path, nodes):
        child_ostream = io.StringIO()

        for node in nodes:
            node.generate_output(child_ostream)

        child_ostream.seek(0)

        print("\n\tfragment@%d {" % self.num_fragments, file=self.outf)
        print("\t\ttarget-path = \"%s\";" % target_path, file=self.outf)
        print("\t\t__overlay__ = {", file=self.outf)
        for line in child_ostream:
            print("\t\t\t%s" % line.rstrip(), file=self.outf)
        print("\t\t};", file=self.outf)
        print("\t};", file=self.outf)

        self.num_fragments += 1

    def __del__(self):
        self.outf.close()

    def generate_footer(self):
        print("}", file=self.outf)
