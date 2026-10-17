
/// entry point of the unit tests. every '*_tests.c' file in this directory
/// registers its tests with cairo; this file only runs them.

#include "cairo.h"

int main(int argc, const char* argv[]) {
	return cairo_tests_run(cairo_args_new(argc, argv));
}
