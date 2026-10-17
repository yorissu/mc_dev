
# a byte array on the heap, indexed in tight loops.

from io import println
from memory import alloc, free

pub main :: () {
	let size: uint = 2000000;
	let composite = alloc<bool>(size);
	let mut count = 0;
	for i in 2..size {
		if composite[i] {
			continue;
		}
		count += 1;
		let mut j = i * i;
		while j < size {
			composite[j] = true;
			j += i;
		}
	}
	println("{} primes", count);
	_ = free(composite);
}
