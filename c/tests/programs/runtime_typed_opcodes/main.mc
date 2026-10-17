
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
add :: (a: s32, b: s32) -> s32 { return a + b; }
pub main :: () { _ = add(1, 2); _ = 1.5f32 < 2; }
