
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

from option import *
from result import Result, ok, fail
first :: (s: []int) -> Option<int> { if lenof(s) == 0 { return none(); } return some(s[0]); }
digit :: (c: u8) -> Result<int, []u8> {
	if c < '0' || c > '9' { return fail("not a digit"[..]); }
	return ok((int)(c - '0'));
}
pub main :: () {
	let found = first({ 4, 5 });
	let missing = first(null);
	println("{}", value_or(&found, -1));
	println("{}", value_or(&missing, -1));
	let good = digit('7');
	let bad = digit('x');
	println("{}", good.value);
	println("{}", bad.error);
}
