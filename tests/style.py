#!/usr/bin/env python3
"""checks the code style of the repository (see the readme, 'code style').

	python3 tests/style.py          prints every finding; fails when there is one
	python3 tests/style.py --fix    first fixes what it can (the alignment), then checks

files: every file starts with an empty line (but scripts, json and dot
files) and ends with exactly one newline, with no trailing whitespace; code
is indented with tabs; only .bat files have crlf line ends.

c: macros are lowercase; type names end in _s, _e, _u or _f; no name ends
in '_'; every name of a module starts with mia_<module>_ (public) or
_mia_<module>_ (private to its file), but the module 'global''s; parameters
of function definitions are const; public declarations in include/mia have
a '///' comment (one comment may cover a group of declarations). aligned:
the fields of a struct ('type   name   ;') and their trailing comments, the
backslashes of a macro, and a continuation line under the column after its
open parenthesis (a line one tab deeper is a block indent instead)."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP = {'.git', 'build', 'node_modules', 'programs'}
TAB = 4
TEXT = ('.c', '.h', '.def', '.mia', '.txt', '.sh', '.py', '.json', '.bat', '.yml')
CODE = ('.c', '.h', '.def', '.mia', '.sh', '.py')

MODULES = set('''arena ast bytecode checker cli code compiler diag evaluator format global int128 lexer map opcode parser
plugin plugins printer program scalar source span string system token type vector view vm io memory'''.split())
GLOBAL = set('''mia_has_typeof mia_noreturn mia_unused mia_count_of mia_printf_like mia_panic mia_panic_at mia_assert
mia_unreachable mia_malloc mia_calloc mia_realloc mia_free mia_strdup mia_portable mia_printf_family'''.split())
SYSTEM_MACROS = {'_XOPEN_SOURCE', '_POSIX_C_SOURCE', '_GNU_SOURCE', '_DARWIN_C_SOURCE', 'WIN32_LEAN_AND_MEAN'}
# typedefs of the compiler's own 128-bit integers: not a struct, enum, union or function
PLAIN_TYPEDEFS = {'mia_int128_native_u128', 'mia_int128_native_s128'}
# a module's file that also defines the functions of another one: checker.c implements program.h
ALSO_DEFINES = {'checker': {'program'}, 'io': {'plugin'}, 'memory': {'plugin'}}

findings = []


def report(path, line, rule, text=''):
	findings.append(f'{os.path.relpath(path, ROOT)}:{line}: {rule}' + (f': {text.strip()[:120]}' if text else ''))


def width(text):
	"""the display column after 'text' (tabs to multiples of 4)."""
	at = 0
	for character in text:
		at = (at // TAB + 1) * TAB if character == '\t' else at + 1
	return at


def indent(target):
	return '\t' * (target // TAB) + ' ' * (target % TAB)


# --- alignment (checked, and fixed with --fix) -----------------------------------------------

FIELD = re.compile(r'^(\t+)((?:alignas\([^()]*\) )?(?:const |struct |unsigned )*[\w*]+(?:\([^()]*\))?\**(?: ?const)?\**)( +)(\**\w+(?:\[[^\]]*\])?)( *)([;,])(.*)$')


def align_fields(lines):
	"""groups of struct fields ('type   name  ;') that are meant to be aligned."""
	changed = []
	number = 0
	while number < len(lines):
		group = []
		while number < len(lines) and FIELD.match(lines[number]) and FIELD.match(lines[number]).group(6) == ';':
			group.append(number)
			number += 1
		if len(group) >= 2:
			changed += _align_group(lines, group)
		if not group:
			number += 1
	return changed


def _align_group(lines, group):
	parts = [FIELD.match(lines[i]).groups() for i in group]
	if any(p[0] != parts[0][0] for p in parts):
		return []
	# meant to be aligned: some field has a wide gap, or a space before ';'
	if not any(len(p[2]) > 1 or p[4] for p in parts):
		return []
	tabs = parts[0][0]
	type_width = max(len(p[1]) for p in parts) + 1
	ends_aligned = any(p[4] for p in parts)
	name_width = max(len(p[3]) for p in parts)
	bodies = [tabs + p[1].ljust(type_width) + (p[3].ljust(name_width) if ends_aligned else p[3] + p[4]) + p[5] for p in parts]
	comment_column = max(width(body) for body in bodies) + 3
	changed = []
	for i, p, body in zip(group, parts, bodies):
		rest = p[6]
		if rest.strip().startswith('//'):
			rest = ' ' * max(1, comment_column - width(body)) + rest.strip()
		if body + rest != lines[i]:
			lines[i] = body + rest
			changed.append(i)
	return changed


def align_backslashes(lines):
	"""the backslashes that end the lines of a macro: in one column."""
	changed = []
	number = 0
	while number < len(lines):
		if not lines[number].rstrip().endswith('\\'):
			number += 1
			continue
		group = []
		while number < len(lines) and lines[number].rstrip().endswith('\\'):
			group.append(number)
			number += 1
		current = [width(lines[i].rstrip()[:-1]) for i in group]
		common = max(set(current), key=current.count)
		target = max(common, max(width(lines[i].rstrip()[:-1].rstrip()) + 1 for i in group))
		for i in group:
			body = lines[i].rstrip()[:-1].rstrip()
			new = body + ' ' * max(1, target - width(body)) + '\\'
			if new != lines[i]:
				lines[i] = new
				changed.append(i)
	return changed


def _parentheses(line, stack, state):
	"""pushes the column after each '(' of 'line' and pops at ')'; 'state' carries block comments."""
	index = 0
	while index < len(line):
		character = line[index]
		if state['comment']:
			if line.startswith('*/', index):
				state['comment'] = False
				index += 1
		elif line.startswith('//', index):
			return
		elif line.startswith('/*', index):
			state['comment'] = True
			index += 1
		elif character in '"\'':
			end = index + 1
			while end < len(line) and line[end] != character:
				end += 2 if line[end] == '\\' else 1
			index = end
		elif character == '(':
			stack.append(width(line[:index]) + 1)
		elif character == ')' and stack:
			stack.pop()
		index += 1


def align_continuations(lines):
	"""a line inside parentheses opened on an earlier line, indented more than one tab past
	its statement, goes under the column after the innermost open one (lines that start
	with an operator continue an expression and keep their indent)."""
	changed = []
	stack, state, base = [], {'comment': False}, 0
	for number, line in enumerate(lines):
		stripped = line.lstrip('\t ')
		if stack and stripped and not line.startswith('#'):
			lead = width(line[:len(line) - len(stripped)])
			operator = re.match(r'(&&|\|\||[-+*/%|&^?:<>=])', stripped)
			if lead > base + TAB and lead != stack[-1] and not operator:
				lines[number] = indent(stack[-1]) + stripped
				line = lines[number]
				changed.append(number)
		elif not stack:
			base = width(line[:len(line) - len(stripped)])
		if not line.startswith('#'):
			_parentheses(line, stack, state)
	return changed


# --- the checks ------------------------------------------------------------------------------

def module_of(relative):
	"""the module of a c file, or None outside mia's own code (the example and test plugins)."""
	if relative.startswith('source/system/'):
		return 'system'
	if relative.startswith(('source/', 'include/mia/', 'stdlib/')):
		return os.path.splitext(os.path.basename(relative))[0]
	return None


def check_layout(path, name, data):
	if not data.startswith((b'\n', b'\r\n', b'#!')) and not name.endswith('.json') and not name.startswith('.'):
		report(path, 1, 'the file does not start with an empty line')
	if not data.endswith(b'\n') or data.endswith((b'\n\n', b'\r\n\r\n')):
		report(path, 0, 'the file does not end with exactly one newline')
	if b'\r\n' in data and not name.endswith('.bat'):
		report(path, 0, 'crlf line ends')
	for number, line in enumerate(data.decode('utf-8').replace('\r\n', '\n').split('\n'), 1):
		if line != line.rstrip(' \t') and not name.endswith('.txt'):
			report(path, number, 'trailing whitespace')
		if name.endswith(CODE) and re.match(r'^ +\S', line) and not line.lstrip().startswith(('*', '//', '#')):
			report(path, number, 'indented with spaces', line)


def check_names(path, name, module, lines):
	for number, line in enumerate(lines, 1):
		code = re.sub(r'//.*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', line)
		match = re.match(r'\s*#\s*define\s+(\w+)', code)
		if match and match.group(1) != match.group(1).lower() and match.group(1) not in SYSTEM_MACROS:
			report(path, number, 'a macro that is not lowercase', match.group(1))
		for type_name in re.findall(r'^\}\s*(\w+)\s*;', code) + re.findall(r'^typedef\s+.*?\b(\w+)\s*;$', code):
			if not re.search(r'_[seuf]$', type_name) and type_name not in PLAIN_TYPEDEFS:
				report(path, number, 'a type name without _s, _e, _u or _f', type_name)
		if re.match(r'^\s*\}\s*else\b', code):
			report(path, number, '`else` on the line of `}`')
		for identifier in re.findall(r'(?<![A-Za-z0-9_])([A-Za-z_]\w*_)(?![A-Za-z0-9_#])', code):
			if not identifier.startswith('__') and identifier != 'mia_':
				report(path, number, 'a name that ends with `_`', identifier)
		if module is None:
			continue
		for identifier in re.findall(r'(?<![A-Za-z0-9_])(_?mia_\w*)', code):
			if identifier.startswith('_'):
				if module != 'global' and not identifier.startswith(f'_mia_{module}_'):
					report(path, number, f'a private name without _mia_{module}_', identifier)
			elif identifier not in GLOBAL and identifier != 'mia_' and not any(identifier == f'mia_{m}' or identifier.startswith(f'mia_{m}_') for m in MODULES):
				report(path, number, 'a name without a module', identifier)
		match = re.match(r'^(?!static|typedef|#|//|\s|\}|struct|union|enum|mia_plugin_export)[\w\s\*]*?\b(mia_\w+)\s*\(', code)
		if match and name.endswith('.c') and module != 'global' and line.rstrip().endswith('{'):
			allowed = {module} | ALSO_DEFINES.get(module, set())
			if not any(match.group(1).startswith(f'mia_{m}_') for m in allowed):
				report(path, number, f'a function of another module defined in the module {module}', match.group(1))
		match = re.match(r'^(?:mia_noreturn\s+)?static\s+(?:inline\s+)?[\w\s\*]*?\b(\w+)\s*\(', code)
		if match and '=' not in code.split('(')[0] and name.endswith('.c') and module != 'global' and not match.group(1).startswith(f'_mia_{module}_'):
			report(path, number, f'a static function without _mia_{module}_', match.group(1))


def check_parameters(path, text):
	"""the parameters of function definitions are const (va_list, arrays and function pointers aside)."""
	for match in re.finditer(r'^(?=\S)(?:static\s+)?(?:inline\s+)?[\w\s\*]+?\b(\w+)\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:mia_\w+\([^)]*\)\s*)?\{', text, re.M):
		parameters = match.group(2).strip()
		if parameters in ('', 'void'):
			continue
		for parameter in re.split(r',(?![^()]*\))', parameters):
			parameter = parameter.strip()
			if parameter == '...' or '(*' in parameter or parameter.startswith('va_list') or re.search(r'\[\w*\]$', parameter):
				continue
			value_const = parameter.startswith('const ') and '*' not in parameter
			pointer_const = re.search(r'\*\s*const\s+\w+$', parameter) is not None
			if not (value_const or pointer_const):
				report(path, text[:match.start()].count('\n') + 1, 'a parameter that is not const', f'{match.group(1)}: {parameter}')


def check_comments(path, lines):
	"""every public declaration of a header in include/mia has a /// comment: right above
	it, or above the group of declarations it belongs to (blank lines between them)."""
	declaration = re.compile(r'^[a-z][\w\s\*]*\bmia_\w+\(')
	for number, line in enumerate(lines, 1):
		if not declaration.match(line) or line.startswith(('static', 'typedef')):
			continue
		above = number - 2
		while above >= 0 and (lines[above].strip() == '' or lines[above].startswith(('\t', ' ')) or declaration.match(lines[above])):
			above -= 1
		if above < 0 or not lines[above].startswith('///'):
			report(path, number, 'a public declaration without a /// comment', line)


def check_file(path, fix):
	relative = os.path.relpath(path, ROOT).replace(os.sep, '/')
	name = os.path.basename(path)
	data = open(path, 'rb').read()
	check_layout(path, name, data)
	if not name.endswith(('.c', '.h', '.def')):
		return
	lines = data.decode('utf-8').split('\n')
	if fix:
		fixed = list(lines)
		if align_continuations(fixed) + align_fields(fixed) + align_backslashes(fixed):
			open(path, 'w', encoding='utf-8', newline='').write('\n'.join(fixed))
			lines = fixed
	for kind, align in (('a continuation line', align_continuations), ('a field', align_fields), ('a backslash', align_backslashes)):
		for number in align(list(lines)):
			report(path, number + 1, f'{kind} is not aligned', lines[number])
	check_names(path, name, module_of(relative), lines)
	check_parameters(path, '\n'.join(lines))
	if relative.startswith('include/mia/') and name.endswith('.h'):
		check_comments(path, lines)


def main():
	fix = '--fix' in sys.argv[1:]
	for directory, folders, names in os.walk(ROOT):
		folders[:] = sorted(f for f in folders if f not in SKIP)
		for name in sorted(names):
			if name.endswith(TEXT) or name in ('makefile', '.gitignore', '.gitattributes'):
				check_file(os.path.join(directory, name), fix)
	for finding in findings:
		print(finding)
	print(f'style: {len(findings)} finding(s)')
	return 1 if findings else 0


if __name__ == '__main__':
	sys.exit(main())
