
			f :: (a: V, b: V) -> bool {
				let mut c = a + b * 2;
				c += a;
				let d = -c;
				return a != b && a > b && d <= c && a >= b && add(a, b) == c;
			}

pub main :: () {}
