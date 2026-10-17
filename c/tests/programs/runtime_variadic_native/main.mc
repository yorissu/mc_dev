
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
[extern = "test_collect"] collect :: (values: ...) -> int;
pub main :: () { println("{}", collect(1, 2.5, true)); }
