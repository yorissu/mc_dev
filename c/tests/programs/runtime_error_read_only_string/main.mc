
from io import print, println
from memory import alloc, realloc, free, copy

set :: (text: []u8) { text[0] = 65; }
pub main :: () { set("hello"[..]); }
