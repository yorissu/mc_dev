
"""differential tests: runs programs through the c and the python
implementation and compares what they print.

usage (from the c directory): python3 tests/differential.py <mc binary>
	[--commands tokens,ast,check,mono,dis,run] [--mutations N] [--seed S] [--only TEXT] [--verbose]

'run' compares what the programs print, their exit status and the message
of a runtime error (the c implementation reports the calls more compactly);
both stop after LIMIT instructions, so mutated programs that never finish
compare too.

the programs come from the python test suites, the examples and the
standard library, plus mutated copies of them (deleted and inserted text),
so error paths are compared too. exit status: 0 when everything matches."""

import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
C_ROOT = os.path.dirname(HERE)
PYTHON_ROOT = os.path.dirname(C_ROOT)
PRELUDE = 'from io import print, println, write, write_line\nfrom memory import alloc, realloc, free, copy\n'
LIMIT = 200000
PIECES = [' + 1', ' - 1', ' * 2', '[0]', '[..]', '[1..]', '; return 0;', ' x', '0', '1', '-1', '255', 'true', '!',
	'(int)', '(u8)', ' break;', ' continue;', '{', '}', '(', ')', ';', ',', '&', 'mut &', '::', '<', '>', '.',
	'..', 'let', '"s"', '{}', 'null', '1.5', '[op._+_]', '...']


def programs():
	"""(name, source) of every program the comparison uses."""
	found = []
	for name in sorted(os.listdir(os.path.join(PYTHON_ROOT, 'tests'))):
		if not name.startswith('test_') or not name.endswith('.py'):
			continue
		text = open(os.path.join(PYTHON_ROOT, 'tests', name), encoding='utf-8').read()
		prelude = PRELUDE if name == 'test_runtime.py' else ''
		count = 0
		for match in re.finditer(r"'''(.*?)'''", text, re.S):
			found.append((f'{name}:{count}', prelude + match.group(1)))
			count += 1
		for match in re.finditer(r"'((?:[^'\\\n]|\\.)*::(?:[^'\\\n]|\\.)*)'", text):
			try:
				source = bytes(match.group(1), 'utf-8').decode('unicode_escape')
			except UnicodeDecodeError:
				continue
			found.append((f'{name}:{count}', prelude + source))
			count += 1
	for directory in ('examples/tour', 'examples/vector', 'stdlib'):
		for name in sorted(os.listdir(os.path.join(PYTHON_ROOT, directory))):
			if name.endswith('.mc'):
				found.append((f'{directory}/{name}', open(os.path.join(PYTHON_ROOT, directory, name), encoding='utf-8').read()))
	return found


def mutate(source, generator):
	for _ in range(generator.randint(1, 3)):
		position = generator.randint(0, len(source))
		if generator.random() < 0.5:
			source = source[:position] + source[min(len(source), position + generator.randint(1, 8)):]
		else:
			source = source[:position] + generator.choice(PIECES) + source[position:]
	return source


def run(command, cwd, timeout=60):
	try:
		result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout,
			env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
		return result.returncode, result.stdout.decode('utf-8', 'replace'), result.stderr.decode('utf-8', 'replace')
	except subprocess.TimeoutExpired:
		return 'timeout', '', ''


def compare(binary, command, path, verbose):
	"""none when both implementations agree, otherwise a description."""
	options = ['--format', 'short'] if command in ('check', 'run', 'mono', 'dis') else []
	if command == 'run':
		options += ['--limit', str(LIMIT)]
	if command in ('check', 'mono', 'run', 'dis'):
		needs_main = 'main ::' not in open(path, encoding='utf-8').read()
		if needs_main:
			with open(path, 'a', encoding='utf-8') as file:
				file.write('\npub main :: () {}\n')
	python = run([sys.executable, '-m', 'mc', command, '--no-color', *options, path], PYTHON_ROOT)
	c = run([os.path.abspath(binary), command, '--no-color', *options, path], C_ROOT)
	differences = []
	if python[0] != c[0]:
		differences.append(f'exit status: python {python[0]}, c {c[0]}')
	if python[1] != c[1]:
		differences.append('stdout differs:\n' + diff(python[1], c[1]))
	if command in ('check', 'mono', 'dis') or (command == 'run' and python[0] != 101):
		if python[2] != c[2]:
			differences.append('stderr differs:\n' + diff(python[2], c[2]))
	elif command == 'run' and runtime_message(python[2]) != runtime_message(c[2]):
		differences.append('runtime errors differ:\n' + diff(python[2], c[2]))
	if command in ('ast', 'tokens') and ('error[' in python[2]) != ('error[' in c[2]):
		differences.append('errors differ:\n' + diff(python[2], c[2]))
	if 'AddressSanitizer' in c[2] or 'LeakSanitizer' in c[2] or re.search(r'\.c:\d+:\d+: runtime error:', c[2]):
		differences.append('c crashed:\n' + c[2][-3000:])
	return '\n'.join(differences) if differences else None


def runtime_message(stderr):
	"""the message of the first runtime error ('runtime error: <message>')."""
	match = re.search(r'runtime error: (.*)', stderr)
	return match.group(1) if match else None


def diff(expected, actual, limit=30):
	import difflib
	lines = list(difflib.unified_diff(expected.splitlines(), actual.splitlines(), 'python', 'c', lineterm='', n=2))
	return '\n'.join(lines[:limit]) + ('\n...' if len(lines) > limit else '')


def main(arguments):
	if not arguments:
		sys.stderr.write(__doc__)
		return 2
	binary = arguments[0]
	commands = ['tokens', 'ast', 'check', 'mono', 'dis', 'run']
	mutations, seed, verbose, only = 2, 1, False, None
	index = 1
	while index < len(arguments):
		if arguments[index] == '--commands':
			commands = arguments[index + 1].split(',')
			index += 1
		elif arguments[index] == '--mutations':
			mutations = int(arguments[index + 1])
			index += 1
		elif arguments[index] == '--seed':
			seed = int(arguments[index + 1])
			index += 1
		elif arguments[index] == '--only':
			only = arguments[index + 1]
			index += 1
		elif arguments[index] == '--verbose':
			verbose = True
		index += 1
	generator = random.Random(seed)
	cases = []
	for name, source in programs():
		cases.append((name, source))
		for number in range(mutations):
			cases.append((f'{name}~{number}', mutate(source, generator)))
	if only:
		cases = [case for case in cases if only in case[0]]
	failures = 0
	total = 0
	with tempfile.TemporaryDirectory() as directory:
		for name, source in cases:
			for command in commands:
				path = os.path.join(directory, 'main.mc')
				with open(path, 'w', encoding='utf-8') as file:
					file.write(source)
				total += 1
				problem = compare(binary, command, path, verbose)
				if problem:
					failures += 1
					print(f'=== {name} [{command}]')
					print(problem)
					if verbose:
						print('--- source:')
						print(source)
	print(f'{total - failures} of {total} comparisons agree')
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
