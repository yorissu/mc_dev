
from io import print, println
from memory import alloc, realloc, free, copy

set :: (items: []int) { items[0] = 9; }
pub main :: () { let fixed: [2]int = { 1, 2, }; let mut open: [2]int = { 1, 2, }; set(open[..]); println("{}", open); set(fixed[..]); }
