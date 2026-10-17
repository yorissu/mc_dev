
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

fill :: (s: []int, value: int) { for i in 0..lenof(s) { s[i] = value; } }
pub main :: () {
	let mut a: [5]int = {};
	fill(a[1..4], 7);
	println("{}", a);
	let s = a[..];
	let t = s[3..];
	t[0] = 9;
	println("{}", a);
	println("{}", lenof(t));
	println("{}", s == null);
	println("{}", null == t[0..0]);
	_ = copy(a[0..2], t);
	println("{}", a);
}
