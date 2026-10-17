
swap :: (mut a: &int, mut b: &int) { let t = a; a = b; b = t; }
P :: { let mut x: int; let mut y: int; }
f :: () { let mut s: [2]int = {}; let i = 0; swap(mut &s[i], mut &s[1]); }
pub main :: () {}
