#!/bin/sh

# golden tests: runs every program of tests/programs through the compiler and
# compares what it prints with the expectations stored beside it.
#
#   tests/golden.sh <mia> [--update] [name...]
#
# a program is a directory with 'main.mia' (and the modules it imports). its
# optional file 'options' holds lines
#
#   commands: check run ast miab   commands run on main.mia: any command of mia that takes
#                                 a file; 'miab' builds a bytecode file and runs it.
#                                 the default is 'check run' (none when there are 'invoke' lines)
#   invoke: explain E0502         any other command line of mia, run as it is (repeatable)
#   args: a b c                   the program's command line, after the file of 'run'
#   flags: -P @examples@
#                                 more options for every command; '@plugins@' and
#                                 '@examples@' are the directories of the test and the
#                                 example plugins built beside <mia> (tests/plugins,
#                                 examples/plugins), '@shared@' the file ending of plugins
#
# 'expected' holds what each command printed: its exit status, stdout and
# stderr (the short format, without color). --update rewrites the
# expectations instead of comparing; review the diff before committing it.

set -u

if [ $# -lt 1 ]; then
	echo "usage: tests/golden.sh <mia> [--update] [name...]" >&2
	exit 2
fi

case "$1" in
	/*) mia="$1" ;;
	*) mia="$(pwd)/$1" ;;
esac
shift
update=0
if [ $# -gt 0 ] && [ "$1" = "--update" ]; then
	update=1
	shift
fi

here="$(cd "$(dirname "$0")" && pwd)"
programs="$here/programs"
stdlib="$(dirname "$mia")/stdlib"
plugins="$(dirname "$mia")/tests/plugins"
examples="$(dirname "$mia")/examples/plugins"
if command -v cygpath > /dev/null 2>&1; then
	# windows (msys, git's sh): mia sees and prints windows paths, so they are what is replaced
	stdlib="$(cygpath -m "$stdlib")"
	plugins="$(cygpath -m "$plugins")"
	examples="$(cygpath -m "$examples")"
fi
case "$(uname -s)" in
	Darwin) shared=".dylib" ;;
	MINGW*|MSYS*|CYGWIN*) shared=".dll" ;;
	*) shared=".so" ;;
esac
# the ending as a pattern of sed ('.' escaped)
shared_pattern="$(printf '%s' "$shared" | sed 's/\./\\./g')"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/mia_golden_XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

# appends a section with the text of a file, marking a missing final newline;
# the standard library and the plugin directories are written as '@stdlib@',
# '@plugins@' and '@examples@', and the ending of a plugin there as '@shared@',
# so the text is the same everywhere.
section() {
	if [ -s "$2" ]; then
		echo "-- $1"
		sed "s|$stdlib|@stdlib@|g; s|$plugins|@plugins@|g; s|$examples|@examples@|g; s|\(@[a-z]*@/[A-Za-z0-9_/]*\)$shared_pattern|\1@shared@|g" "$2"
		if [ "$(tail -c 1 "$2" | od -An -c | tr -d ' ')" != '\n' ]; then
			echo
			echo "-- (no newline at the end)"
		fi
	fi
}

option() {
	if [ -f options ]; then
		sed -n "s/^$1: *//p" options
	fi
}

expand() {
	sed "s|@stdlib@|$stdlib|g; s|@plugins@|$plugins|g; s|@examples@|$examples|g; s|@shared@|$shared|g"
}

# the output of every command of one program, as the expected file holds it.
describe() {
	commands="$(option commands)"
	invokes="$(option invoke | expand)"
	[ -n "$commands" ] || [ -n "$invokes" ] || commands="check run"
	flags="$(option flags | expand)"
	args="$(option args)"
	for command in $commands; do
		extra=""
		case "$command" in run|miab) extra="--limit 1000000" ;; esac
		# shellcheck disable=SC2086
		if [ "$command" = "miab" ]; then
			# build, then run the bytecode file
			"$mia" build --format short --no-color --stdlib "$stdlib" $flags -o "$scratch/program.miab" main.mia > "$scratch/out" 2> "$scratch/err" < /dev/null \
				&& "$mia" run --format short --no-color $extra $flags "$scratch/program.miab" $args > "$scratch/out" 2> "$scratch/err" < /dev/null
		elif [ "$command" = "run" ]; then
			"$mia" "$command" --format short --no-color --stdlib "$stdlib" $extra $flags main.mia $args > "$scratch/out" 2> "$scratch/err" < /dev/null
		else
			"$mia" "$command" --format short --no-color --stdlib "$stdlib" $flags main.mia > "$scratch/out" 2> "$scratch/err" < /dev/null
		fi
		status=$?
		echo "== $command (status $status)"
		section stdout "$scratch/out"
		section stderr "$scratch/err"
	done
	if [ -n "$invokes" ]; then
		echo "$invokes" | while IFS= read -r line; do
			# shellcheck disable=SC2086
			"$mia" --format short --no-color --stdlib "$stdlib" $flags $line > "$scratch/out" 2> "$scratch/err" < /dev/null
			status=$?
			echo "== invoke $line (status $status)" | sed "s|$stdlib|@stdlib@|g; s|$plugins|@plugins@|g; s|$examples|@examples@|g; s|\(@[a-z]*@/[A-Za-z0-9_/]*\)$shared_pattern|\1@shared@|g"
			section stdout "$scratch/out"
			section stderr "$scratch/err"
		done
	fi
}

passed=0
failed=0
names="$*"
[ -n "$names" ] || names="$(ls "$programs")"
for name in $names; do
	directory="$programs/$name"
	[ -f "$directory/main.mia" ] || continue
	cd "$directory" || exit 2
	describe > "$scratch/actual"
	if [ $update -eq 1 ]; then
		if ! cmp -s "$scratch/actual" expected 2> /dev/null; then
			cp "$scratch/actual" expected
			echo "updated $name"
		fi
		passed=$((passed + 1))
	elif cmp -s "$scratch/actual" expected 2> /dev/null; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		echo "FAILED $name"
		diff -u expected "$scratch/actual" | head -40
	fi
done

if [ $update -eq 1 ]; then
	echo "golden: $passed program(s) written"
else
	echo "golden: $passed passed, $failed failed"
fi
[ $failed -eq 0 ]
