
#ifndef __mia__cli_h__
#define __mia__cli_h__

/// the command line: 'mia <command> [options] <file>'.

/// runs the command line; returns the exit status.
int mia_cli_main(const int argc, const char* const argv[]);

#endif
