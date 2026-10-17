
from io import print, println
from memory import alloc, realloc, free, copy

level3 :: (items: []int) -> int { return items[5]; }
level2 :: (items: []int) -> int { return level3(items) + 1; }
level1 :: (n: int) -> int { if n > 0 { return level1(n - 1); } let data: [2]int = {}; return level2(data[..]); }
pub main :: () -> int { return level1(4); }
