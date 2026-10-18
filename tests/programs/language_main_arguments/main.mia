
# main may take the command line: the file first, then what follows it.

from io import println
from memory import free

pub main :: (arguments: [][]u8) -> int {
	for i in 1..lenof(arguments) {
		println("{}: [{}] {}", i, arguments[i], lenof(arguments[i]));
	}
	if lenof(arguments) == 4 {
		_ = free(arguments);
	}
	arguments[1][0] = 65;
	return (int)lenof(arguments);
}
