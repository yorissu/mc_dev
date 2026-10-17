
# option: a value that may be missing.

pub Option<T> :: {
	let ok: bool = false;
	let value: T = {};
}

pub some<T> :: (value: T) -> Option<T> {
	return { ok: true, value: value, };
}

pub none<T> :: () -> Option<T> {
	return {};
}

pub is_some<T> :: (option: &Option<T>) -> bool {
	return option.ok;
}

pub unwrap<T> :: (option: &Option<T>) -> T {
	assert(option.ok, "unwrap of an empty option");
	return option.value;
}

pub value_or<T> :: (option: &Option<T>, fallback: T) -> T {
	if option.ok {
		return option.value;
	}
	return fallback;
}
