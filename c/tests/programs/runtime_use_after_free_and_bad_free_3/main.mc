
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
pub main :: () { let data = alloc<int>(4); _ = free(data[1..]); }
