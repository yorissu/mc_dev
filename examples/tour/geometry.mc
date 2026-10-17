
# geometry: a small vector type with operator functions. operators are found
# through the operand type, so users of Vec2 need no extra imports.

pub Vec2 :: {
	let mut x: f32;
	let mut y: f32;
}

[op._+_]
pub vec2_add :: (a: Vec2, b: Vec2) -> Vec2 {
	return { x: a.x + b.x, y: a.y + b.y, };
}

[op._-_]
pub vec2_subtract :: (a: Vec2, b: Vec2) -> Vec2 {
	return { x: a.x - b.x, y: a.y - b.y, };
}

[op._*_]
pub vec2_scale :: (a: Vec2, s: f32) -> Vec2 {
	return { x: a.x * s, y: a.y * s, };
}

[op._*_]
pub f32_scale_vec2 :: (s: f32, a: Vec2) -> Vec2 {
	return a * s;
}

[op._==_]
pub vec2_equal :: (a: Vec2, b: Vec2) -> bool {
	return a.x == b.x && a.y == b.y;
}

[op.-_]
pub vec2_negate :: (a: Vec2) -> Vec2 {
	return { x: -a.x, y: -a.y, };
}

pub dot :: (a: &Vec2, b: &Vec2) -> f32 {
	return a.x * b.x + a.y * b.y;
}

# divides the caller's vector by its squared length, in place (there is no
# square root in the standard library yet).
pub normalize :: (mut v: &Vec2) {
	let length_squared = dot(&v, &v);
	if length_squared == 0 return;
	v = v * (1 / length_squared);
}
