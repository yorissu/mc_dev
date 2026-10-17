
Grid :: { let cells: []int; }
[op._[_]] at :: (g: Grid, i: int) -> int { return g.cells[i]; }
f :: (g: Grid) -> int { return g[2]; }
pub main :: () {}
