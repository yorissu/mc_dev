
/// the mc command line (see cli.c).

#include "mc/cli.h"
#include "mc/platform.h"

int main(const int argc, const char* argv[]) {
	mc_binary_stream(stdout);
	mc_binary_stream(stderr);
	return mc_cli_main(argc, argv);
}
