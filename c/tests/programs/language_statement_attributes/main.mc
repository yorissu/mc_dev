
# attributes before any statement: '[wrap]' covers everything inside a
# block, loop or if (but not the functions and lambdas in it).

from io import println

pub main :: () -> int {
	[wrap] {
		let b = 200u8 + 100;
		println("{}", b);
	}
	[wrap] for i in 0..2 {
		println("{}", 250u8 + 10);
	}
	[wrap] while false {
		println("{}", 250u8 + 10);
		let f = () -> u8 { return 255u8 + 1; };
		_ = f;
	}
	[wrap] if true { println("{}", 255u8 + 1); } else { println("{}", 255u8 + 2); }
	println("{}", 255u8 + 3);
	[uninit] while false {}
	for i in 0..1 {
		[wrap] continue;
	}
	[wrap] return (int)(127s8 + 1);
}
