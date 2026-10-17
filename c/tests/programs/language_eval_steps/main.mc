
# compile-time evaluation stops after --eval-steps steps; the error points
# at the innermost loop or call still running.

collatz :: (start: int) -> int {
	let mut n = start;
	let mut steps = 0;
	while n != 1 {
		if n % 2 == 0 { n = n / 2; } else { n = 3 * n + 1; }
		steps += 1;
	}
	return steps;
}
fib :: (n: int) -> int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }
LONG :: collatz(27);
LOOP :: forever();
forever :: () -> int { let mut i = 0; while true { i += 1; if i < 0 { return i; } } }
FIB :: fib(40);
pub main :: () {}
