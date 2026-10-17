
from memory import alloc, realloc, free

pub Vector<T> :: {
	let mut data: []T;
	let mut size: uint = 0;
}

pub vector_create<T> :: (cap: uint = 4) -> Vector<T> {
	let data = alloc<T>(cap);
	assert(lenof(data) > 0);
	return { data: data, };
}

pub vector_free<T> :: (mut vector: &Vector<T>) {
	assert(lenof(vector.data) > 0);
	vector.data = free(vector.data);
	vector.size = 0;
}

pub vector_append<T> :: (mut vector: &Vector<T>, value: &T) {
	let cap = lenof(vector.data);
	if vector.size + 1 > cap {
		vector.data = realloc(vector.data, cap * 2);
		assert(lenof(vector.data) > 0);
	}
	vector.data[vector.size] = value;
	vector.size += 1;
}
