#!/usr/bin/env python3
"""lays out mia code as written by hand: blocks and struct bodies over several
lines, one statement per line, indentation by nesting (a tab per level),
blank lines around declarations that take several lines.

	python3 tests/format.py <file.mia>...            rewrites the files
	python3 tests/format.py --check <file or dir>...  lists the files it would change; fails when there are any

composite literals ('{ x: 1, y: 2 }') stay on their line; lines with
multiline strings and block comments are kept as they are, and so are lines
whose brackets do not balance (code with syntax errors on purpose)."""
import os
import sys

PAIRS = {'(': ')', '[': ']', '{': '}'}


def tokenize(text):
	"""(kind, start, end) tokens; None when the line cannot be tokenized safely."""
	tokens, at = [], 0
	while at < len(text):
		c = text[at]
		if c in ' \t':
			at += 1
		elif c == '#' or text.startswith('//', at):
			tokens.append(('comment', at, len(text)))
			at = len(text)
		elif text.startswith('/*', at) or text.startswith('"""', at):
			return None
		elif c == '"':
			end = at + 1
			while end < len(text) and text[end] != '"':
				end += 2 if text[end] == '\\' else 1
			if end >= len(text):
				return None
			end += 1
			if end < len(text) and text[end] == 'c':
				end += 1
			tokens.append(('string', at, end))
			at = end
		elif c == "'":
			end = at + 1
			while end < len(text) and text[end] != "'":
				end += 2 if text[end] == '\\' else 1
			if end >= len(text):
				return None
			tokens.append(('char', at, end + 1))
			at = end + 1
		elif c in '([{)]}':
			tokens.append((c, at, at + 1))
			at += 1
		elif c == ';':
			tokens.append((';', at, at + 1))
			at += 1
		elif c.isalnum() or c == '_' or ord(c) >= 0x80:
			end = at
			while end < len(text) and (text[end].isalnum() or text[end] in '_.' or ord(text[end]) >= 0x80):
				end += 1
			tokens.append(('word', at, end))
			at = end
		else:
			tokens.append(('other', at, at + 1))
			at += 1
	return tokens


def layout(line):
	"""the lines 'line' becomes (or [line] when it stays)."""
	indent = len(line) - len(line.lstrip('\t'))
	if line[indent:indent + 1] == ' ':
		return [line]
	text = line[indent:]
	tokens = tokenize(text)
	if not tokens:
		return [line]
	# brackets
	match, stack = {}, []
	for i, (kind, _, _) in enumerate(tokens):
		if kind in PAIRS:
			stack.append(i)
		elif kind in ')]}':
			if not stack or PAIRS[tokens[stack[-1]][0]] != kind:
				return [line]
			opened = stack.pop()
			match[opened], match[i] = i, opened
	if stack:
		return [line]
	# a brace is a block when a ';' is inside it through braces only
	parent, stack = {}, []
	for i, (kind, _, _) in enumerate(tokens):
		parent[i] = stack[-1] if stack else None
		if kind in PAIRS:
			stack.append(i)
		elif kind in ')]}':
			stack.pop()
	block = set()
	for i, (kind, _, _) in enumerate(tokens):
		if kind != ';':
			continue
		up = parent[i]
		while up is not None and tokens[up][0] == '{':
			block.add(up)
			up = parent[up]
	block = {b for b in block if match[b] > b + 1}
	statements_on_line = sum(1 for i, (kind, _, _) in enumerate(tokens) if kind == ';' and parent[i] is None)
	if not block and statements_on_line < 2:
		return [line]
	out = []
	current = ['\t' * indent]

	def flush():
		piece = ''.join(current).rstrip()
		if piece.strip():
			out.append(piece)
		current.clear()

	def emit(first, last, depth):
		"""the tokens first..last (inclusive) as statements at 'depth'."""
		current[:] = ['\t' * depth]
		i = first
		while i <= last:
			kind, start, end = tokens[i]
			if len(current) > 1 or (current and current[-1].strip()):
				# the original space before this token
				previous_end = tokens[i - 1][2] if i > first else start
				gap = text[previous_end:start] if i > first else ''
				current.append(gap)
			current.append(text[start:end])
			if i in block:
				close = match[i]
				flush()
				emit(i + 1, close - 1, depth + 1)
				flush()
				current[:] = ['\t' * depth, '}']
				i = close + 1
				# what follows the block: ';' or ')' continues the statement, 'else' starts a line, anything else a new statement
				if i <= last:
					following = tokens[i]
					following_text = text[following[1]:following[2]]
					if following_text == 'else':
						flush()
						current[:] = ['\t' * depth]
						continue
					if following[0] in (';', ')', ']', 'other') or following[0] == 'comment':
						continue
					flush()
					current[:] = ['\t' * depth]
				continue
			if kind == ';' and (parent[i] is None or parent[i] in block):
				# a statement ends here, unless a comment follows on this line
				if i + 1 <= last and tokens[i + 1][0] == 'comment':
					current.append(text[end:tokens[i + 1][1]] + text[tokens[i + 1][1]:tokens[i + 1][2]])
					i += 1
				flush()
				current[:] = ['\t' * depth]
			i += 1
		flush()

	emit(0, len(tokens) - 1, indent)
	return out


def format_text(source):
	lines = source.split('\n')
	laid = []   # (line, inside a multiline string or a block comment: kept as it is)
	inside = False
	for line in lines:
		quotes = line.count('"""')
		if inside or quotes or '/*' in line:
			laid.append((line, True))
			if quotes % 2:
				inside = not inside
			continue
		for piece in (layout(line) if line.strip() and not line.lstrip().startswith(('#', '//')) else [line]):
			laid.append((piece, False))
	# indentation by nesting (code that was shifted as a whole, too)
	laid = list(zip(reindent([line for line, _ in laid]), [kept for _, kept in laid]))
	# a declaration over several lines is set apart by blank lines, as one written by hand
	result = []
	for line, kept in laid:
		if kept:
			result.append(line)
			continue
		previous = result[-1] if result else ''
		starts_block = line.rstrip().endswith('{') and not line.startswith(('\t', ' ', '#', '//'))
		# (an attribute on the line before belongs to the declaration)
		if starts_block and previous.strip() and not previous.lstrip().startswith(('#', '//', '[')) and previous != '}':
			result.append('')
		if previous == '}' and line.strip():
			result.append('')
		result.append(line)
	return '\n'.join(result)


def _brackets(code, state):
	"""the brackets of one line, in order, skipping strings, chars and comments; 'state' carries
	block comments ('comment': nesting) and multiline strings ('string': True) across lines."""
	found, at = [], 0
	while at < len(code):
		if state['comment']:
			if code.startswith('/*', at):
				state['comment'] += 1
				at += 2
			elif code.startswith('*/', at):
				state['comment'] -= 1
				at += 2
			else:
				at += 1
			continue
		if state['string']:
			end = code.find('"""', at)
			if end < 0:
				return found
			state['string'] = False
			at = end + 3
			continue
		c = code[at]
		if code.startswith('"""', at):
			state['string'] = True
			at += 3
		elif code.startswith('/*', at):
			state['comment'] = 1
			at += 2
		elif c == '#' or code.startswith('//', at):
			return found
		elif c == '"' or c == "'":
			end = at + 1
			while end < len(code) and code[end] != c:
				end += 2 if code[end] == '\\' else 1
			at = end + 1
		elif c in '({[)}]':
			found.append(c)
			at += 1
		else:
			at += 1
	return found


def _words(code):
	"""the tokens of a line of code as text, comments left out ([] when it cannot be tokenized)."""
	tokens = tokenize(code)
	return [code[start:end] for kind, start, end in (tokens or []) if kind != 'comment']


def reindent(lines):
	"""the lines with their indentation by nesting (brackets, the statement after
	a trailing 'then' or 'else', and an 'else' at the level of its 'if')."""
	out = []
	levels = []   # per open bracket: the level of the lines inside it
	carry = 0     # extra levels for the next line of code
	ifs = []      # the levels of the one-line 'if ... then' lines an 'else' may still belong to, innermost last
	state = {'comment': 0, 'string': False}
	for line in lines:
		kept = state['comment'] or state['string']
		body = line.lstrip('\t ')
		brackets = _brackets(line if kept else body, state)
		if kept or not body:
			out.append(line if kept else '')
			for c in brackets:
				if c in '({[':
					levels.append((levels[-1] if levels else 0) + 1)
				elif levels:
					levels.pop()
			continue
		# closers at the start of the line: the line is at the level of the line that opened them
		leading = 0
		for c in body:
			if c in ')}]':
				leading += 1
			elif c not in ' \t':
				break
		leading = min(leading, len(levels))
		if leading > 0:
			# the level of the line that opened the outermost bracket this line closes first
			level = levels[len(levels) - leading] - 1
		else:
			level = levels[-1] if levels else 0
		base = level
		words = [] if body.startswith(('#', '//')) else _words(body)
		first = words[0] if words else ''
		if 'else' == first and ifs and 0 == leading:
			# an 'else' belongs to the nearest 'if' without one, as the parser decides
			level = ifs.pop()
		else:
			level += carry
		if body.startswith('~') and 0 == leading:
			level += 1   # a step of a pipe that goes on from the line above
		out.append('\t' * level + body)
		for c in brackets:
			if c in '({[':
				levels.append(level + 1)
			elif levels:
				levels.pop()
		if words:
			if 'if' == first or ('else' == first and words[1:2] == ['if']):
				if 0 == carry and 'if' == first:
					ifs.clear()   # a new statement: earlier 'if's are finished
				if 'then' in words:
					ifs.append(level)
			elif first != 'else' and 0 == carry:
				ifs.clear()
			# a trailing 'then' or 'else': the single statement of the body comes next, one level deeper
			carry = level - base + 1 if words[-1] in ('then', 'else') else 0
			if body.startswith('~'):
				carry = 0
	return out


def _files(paths):
	for path in paths:
		if os.path.isdir(path):
			for folder, _, names in os.walk(path):
				yield from (os.path.join(folder, name) for name in sorted(names) if name.endswith('.mia'))
		else:
			yield path


def main():
	check = '--check' in sys.argv[1:]
	changed = []
	for path in _files(arg for arg in sys.argv[1:] if arg != '--check'):
		data = open(path, 'rb').read()
		try:
			source = data.decode('utf-8')
		except UnicodeDecodeError:
			continue   # a test of bytes that are not utf-8
		formatted = format_text(source)
		if formatted == source:
			continue
		changed.append(path)
		if not check:
			open(path, 'w', encoding='utf-8', newline='\n').write(formatted)
	for path in changed:
		print(('not formatted: ' if check else 'formatted: ') + path)
	return 1 if check and changed else 0


if __name__ == '__main__':
	sys.exit(main())
