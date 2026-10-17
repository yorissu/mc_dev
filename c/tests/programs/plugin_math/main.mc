
# uses the example plugin 'math': mc run --plugin <math library> main.mc

import math
from io import println
from memory import free

apply :: (f: (f64) -> f64, x: f64) -> f64 {
	return f(x);
}

pub main :: () -> int {
	println("{} {} {}", math.sqrt(2.0), math.pow(2.0, 10.0), math.floor(-2.5));
	println("{} {}", apply(math.sqrt, 16.0), apply(math.cos, 0.0));
	let values = math.steps(1.0, 0.5, 4);
	math.scale(values, 2.0);
	println("{} {}", values, math.sum(values));
	_ = free(values);
	let point: math.Point = { x: 3, y: 4, };
	println("{}", math.length(point));
	return math.floor(math.sqrt(99.0));
}
