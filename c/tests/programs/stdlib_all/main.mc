
from io import print, println
from memory import alloc, realloc, free, copy

from option import *
from result import *
from io import write, write_line

pub main :: () -> int {
	let a = some(5);
	let b = none<int>();
	println("{} {} {} {}", is_some(&a), is_some(&b), unwrap(&a), value_or(&b, 7));
	let r = ok<int, [5]u8>(3);
	let e = fail<int, [5]u8>("oops!");
	println("{} {} {}", is_ok(&r), is_ok(&e), e.error);
	write("raw "[..]);
	write_line("line"[..]);
	_ = unwrap(&b);
	return 0;
}
