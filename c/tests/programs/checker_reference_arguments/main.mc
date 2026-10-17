
grow :: (mut v: &int) { v += 1; }
read :: (v: &int) -> int { return v; }
f :: () { let mut a = 1; grow(mut &a); _ = read(&a); _ = read(&5); }
pub main :: () {}
