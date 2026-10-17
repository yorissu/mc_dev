
from io import print, println
from memory import alloc, realloc, free, copy

pub main :: () { let mut items: [2]int = {}; _ = realloc(items[..], 4); }
