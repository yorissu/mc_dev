
from io import print, println
from memory import alloc, realloc, free, copy

pub main :: () { let n: uint = 1 << 40; let data = alloc<u8>(n); println("{}", lenof(data)); }
