
from io import print, println
from memory import alloc, realloc, free, copy

convert :: (x: f64) -> int { return (int)x; }
pub main :: () -> int { println("{}", convert(2.9)); return convert(1.0 / 0.0); }
