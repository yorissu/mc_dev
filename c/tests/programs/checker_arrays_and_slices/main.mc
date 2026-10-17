
			sum :: (s: []int) -> int {
				let mut total = 0;
				for i in 0..lenof(s) { total += s[i]; }
				return total;
			}
			f :: () -> int {
				let a: [_]int = { 1, 2, 3, };
				let b: [3]int = { 4, 5, 6, };
				return sum(a[..]) + sum(b[1..]) + sum({ 7, 8 });
			}

pub main :: () {}
