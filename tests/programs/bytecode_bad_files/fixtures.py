#!/usr/bin/env python3
"""makes the crafted .miab files of this test (the format is in include/mia/bytecode.h):
a build of fixtures.mia with one part changed per file, which mia must refuse.

	mia build -o good.miab fixtures.mia
	python3 fixtures.py good.miab .

when the format changes (mia_bytecode_version), build again and run this again."""
import os
import sys


class Reader:
	def __init__(self, data):
		self.data, self.at = data, 0

	def take(self, n):
		value = self.data[self.at:self.at + n]
		self.at += n
		return value

	def u8(self):
		return self.take(1)[0]

	def u(self):
		"""an unsigned varint, and where its bytes are."""
		start, value, shift = self.at, 0, 0
		while True:
			byte = self.u8()
			value |= (byte & 0x7F) << shift
			shift += 7
			if not byte & 0x80:
				return value, (start, self.at)

	def v(self):
		return self.u()[0]

	def s(self):
		value, place = self.u()
		return (value >> 1) ^ -(value & 1), place

	def string(self):
		return self.take(self.v())


def varint(value):
	out = bytearray()
	while value >= 0x80:
		out.append((value & 0x7F) | 0x80)
		value >>= 7
	out.append(value)
	return bytes(out)


def zigzag(value):
	return varint(value << 1 if value >= 0 else ((-value) << 1) - 1)


# the type codes and operands of each opcode (opcodes.def, in order)
OPCODES = []
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'include', 'mia', 'opcodes.def')):
	if line.startswith('mia_opcode('):
		parts = line.split('(', 1)[1].split(',')
		OPCODES.append((int(parts[1]), int(parts[2].strip(' )\n'))))


def parse(data):
	"""where the parts a variant changes are (start and end of their bytes)."""
	r = Reader(data)
	assert r.take(4) == b'MIA\0'
	r.take(4)
	places = {'types': [], 'slots': [], 'files_count': r.at}
	for _ in range(r.v()):
		r.string()
	for _ in range(r.v()):
		name = r.string()
		kind, code_at = r.u8(), r.at
		r.u8()
		size, _ = r.u()
		r.v()
		r.v()
		_, length_place = r.s()
		r.s()
		fields = []
		for _ in range(r.v()):
			r.string()
			r.v()
			fields.append(r.u()[1])
		places['types'].append(dict(name=name, kind=kind, code_at=code_at, size=size, length=length_place, fields=fields))

	def constant():
		kind = r.u8()
		r.s()
		if kind == 1:
			signed = r.u8()
			r.s() if signed else r.u()
		elif kind == 2:
			r.take(8)
		elif kind == 3:
			r.u8()
		elif kind == 4:
			r.string()
		elif kind in (6, 8):
			r.v()
		elif kind == 7:
			r.u8()
			r.u8()
			r.s()
			r.s()
		if kind in (5, 7):
			for _ in range(r.v()):
				constant()
	for _ in range(r.v()):
		constant()
	for _ in range(r.v()):
		r.string()
		r.string()
		r.s()
		counts = [r.v() for _ in range(3)]
		for count in counts:
			for _ in range(count):
				r.v()
		for _ in range(r.v()):
			r.v()
	for _ in range(r.v()):
		name = r.string()
		r.v()
		r.v()
		r.v()
		r.u8()
		for _ in range(r.v()):
			r.u8()
			places['slots'].append(dict(function=name, offset=r.u()[1]))
			r.v()
		code = r.v()
		for _ in range(code):
			types, operands = OPCODES[r.u8()]
			r.take(types)
			for _ in range(operands):
				r.s()
		for _ in range(code):
			changed = r.u8()
			for bit in (1, 2, 4):
				if changed & bit:
					r.s()
			if changed & 8:
				r.v()
	return places


def splice(data, place, new):
	start, end = place
	return data[:start] + new + data[end:]


good = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
places = parse(good)
variants = {}
struct_type = next(t for t in places['types'] if t['kind'] == 5 and t['fields'])
scalar_type = next(t for t in places['types'] if t['kind'] == 0)
array_type = next(t for t in places['types'] if t['kind'] == 3)
variants['field_offset_wraps'] = splice(good, struct_type['fields'][-1], varint((1 << 64) - 8))
variants['field_offset_outside'] = splice(good, struct_type['fields'][-1], varint(struct_type['size']))
variants['scalar_code_bytes'] = splice(good, (scalar_type['code_at'], scalar_type['code_at'] + 1), bytes([16]))
variants['array_too_long'] = splice(good, array_type['length'], zigzag(1 << 40))
variants['slot_offset_wraps'] = splice(good, places['slots'][0]['offset'], varint((1 << 64) - 4))
variants['truncated'] = good[:len(good) // 2]
variants['wrong_version'] = good[:4] + bytes([99, 0, 0, 0]) + good[8:]
files_count = Reader(good)
files_count.at = places['files_count']
place = files_count.u()[1]
variants['bad_section'] = splice(good, place, varint(0xFFFFFFF0))
variants['number_too_long'] = splice(good, place, b'\xff' * 10 + b'\x01')
for name, data in variants.items():
	open(os.path.join(out, name + '.miab'), 'wb').write(data)
	print(name)
