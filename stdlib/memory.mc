
# memory: heap allocation. the functions are provided by the 'std' plugin,
# which is built into mc; heap memory is only ever seen through slices, and
# every access through them is checked (bounds, and use after free).

# a slice of 'count' zeroed elements on the heap.
[plugin]
pub alloc<T> :: (count: uint) -> []T;

# a slice of 'count' elements with the old contents; 'data' is invalid afterwards.
[plugin]
pub realloc<T> :: (data: []T, count: uint) -> []T;

# frees the whole allocation that 'data' views; returns an empty slice.
[plugin]
pub free<T> :: (data: []T) -> []T;

# copies min(lenof(target), lenof(source)) elements; returns how many.
[plugin]
pub copy<T> :: (target: []T, source: []T) -> uint;
