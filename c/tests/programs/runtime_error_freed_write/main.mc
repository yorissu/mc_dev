
from io import print, println
from memory import alloc, realloc, free, copy

pub main :: () { let data = alloc<int>(3); let view = data[1..]; _ = free(data); view[0] = 5; }
