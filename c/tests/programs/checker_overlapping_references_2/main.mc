
swap :: (mut a: &int, mut b: &int) { let t = a; a = b; b = t; }
P :: { let mut x: int; let mut y: int; }
f :: () { let mut p: P = {}; swap(mut &p.x, mut &p.y); }
pub main :: () {}
