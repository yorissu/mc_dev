
"""source files and spans (character ranges inside a source file)."""

from __future__ import annotations

import bisect
from dataclasses import dataclass


class Source:
	"""one loaded source file. 'path' is what diagnostics print."""

	def __init__(self, path: str, text: str) -> None:
		self.path = path
		self.text = text
		self.line_starts = [0]
		for index, character in enumerate(text):
			if character == '\n':
				self.line_starts.append(index + 1)

	def location(self, offset: int) -> tuple[int, int]:
		"""1-based line and column of 'offset'."""
		line = bisect.bisect_right(self.line_starts, offset) - 1
		return line + 1, offset - self.line_starts[line] + 1

	def line_text(self, line: int) -> str:
		"""text of the 1-based 'line' without its line break."""
		start = self.line_starts[line - 1]
		end = self.line_starts[line] - 1 if line < len(self.line_starts) else len(self.text)
		return self.text[start:end].rstrip('\r')

	def __deepcopy__(self, memo: dict) -> Source:
		# syntax trees are copied for every generic instantiation; the source
		# they point into is shared.
		return self

	def __repr__(self) -> str:
		return f'Source({self.path!r})'


@dataclass(frozen=True)
class Span:
	source: Source
	start: int
	end: int

	def join(self, other: Span) -> Span:
		return Span(self.source, min(self.start, other.start), max(self.end, other.end))

	@property
	def text(self) -> str:
		return self.source.text[self.start:self.end]

	def __repr__(self) -> str:
		line, column = self.source.location(self.start)
		return f'{self.source.path}:{line}:{column}'
