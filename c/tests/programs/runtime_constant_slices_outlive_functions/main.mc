
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
name :: () -> []u8 { return "static"[..]; }
pub main :: () { println("{}", name()); }
