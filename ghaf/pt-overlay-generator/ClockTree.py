#! /usr/bin/python

import re

class ClockTree:
    def __init__(self):
        self.clk_names = {}
        self.clk_parents = {}

        current_clk = -1
        num_parents = 0

        with open('tegra234-clks.txt', 'r') as file:
            lines = [line.strip() for line in file]

        for line in lines:
            clock_name = re.search(r'^(\d+):\s+(\S+)$', line)
            clock_flags = re.search(r'^flags:\s+(\S+).*$', line)
            clock_num_parents = re.search(r'^parents:\s+(\d+)$', line)
            clock_parent_clk = re.search(r'^(\d+)$', line)

            if clock_name:
                current_clk = int(clock_name.group(1))
                self.clk_names[current_clk] = clock_name.group(2)
                self.clk_parents[current_clk] = []
#            elif clock_flags:
#                print('flags: %d' % int(clock_flags.group(1)))
            elif clock_num_parents:
                num_parents = int(clock_num_parents.group(1))
            elif clock_parent_clk:
                num_parents = num_parents - 1
                if num_parents < 0:
                    raise ValueError('inconsistent input')
                self.clk_parents[current_clk].append(int(clock_parent_clk.group(1)))

    def required_for_clocks(self, clks):
        clks_next = clks
        clks = []

        while clks != clks_next:
            clks = clks_next
            clks_next = []
            for clk in clks:
                clks_next.append(clk)
                if clk in self.clk_parents:
                    clks_next.extend(self.clk_parents[clk])

            clks_next = list(dict.fromkeys(clks_next))

        return clks_next
