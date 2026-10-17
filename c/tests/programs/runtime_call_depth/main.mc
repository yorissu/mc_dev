
# --call-depth limits the calls running at once.

from io import println

depth :: (n: int, limit: int) -> int {
	if n == limit {
		return n;
	}
	return depth(n + 1, limit);
}

pub main :: () -> int {
	println("{}", depth(0, 20));
	return depth(0, 200);
}
