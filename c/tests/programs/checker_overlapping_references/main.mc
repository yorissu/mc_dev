
swap :: (mut a: &int, mut b: &int) { let t = a; a = b; b = t; }
P :: { let mut x: int; let mut y: int; }
f :: () { let mut a = 1; swap(mut &a, mut &a); }
pub main :: () {}
