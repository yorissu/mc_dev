#!/bin/sh

# runs every program of bench/ and prints how long each one took (the best
# of three runs, in seconds) and what it printed.
#
#   bench/bench.sh <mia>

set -u
mia="$1"
here="$(cd "$(dirname "$0")" && pwd)"
for program in "$here"/*.mia; do
	best=""
	for _ in 1 2 3; do
		start=$(date +%s%N)
		output="$("$mia" run "$program" 2>&1)"
		end=$(date +%s%N)
		elapsed=$(( (end - start) / 1000000 ))
		if [ -z "$best" ] || [ "$elapsed" -lt "$best" ]; then best=$elapsed; fi
	done
	printf '%-12s %6d ms   %s\n' "$(basename "$program" .mia)" "$best" "$(echo "$output" | head -1)"
done
