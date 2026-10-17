
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

escape :: () -> []int { let mut local: [3]int = { 1, 2, 3 }; return local[..]; }
pub main :: () { let s = escape(); _ = s[0]; }
