
# memory: heap allocation. the virtual machine implements these functions;
# heap memory is only ever seen through slices.

# a slice of 'count' zeroed elements on the heap.
[builtin]
pub alloc<T> :: (count: uint) -> []T;

# a slice of 'count' elements with the old contents; 'data' is invalid afterwards.
[builtin]
pub realloc<T> :: (data: []T, count: uint) -> []T;

# frees the whole allocation that 'data' views; returns an empty slice.
[builtin]
pub free<T> :: (data: []T) -> []T;

# copies min(lenof(target), lenof(source)) elements; returns how many.
[builtin]
pub copy<T> :: (target: []T, source: []T) -> uint;
