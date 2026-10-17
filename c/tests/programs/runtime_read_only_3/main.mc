
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
set :: (s: []int) { s[0] = 5; }
pub main :: () { let a: [2]int = {}; set(a[..]); }
