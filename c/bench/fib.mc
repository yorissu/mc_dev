
# recursive calls and integer arithmetic.

from io import println

fib :: (n: int) -> int {
	if n < 2 {
		return n;
	}
	return fib(n - 1) + fib(n - 2);
}

pub main :: () {
	println("{}", fib(30));
}
