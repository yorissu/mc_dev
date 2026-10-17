
		V :: { let x: f32; let y: f32; }
		[op._+_] add :: (a: V, b: V) -> V { return { x: a.x + b.x, y: a.y + b.y, }; }
		[op._*_] scale :: (a: V, s: f32) -> V { return { x: a.x * s, y: a.y * s, }; }
		[op._==_] equal :: (a: V, b: V) -> bool { return a.x == b.x && a.y == b.y; }
		[op._<_] less :: (a: V, b: V) -> bool { return a.x < b.x; }
		[op.-_] negate :: (a: V) -> V { return { x: -a.x, y: -a.y, }; }

			f :: (a: V, b: V) -> bool {
				let mut c = a + b * 2;
				c += a;
				let d = -c;
				return a != b && a > b && d <= c && a >= b && add(a, b) == c;
			}

pub main :: () {}
