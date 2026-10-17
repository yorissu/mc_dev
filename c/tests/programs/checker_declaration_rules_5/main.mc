
V :: { let x: int; }
[op._+_] f :: (a: V, b: V) -> V { return a; }
[op._+_] g :: (a: V, b: V) -> V { return b; }
pub main :: () {}
