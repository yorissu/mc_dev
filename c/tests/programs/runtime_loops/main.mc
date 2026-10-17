
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

pub main :: () {
	for i in 0..3 print("{}", i);
	println("");
	for i in 3..0 print("{}", i);
	println("");
	for i in 0..=2 { print("{}", i); }
	println("");
	let mut n = 0;
	while true {
		n += 1;
		if n == 2 continue;
		if n > 4 break;
		print("{}", n);
	}
	println("");
	for i in 0..10 {
		if i % 2 == 0 continue;
		if i > 7 break;
		print("{}", i);
	}
	println("");
}
