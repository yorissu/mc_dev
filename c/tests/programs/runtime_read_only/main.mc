
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
TABLE :: ([_]int){ 1, 2 };
set :: (s: []int) { s[0] = 5; }
pub main :: () { set(TABLE[..]); }
