
from io import print, println
from memory import alloc, realloc, free, copy

pub main :: () { let mut items: [3]int = {}; let mut i = 0; while true { items[i] = i; i += 1; } }
