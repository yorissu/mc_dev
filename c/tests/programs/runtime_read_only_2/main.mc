
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
set :: (s: []u8) { s[0] = 5; }
pub main :: () { set("text"[..]); }
