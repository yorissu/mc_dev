
from io import print, println
from memory import alloc, realloc, free, copy

get :: (items: [3]int, index: int) -> int { return items[index]; }
pub main :: () -> int { let items: [3]int = { 1, 2, 3, }; return get(items, -1); }
