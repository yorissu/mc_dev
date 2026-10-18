
# result: a value, or the error why there is none.

pub Result<T, E> :: {
	let ok: bool = false;
	let value: T = {};
	let error: E = {};
}

pub ok<T, E> :: (value: T) -> Result<T, E> {
	return { ok: true, value: value, };
}

pub fail<T, E> :: (error: E) -> Result<T, E> {
	return { ok: false, error: error, };
}

pub is_ok<T, E> :: (result: &Result<T, E>) -> bool {
	return result.ok;
}
