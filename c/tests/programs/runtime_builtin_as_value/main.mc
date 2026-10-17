
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
pub main :: () { let make: (uint) -> []int = alloc<int>; let s = make(3); println("{}", lenof(s)); _ = free(s); }
