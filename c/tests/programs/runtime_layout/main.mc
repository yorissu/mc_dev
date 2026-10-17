
# values laid out like c: sizes, nested structs and arrays, slices of
# fields, references into heap memory, values on the stack and in memory.

from io import println
from memory import alloc, free, copy

Small :: {
	let mut tag: u8;
	let mut value: s32 = -7;
	let mut flag: bool;
}

Big :: {
	let mut name: [5]u8;
	let mut inner: Small;
	let mut items: [3]s16;
	let mut view: []u8;
	let mut ratio: f32 = 0.5;
	let mut next: (int) -> int;
}

twice :: (x: int) -> int {
	return 2 * x;
}

make :: (tag: u8) -> Small {
	return { tag: tag, flag: tag > 1, };
}

bump :: (mut value: &s32) {
	value += 100;
}

pick :: (items: [3]s16, index: int) -> s16 {
	return items[index];
}

pub main :: () -> int {
	println("{} {} {} {}", sizeof(Small), sizeof(Big), sizeof([3]Small), sizeof([]Big));
	let mut big: Big = { name: "hello", inner: make(2), items: { 1, -2, 3, }, view: "abc"[..], next: twice, };
	println("{}", big);
	println("{} {} {} {}", big.inner, big.items[1], big.name[1..3], big.next(21));
	bump(mut &big.inner.value);
	big.items[2] = (s16)big.inner.value;
	println("{} {}", big.inner.value, big.items);
	let smalls: [3]Small = { make(1), make(2), make(3), };
	println("{} {}", smalls[2], smalls[1].flag);
	let heap = alloc<Small>(4);
	println("{}", heap[3]);
	heap[1] = smalls[2];
	bump(mut &heap[1].value);
	heap[2].tag = 9;
	println("{} {}", heap[1], heap[2]);
	let part = heap[1..3];
	println("{} {}", lenof(part), part[1].tag);
	let bytes = alloc<u8>(8);
	_ = copy(bytes, big.name[..]);
	bytes[5] = 33;
	println("{} {}", bytes[0..6], pick(big.items, 1));
	let mut total = 0;
	for i in 0..3 {
		let temporary: [2]int = { i, i * 10, };
		let view = temporary[..];
		total += view[1];
	}
	println("{} {}", total, make(5).value);
	_ = free(bytes);
	_ = free(heap);
	let floats: [3]f32 = { 0.1, 0.2, 0.3, };
	let mut sum: f32 = 0;
	for i in 0..3 {
		sum += floats[i];
	}
	println("{} {}", sum, (f64)sum);
	return (int)big.inner.value;
}
