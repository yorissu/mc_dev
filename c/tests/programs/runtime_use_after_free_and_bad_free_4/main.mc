
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
pub main :: () { let mut a: [2]int = {}; _ = free(a[..]); }
