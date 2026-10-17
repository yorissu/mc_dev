
# insertion sort of a local array through a slice, and references.

from io import println

accumulate :: (mut total: &int, value: int) {
	total = (total + value) % 1000003;
}

sort :: (items: []int) {
	for i in 1..lenof(items) {
		let mut j = i;
		while j > 0 && items[j - 1] > items[j] {
			let saved = items[j];
			items[j] = items[j - 1];
			items[j - 1] = saved;
			j -= 1;
		}
	}
}

pub main :: () {
	let mut items: [3000]int = {};
	let mut seed: u32 = 12345;
	for i in 0..3000 {
		[wrap] seed = seed * 1103515245 + 12345;
		items[i] = (int)(seed % 100000);
	}
	sort(items[..]);
	let mut total = 0;
	for i in 0..3000 {
		accumulate(mut &total, items[i]);
	}
	println("{} {} {} {}", items[0], items[1500], items[2999], total);
}
