#! /usr/bin/python

import re

clk_names = {}
clk_parents = {}

current_clk = -1
num_parents = 0
parent_list = []

with open('tegra234-clks.txt', 'r') as file:
    for str in file:
        str = str.strip()
        match = re.search(r'^(\d+):\s+(\S+)$', str)
        if match:
            current_clk = int(match.group(1))
            clk_names[current_clk] = match.group(2)
        else:
            match = re.search(r'^flags:\s+(\S+).*$', str)
            if match:
                print('flags: %d' % int(match.group(1)))
            else:
                match = re.search(r'^parents:\s+(\d+)$', str)
                if match:
                    num_parents = int(match.group(1))
                    parent_list = []
                else:
                    match = re.search(r'^(\d+)$', str)
                    num_parents = num_parents - 1
                    if num_parents < 0:
                        raise ValueError('inconsistent input')
                    parent_list.append(int(match.group(1)))
                    clk_parents[current_clk] = parent_list

old_list = [155]

while True:
    new_list = []
    for clk in old_list:
        new_list.append(clk)
        if clk in clk_parents:
            new_list.extend(clk_parents[clk])

    new_list = list(dict.fromkeys(new_list))

#    print(new_list)

    if old_list == new_list:
        break

    old_list = new_list

for clk in new_list:
    print('%d\t%s', clk, clk_names[clk])


#for id in clk_names.keys():
#    print('%d: %s' % (id, clk_names[id]))


