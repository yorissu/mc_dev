
grow :: (mut v: &int) { v += 1; }
read :: (v: &int) -> int { return v; }
f :: () { grow(mut &5); }
pub main :: () {}
