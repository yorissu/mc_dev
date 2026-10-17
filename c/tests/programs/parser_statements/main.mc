
			f :: () {
				let mut x: int = 1;
				[uninit] let buffer: [64]u8;
				if x > 0 x += 1; else if x < 0 { x = 0; } else return;
				while x < 10 x += 1;
				for i in 10..0 { continue; }
				for i in 0..=n break;
				{ let y = 2; }
				LOCAL :: 5;
				helper :: (a: int) -> int { return a; }
			}

pub main :: () {}
