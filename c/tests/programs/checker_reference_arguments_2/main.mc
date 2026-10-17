
grow :: (mut v: &int) { v += 1; }
read :: (v: &int) -> int { return v; }
f :: () { let mut a = 1; grow(&a); }
pub main :: () {}
