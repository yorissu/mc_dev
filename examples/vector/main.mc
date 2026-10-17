
from vector import *

Point2d :: {
	let mut x: f32;
	let mut y: f32;
}

pub main :: () {
	{
		let mut v = vector_create<int>();
		vector_append(mut &v, &11);
		vector_append(mut &v, &12);
		vector_append(mut &v, &13);
		vector_free(mut &v);
	}
	{
		let mut v = vector_create<Point2d>();
		vector_append(mut &v, &{ x: 10, y: 11, });
		vector_append(mut &v, &{ x: 12, y: 13, });
		vector_append(mut &v, &{ x: 14, y: 15, });
		vector_free(mut &v);
	}
}
