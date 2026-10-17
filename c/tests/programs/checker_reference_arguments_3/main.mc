
grow :: (mut v: &int) { v += 1; }
read :: (v: &int) -> int { return v; }
f :: () { let a = 1; grow(mut &a); }
pub main :: () {}
