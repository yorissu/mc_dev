
from io import print, println
from memory import alloc, realloc, free, copy

pub main :: () -> int { let data = alloc<u8>(3); let bigger = realloc(data, 10); println("{}", lenof(bigger)); return (int)data[0]; }
