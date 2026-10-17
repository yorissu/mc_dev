
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
[plugin = "test_record"] record :: (x: int) -> int;
pub main :: () { println("{}", record(21)); }
