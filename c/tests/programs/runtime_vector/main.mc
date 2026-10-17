
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

from vector import *
pub main :: () {
	let mut v = vector_create<int>(1);
	for i in 0..10 { vector_append(mut &v, &(i * i)); }
	println("{}", v.data[0..v.size]);
	vector_free(mut &v);
}
