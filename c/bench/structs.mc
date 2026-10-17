
# structs passed by value, operator functions and f32 arithmetic.

from io import println

Vec3 :: {
	let mut x: f32;
	let mut y: f32;
	let mut z: f32;
}

[op._+_]
add :: (a: Vec3, b: Vec3) -> Vec3 {
	return { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z, };
}

[op._*_]
scale :: (a: Vec3, s: f32) -> Vec3 {
	return { x: a.x * s, y: a.y * s, z: a.z * s, };
}

pub main :: () {
	let mut position: Vec3 = { x: 0, y: 0, z: 0, };
	let velocity: Vec3 = { x: 1, y: 2, z: 3, };
	for i in 0..300000 {
		position = position + velocity * 0.001;
		if position.x > 100 {
			position.x = 0;
		}
	}
	println("{}", position);
}
