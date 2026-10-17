
# a trailing NUL is left out when printing only for c strings: "..."c
# literals, constants and variables that are not mut made from one, and
# slices of them.

from io import println

GREETING :: "hey"c;
PLAIN :: "hey\0";

Named :: {
	let name: [3]u8;
}

pub main :: () {
	let text = "hello"c;
	let mut changing = "hi"c;
	changing[0] = 72;
	let named: Named = { name: "ab"c, };
	println("[{}] [{}] [{}] [{}] [{}]", text, GREETING, GREETING[1..], text[..], "lit"c);
	println("[{}] [{}] [{:>6}] [{}]", PLAIN, changing, GREETING, named);
	println("{} {}", lenof(text), lenof(GREETING));
}
