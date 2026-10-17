# io: writing to standard output. the functions are provided by the 'std'
# plugin, which is built into mc.

# writes 'format' with each placeholder replaced by the next value. any value
# can be written: numbers, bools, arrays, slices and structs; arrays and
# slices of u8 are written as text. '{{' and '}}' write braces. the format
# must be known at compile time, and the compiler checks that its
# placeholders match the values:
#
#	println("{} + {} = {}", a, b, a + b);
#
# a placeholder is '{}' or '{:spec}', spec = [[fill]align][sign][#][0][width][.precision][type]:
#
#	align      '<' left, '>' right, '^' center; 'fill' is any printable ascii
#	           character but '{' and '}' (a space by default)
#	sign       '+' writes a sign on positive numbers too, '-' only on negative ones (the default)
#	#          '0x', '0o' or '0b' before integers written in those bases
#	0          pads numbers with zeros after the sign: {:05} writes 00042
#	width      the least number of characters (numbers are right-aligned,
#	           everything else left-aligned, unless 'align' says)
#	precision  floats: digits after the point ('f', 'e') or significant digits ('g')
#	type       integers: 'd' (the default), 'x', 'X', 'o', 'b'; floats: 'f', 'e',
#	           'E', 'g' ('g' without a precision is the default: the shortest text
#	           that reads back as the same number)
#
#	println("{:>8} {:08.3f} {:#x} {:<6}|", "right", 3.14159, 255, true);
#
# a u8 array or slice that comes from a "..."c literal (directly, or through
# a constant or a variable that is not 'mut') is written without the NUL
# that ends it; other bytes are written as they are, a NUL included.
[plugin]
pub print :: (format: []u8, values: ...);

# like print, then a line break.
[plugin]
pub println :: (format: []u8, values: ...);

# writes raw bytes, as given.
[plugin]
pub write :: (bytes: []u8);

# writes raw bytes and a line break.
[plugin]
pub write_line :: (bytes: []u8);
