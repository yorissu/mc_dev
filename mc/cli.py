
"""command line: 'python -m mc <command> [options] <file>'.

commands:
  tokens   print the tokens of a file
  ast      print the syntax tree of a file
  check    check a program (the file and every module it imports)
  mono     check a program and print it with every generic instantiated and
           every expression typed
  build    compile a program to bytecode ('-o <file.mcb>', default: next to it)
  dis      print the bytecode of a program ('.mc' or '.mcb')
  run      run a program ('.mc' or '.mcb')

options:
  -I <dir>         add a module search directory (after the entry's directory)
  --stdlib <dir>   the standard library directory (default: the bundled one)
  -o <file>        the output of 'build'
  --color          color diagnostics (default when printing to a terminal)
  --no-color       plain diagnostics
  --format short   one line per diagnostic: path:line:column: error[code]: message
  --limit <n>      'run' stops after <n> instructions

exit status: 0 success, 1 errors, 2 usage, 101 runtime error. 'run' exits
with the result of 'main' when it returns an int."""

from __future__ import annotations

import os
import sys

from . import ast
from .checker import ConstantInfo, FunctionInfo, Program, StructInfo, check
from .diag import Diagnostics
from .evaluate import FunctionValue, SliceValue
from .lexer import lex
from .parser import parse
from .printer import dump
from .source import Source
from .types import Reference

STDLIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'stdlib')
COMMANDS = ('tokens', 'ast', 'check', 'mono', 'build', 'dis', 'run')
RUNTIME_ERROR = 101


def format_value(value: object) -> str:
	if isinstance(value, bool):
		return 'true' if value else 'false'
	if isinstance(value, list):
		return '{' + ', '.join(format_value(item) for item in value) + '}'
	if isinstance(value, dict):
		return '{' + ', '.join(f'{key}: {format_value(item)}' for key, item in value.items()) + '}'
	if isinstance(value, SliceValue):
		return 'slice ' + format_value(value.data[value.start:value.start + value.length])
	if isinstance(value, FunctionValue):
		return repr(value)
	return repr(value)


def indent(text: str, prefix: str) -> str:
	return ''.join(prefix + line + '\n' for line in text.rstrip('\n').split('\n'))


def program_text(program: Program) -> str:
	"""every module with its struct instances, constants and function instances."""
	out = []
	for module in program.modules:
		out.append(f'module {module.name} ({module.path})\n')
		for symbol in module.symbols.values():
			if isinstance(symbol, StructInfo):
				for layout in symbol.instances.values():
					fields = ', '.join(f'{"mut " if item.mutable else ""}{name}: {item.type}' for name, item in layout.fields.items())
					out.append(f'  struct {layout.type} {{ {fields} }}\n')
			elif isinstance(symbol, ConstantInfo):
				out.append(f'  const {symbol.name}: {symbol.type} = {format_value(symbol.value)}\n')
		for symbol in module.symbols.values():
			if not isinstance(symbol, FunctionInfo):
				continue
			if symbol.generics and not symbol.instances:
				out.append(f'  func {symbol.name}<{", ".join(symbol.generics)}> (never instantiated)\n')
			for instance in symbol.instances.values():
				parameters = ', '.join(f'{"mut " if parameter.mutable else ""}{parameter.name}: '
					+ (f'&{type_.element}' if isinstance(type_, Reference) else str(type_))
					for parameter, type_ in zip(instance.node.parameters, instance.parameters))
				result = f' -> {instance.result}' if instance.result is not None else ''
				kind = ' [builtin]' if symbol.builtin else ''
				out.append(f'  func {instance}({parameters}){result}{kind}\n')
				if instance.node.body is not None:
					out.append(indent(dump(instance.node.body, typed=True), '    '))
		out.append('\n')
	return ''.join(out)


def runtime_report(error, module, color: bool) -> str:
	"""a runtime error with the source line and the calls that led to it."""
	from .diag import Diagnostic, render
	from .source import Span
	diagnostic = Diagnostic('error', 'runtime', error.message)
	sources: dict[str, Source] = {}
	trace = error.trace
	for depth, (name, position) in enumerate(trace):
		where = ''
		if position is not None:
			path = module.files[position[0]]
			where = f' at {path}:{position[1]}:{position[2]}'
			if depth == 0:
				if path not in sources:
					try:
						with open(path, encoding='utf-8') as file:
							sources[path] = Source(path, file.read())
					except OSError:
						sources[path] = None
				source = sources[path]
				if source is not None and position[1] <= len(source.line_starts):
					start = source.line_starts[position[1] - 1] + position[2] - 1
					diagnostic.labels.append(__import__('mc.diag', fromlist=['Label']).Label(
						Span(source, start, start + position[3]), 'here', True))
		diagnostic.note(('in ' if depth == 0 else 'called from ') + f'`{name}`{where}')
	text = render(diagnostic, color)
	return text.replace('error[runtime]', 'runtime error', 1)


def build(path: str, diagnostics: Diagnostics, include: list[str], stdlib: str):
	"""the bytecode of the program at 'path' ('.mcb' files are loaded), or none."""
	from .bytecode import load
	from .compiler import CompileError, compile_program
	if path.endswith('.mcb'):
		try:
			return load(path)
		except (OSError, ValueError, KeyError) as problem:
			diagnostics.error('E0601', f'cannot load `{path}`: {problem}')
			return None
	program = check(path, diagnostics, include, stdlib)
	if diagnostics.errors:
		return None
	try:
		return compile_program(program)
	except CompileError as problem:
		diagnostics.error('E0602', f'cannot compile: {problem}')
		return None


def main(arguments: list[str] | None = None) -> int:
	arguments = list(sys.argv[1:] if arguments is None else arguments)
	include: list[str] = []
	stdlib = STDLIB
	color = sys.stderr.isatty()
	positional: list[str] = []
	output: str | None = None
	short = False
	limit = None
	while arguments:
		argument = arguments.pop(0)
		if argument in ('-h', '--help'):
			sys.stdout.write(__doc__.strip() + '\n')
			return 0
		if argument == '-I' and arguments:
			include.append(arguments.pop(0))
		elif argument == '--stdlib' and arguments:
			stdlib = arguments.pop(0)
		elif argument == '-o' and arguments:
			output = arguments.pop(0)
		elif argument == '--format' and arguments:
			short = arguments.pop(0) == 'short'
		elif argument == '--limit' and arguments:
			limit = int(arguments.pop(0))
		elif argument == '--color':
			color = True
		elif argument == '--no-color':
			color = False
		elif argument.startswith('-'):
			sys.stderr.write(f'error[E0602]: unknown option `{argument}`\n')
			return 2
		else:
			positional.append(argument)
	if len(positional) != 2 or positional[0] not in COMMANDS:
		sys.stderr.write('usage: python -m mc (tokens|ast|check|mono|build|dis|run) [-I <dir>] [--stdlib <dir>] [-o <file>] <file>\n')
		return 2
	command, path = positional
	diagnostics = Diagnostics()
	if command in ('build', 'dis', 'run'):
		from .bytecode import disassemble, save
		from .vm import Machine, MachineError
		module = build(path, diagnostics, include, stdlib)
		diagnostics.render(sys.stderr, color, short)
		if module is None:
			return 1
		if command == 'build':
			target = output or (path[:-3] if path.endswith('.mc') else path) + '.mcb'
			save(module, target)
			return 0
		if command == 'dis':
			sys.stdout.write(disassemble(module))
			return 0
		try:
			result = Machine(module, limit=limit).run()
		except MachineError as error:
			sys.stdout.flush()
			sys.stderr.write(runtime_report(error, module, color))
			return RUNTIME_ERROR
		sys.stdout.flush()
		return result & 0xFF if isinstance(result, int) and not isinstance(result, bool) else 0
	if command in ('tokens', 'ast'):
		try:
			with open(path, encoding='utf-8') as file:
				source = Source(path, file.read())
		except OSError:
			sys.stderr.write(f'error[E0601]: cannot read `{path}`\n')
			return 1
		if command == 'tokens':
			for token in lex(source, diagnostics):
				line, column = source.location(token.span.start)
				value = f' {token.value!r}' if token.value is not None else ''
				suffix = f' suffix={token.suffix}' if token.suffix else ''
				sys.stdout.write(f'{line}:{column} {token.kind} {token.text!r}{value}{suffix}\n')
		else:
			sys.stdout.write(dump(parse(source, diagnostics)))
	else:
		program = check(path, diagnostics, include, stdlib)
		if command == 'mono' and not diagnostics.errors:
			sys.stdout.write(program_text(program))
	diagnostics.render(sys.stderr, color, short)
	errors, warnings = len(diagnostics.errors), len(diagnostics.warnings)
	if (errors or warnings) and not short:
		sys.stderr.write(f'{errors} error(s), {warnings} warning(s)\n')
	return 1 if errors else 0
