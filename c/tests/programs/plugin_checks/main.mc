
# the checks of plugin functions: math.sqrt warns about a negative
# constant; a read-only slice stops math.scale at run time.

import math
from io import println

NEGATIVE :: -2.0;

pub main :: () {
	println("{}", math.sqrt(NEGATIVE));
	println("{}", math.sqrt(-1.0 * 4.0));
	let mut x: f64 = 9.0;
	x += 0.0;
	println("{}", math.sqrt(x));
	let fixed: [2]f64 = { 1.0, 2.0, };
	math.scale(fixed[..], 2.0);
}
