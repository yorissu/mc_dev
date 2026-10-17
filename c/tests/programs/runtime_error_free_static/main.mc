
from io import print, println
from memory import alloc, realloc, free, copy

pub main :: () { let items: [2]int = {}; _ = free(items[..]); }
