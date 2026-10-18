
/// the mia command line (see cli.c).

#include "mia/cli.h"
#include "mia/system.h"

int main(const int argc, const char* const argv[]) {
	mia_system_binary_stream(stdout);
	mia_system_binary_stream(stderr);
	return mia_cli_main(argc, argv);
}
