#!/usr/bin/env python3
"""makes the crafted .miab files of this test (the format is in include/mia/bytecode.h):
a build of fixtures.mia with one layout changed per file, which the machine must refuse.

	mia build -o good.miab fixtures.mia
	python3 fixtures.py good.miab .

when the format changes (mia_bytecode_version), build again and run this again."""
import os, struct, sys


class Reader:
	def __init__(self, data):
		self.data, self.at = data, 0

	def take(self, n):
		value = self.data[self.at:self.at + n]
		self.at += n
		return value

	def u8(self): return self.take(1)[0]
	def u32(self): return struct.unpack('<I', self.take(4))[0]
	def u64(self): return struct.unpack('<Q', self.take(8))[0]
	def s64(self): return struct.unpack('<q', self.take(8))[0]
	def string(self): return self.take(self.u32())


def parse(data):
	"""the offsets of the fields a variant changes."""
	r = Reader(data)
	assert r.take(4) == b'MIA\0'
	r.u32()
	places = {'types': [], 'slots': [], 'constants': []}
	for _ in range(r.u32()): r.string()
	for _ in range(r.u32()):
		start = r.at
		name = r.string()
		kind_at = r.at
		kind, code = r.u8(), r.u8()
		size_at = r.at
		size, alignment, element, length_at = r.u64(), r.u64(), r.u32(), r.at
		length, zero = r.s64(), r.s64()
		fields = []
		for _ in range(r.u32()):
			r.string()
			field_type = r.u32()
			fields.append((r.at, field_type))
			r.u64()
		places['types'].append(dict(name=name, kind=kind, kind_at=kind_at, code_at=kind_at + 1, size=size, size_at=size_at, length_at=length_at, fields=fields))
	def constant():
		start = r.at
		kind = r.u8()
		r.s64()
		info = dict(kind=kind, at=start)
		if kind == 1: r.u64(); r.u8()
		elif kind == 2: r.u64()
		elif kind == 3: r.u8()
		elif kind == 4: r.string()
		elif kind in (6, 8): r.u64()
		elif kind == 7:
			r.u8(); r.u8(); info['start_at'] = r.at; r.s64(); r.s64()
		if kind in (5, 7):
			info['items'] = [constant() for _ in range(r.u32())]
		return info
	for _ in range(r.u32()): places['constants'].append(constant())
	for _ in range(r.u32()):
		r.string(); r.string(); r.s64()
		counts = [r.u32() for _ in range(3)]
		for count in counts:
			for _ in range(count): r.u32()
		for _ in range(r.u32()): r.u32()
	for _ in range(r.u32()):
		name = r.string()
		r.u32(); frame_at = r.at; r.u64(); r.u32(); r.u8()
		for _ in range(r.u32()):
			r.u8()
			places['slots'].append(dict(function=name, offset_at=r.at))
			r.u64(); r.u64()
		code = r.u32()
		for _ in range(code):
			opcode = r.u8(); r.u8(); r.u8()
			# operands: as many as the opcode takes (read from the next instruction's position is not possible): skip by table
			r.take(8 * OPERANDS[opcode])
		r.take(16 * code)
	return places


# the operand counts of opcodes.def, in order
OPERANDS = []
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'include', 'mia', 'opcodes.def')):
	if line.startswith('mia_opcode('):
		OPERANDS.append(int(line.split(',')[2].strip(' )\n')))


def patch(data, at, packed):
	return data[:at] + packed + data[at + len(packed):]


good = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
places = parse(good)
variants = {}
struct_types = [t for t in places['types'] if t['kind'] == 5 and t['fields']]
scalar_types = [t for t in places['types'] if t['kind'] == 0]
array_types = [t for t in places['types'] if t['kind'] == 3]
if struct_types:
	t = struct_types[0]
	variants['field_offset_wraps'] = patch(good, t['fields'][-1][0], struct.pack('<Q', (1 << 64) - 8))
	variants['field_offset_outside'] = patch(good, t['fields'][-1][0], struct.pack('<Q', t['size']))
if scalar_types:
	t = scalar_types[0]
	variants['scalar_code_slice'] = patch(good, t['code_at'], bytes([15]))
	variants['scalar_code_bytes'] = patch(good, t['code_at'], bytes([16]))
if array_types:
	t = array_types[0]
	variants['array_too_long'] = patch(good, t['length_at'], struct.pack('<q', 1 << 40))
if places['slots']:
	variants['slot_offset_wraps'] = patch(good, places['slots'][0]['offset_at'], struct.pack('<Q', (1 << 64) - 4))
slices = []
def find_slices(items):
	for item in items:
		if item['kind'] == 7: slices.append(item)
		find_slices(item.get('items', []))
find_slices(places['constants'])
if slices:
	at = slices[0]['start_at']
	variants['constant_slice_overflow'] = patch(patch(good, at, struct.pack('<q', (1 << 62))), at + 8, struct.pack('<q', (1 << 62)))
kept = ('field_offset_wraps', 'field_offset_outside', 'scalar_code_bytes', 'slot_offset_wraps', 'array_too_long')
for name, data in variants.items():
	if name not in kept:
		continue
	open(os.path.join(out, name + '.miab'), 'wb').write(data)
	print(name)
