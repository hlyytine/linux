from pathlib import PurePosixPath

import io
import libfdt
import struct

class LiteralProperty(object):
    def __init__(self, name, value=None):
        self.name = name
        self.value = value

    def __str__(self):
        if self.value:
            return "%s = %s;" % (self.name, self.value)
        else:
            return "%s;" % self.name;


class Property(libfdt.Property):
    def __init__(self, name, value):
        if isinstance(value, str):
            value = bytearray(value + "\000", 'utf-8')
        super().__init__(name, value)

    def __str__(self):
        if len(self) == 0:
            return '%s;' % self.name

        return '%s = %s;' % (self.name, self.value_to_str())

    def cell_to_str(self, name, ncells):
        if self.name != name:
            return None

        if len(self) % (ncells * 4) != 0:
            raise ValueError('unexpected number of bytes')

        ints = struct.unpack(f'>{len(self)//4}I', self)

        p = 0
        strs = []
        while p < len(self) // 4:
            strs.append(' '.join(f'0x{x:x}' for x in ints[p:p+ncells]))
            p += ncells

        return ', '.join('<%s>' % x for x in strs)

    def value_to_str(self):
        if len(self) == 0:
            formatted = ''

        formatted = self.cell_to_str('reg', 4)
        if formatted:
            return formatted

        formatted = self.cell_to_str('interrupts', 3)
        if formatted:
            return formatted

        formatted = self.cell_to_str('clocks', 2)
        if formatted:
            return formatted

        formatted = self.cell_to_str('resets', 2)
        if formatted:
            return formatted

        formatted = self.cell_to_str('allowed-clocks', 1)
        if formatted:
            return formatted

        formatted = self.cell_to_str('allowed-resets', 1)
        if formatted:
            return formatted

        elif self.endswith(b'\x00') and b'\x00' in self[:-1]:
            strings = self.rstrip(b'\x00').split(b'\x00')
            formatted = ', '.join(f'"{s.decode()}"' for s in strings)

        elif self.endswith(b'\x00'):
            formatted = f'"{self.rstrip(b"\x00").decode()}"'

        elif len(self) % 4 == 0:
            ints = struct.unpack(f'>{len(self)//4}I', self)
            formatted = ', '.join(f'<0x{x:x}>' for x in ints)

        else:
            formatted = '[' + ' '.join(f'{x:02x}' for x in self) + ']'

        return formatted

class Node(object):
    def __init__(self, name):
        self.name = name
        self.children = []
        self.props = {}

    def walk_props(self, overlay):
        for prop_name, prop_value in self.props.items():
            self.props[prop_name] = overlay.handleProperty(prop_value)

    def finalize_node(self, overlay):
         overlay.finalize_node(self)

    def add_property(self, name, value):
        self.props[name] = Property(name, value)

    def add_property_u32_list(self, name, value):
        self.props[name] = Property(name, bytearray(struct.pack('>' + 'I' * len(value), *value)))

    def generate_output(self, ostream):
        child_ostream = io.StringIO()

        print('%s {' % self.name, file=ostream)

        for prop_name, prop_value in self.props.items():
            if prop_value:
                print('%s' % prop_value, file=child_ostream)

        for child in self.children:
            child.generate_output(child_ostream)

        child_ostream.seek(0)
        for line in child_ostream:
            print("\t%s" % line.rstrip(), file=ostream)

        print('};', file=ostream)

    def __iter__(self):
        yield self
        for child in self.children:
            yield from child

#def full_nodename(fdt, offset):
#    name = fdt.get_name(offset)
#    try:
#        parent_offset = fdt.parent_offset(offset)
#        return full_nodename(fdt, parent_offset) + '/' + name
#
#    except libfdt.FdtException as e:
#        if e.err == -libfdt.FDT_ERR_NOTFOUND:
#            return ''
#        # TODO: add raise here
#
#def full_parentname(fdt, offset):
#    return str(PurePosixPath(full_nodename(fdt, offset)).parent)

def find_node_by_name(fdt, name, offset=0):
    offset = fdt.first_subnode(offset)
    while offset >= 0:
        if fdt.get_name(offset) == name:
            return offset
        try:
            sub = find_node_by_name(fdt, name, offset)
            if sub:
                return sub
        except libfdt.FdtException as e:
            if e == libfdt.FDT_ERR_NOTFOUND:
                pass
        offset = fdt.next_subnode(offset, libfdt.QUIET_NOTFOUND)
    return None

class FdtNode(Node):
    def __init__(self, fdt, offset, parent_fullpath=None):
        super().__init__(fdt.get_name(offset))
        self.fdt = fdt
        self.offset = offset

        self.parent_fullpath = parent_fullpath

        prop_offset = self.fdt.first_property_offset(self.offset, libfdt.QUIET_NOTFOUND)
        while prop_offset >= 0:
            prop = self.fdt.get_property_by_offset(prop_offset)
            self.props[prop.name] = Property(prop.name, prop)
            prop_offset = self.fdt.next_property_offset(prop_offset, libfdt.QUIET_NOTFOUND)

        if parent_fullpath:
            parent_fullpath = parent_fullpath + '/' + self.name
        else:
            parent_fullpath = '/'

        subnode_offset = fdt.first_subnode(self.offset, libfdt.QUIET_NOTFOUND)
        while subnode_offset >= 0:
            child = FdtNode(self.fdt, subnode_offset, parent_fullpath)
            self.children.append(child)
            subnode_offset = self.fdt.next_subnode(subnode_offset, libfdt.QUIET_NOTFOUND)
