
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
pub main :: () { let mut count = 0; for i in 0u8..=255u8 { count += 1; } println("{}", count); }
