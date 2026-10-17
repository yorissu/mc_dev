
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

pub main :: () {
	let mut data = alloc<int>(2);
	data[0] = 1;
	data[1] = 2;
	data = realloc(data, 4);
	data[3] = 4;
	println("{}", data);
	println("{}", lenof(data));
	data = free(data);
	println("{}", data == null);
}
