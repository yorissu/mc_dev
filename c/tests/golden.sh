#!/bin/sh

# golden tests: runs every program of tests/programs through the compiler and
# compares what it prints with the expectations stored beside it.
#
#   tests/golden.sh <mc> [--update] [name...]
#
# a program is a directory with 'main.mc' (and the modules it imports). its
# optional file 'options' holds lines 'commands: check run dis mcb' (the
# default is 'check run'; 'mcb' builds a bytecode file and runs it),
# 'args: ...' (arguments after the file for 'run') and 'flags: ...' (more
# options for every command). 'expected' holds what
# each command printed: its exit status, stdout and stderr (the short
# format, without color). --update rewrites the expectations instead of
# comparing; review the diff before committing it.

set -u

if [ $# -lt 1 ]; then
	echo "usage: tests/golden.sh <mc> [--update] [name...]" >&2
	exit 2
fi

case "$1" in
	/*) mc="$1" ;;
	*) mc="$(pwd)/$1" ;;
esac
shift
update=0
if [ $# -gt 0 ] && [ "$1" = "--update" ]; then
	update=1
	shift
fi

here="$(cd "$(dirname "$0")" && pwd)"
programs="$here/programs"
stdlib="../../../../stdlib"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/mc_golden_XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

# appends a section with the text of a file, marking a missing final newline.
section() {
	if [ -s "$2" ]; then
		echo "-- $1"
		cat "$2"
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

# the output of every command of one program, as the expected file holds it.
describe() {
	commands="$(option commands)"
	[ -n "$commands" ] || commands="check run"
	flags="$(option flags)"
	args="$(option args)"
	for command in $commands; do
		extra=""
		case "$command" in run|mcb) extra="--limit 1000000" ;; esac
		# shellcheck disable=SC2086
		if [ "$command" = "mcb" ]; then
			# build, then run the bytecode file
			"$mc" build --format short --no-color --stdlib "$stdlib" $flags -o "$scratch/program.mcb" main.mc > "$scratch/out" 2> "$scratch/err" < /dev/null \
				&& "$mc" run --format short --no-color $extra $flags "$scratch/program.mcb" $args > "$scratch/out" 2> "$scratch/err" < /dev/null
		elif [ "$command" = "run" ]; then
			"$mc" "$command" --format short --no-color --stdlib "$stdlib" $extra $flags main.mc $args > "$scratch/out" 2> "$scratch/err" < /dev/null
		else
			"$mc" "$command" --format short --no-color --stdlib "$stdlib" $flags main.mc > "$scratch/out" 2> "$scratch/err" < /dev/null
		fi
		status=$?
		echo "== $command (status $status)"
		section stdout "$scratch/out"
		section stderr "$scratch/err"
	done
}

passed=0
failed=0
names="$*"
[ -n "$names" ] || names="$(ls "$programs")"
for name in $names; do
	directory="$programs/$name"
	[ -f "$directory/main.mc" ] || continue
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
