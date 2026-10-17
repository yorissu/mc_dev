
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
get :: (s: []int, i: uint) -> int { return s[i]; }
pub main :: () { let mut a: [2]int = {}; _ = get(a[..], 2); }
