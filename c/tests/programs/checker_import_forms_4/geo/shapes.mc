
pub Point :: { let mut x: int; let mut y: int; }
Hidden :: { let secret: int; }
pub make :: () -> Point { return { x: 1, y: 2, }; }
pub hidden :: () -> Hidden { return { secret: 1, }; }
[op._+_] pub add :: (a: Point, b: Point) -> Point { return { x: a.x + b.x, y: a.y + b.y, }; }
private_function :: () {}
