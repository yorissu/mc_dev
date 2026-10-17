
# io: writing to standard output.

# writes 'format' with each '{}' replaced by the next value. any value can be
# written: numbers, bools, arrays, slices and structs; arrays and slices of u8
# are written as text. '{{' and '}}' write braces. the format must be known at
# compile time, and the compiler checks that its '{}' match the values:
#
#	println("{} + {} = {}", a, b, a + b);
[builtin]
pub print :: (format: []u8, values: ...);

# like print, then a line break.
[builtin]
pub println :: (format: []u8, values: ...);

# writes raw bytes, as given.
[extern = "mc_write"]
pub write :: (bytes: []u8);

# writes raw bytes and a line break.
[extern = "mc_write_line"]
pub write_line :: (bytes: []u8);
