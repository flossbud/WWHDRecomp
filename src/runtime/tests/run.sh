#!/usr/bin/env bash
# run.sh [OUT_DIR]: build and run the runtime's unit tests that need neither the game nor Cemu
# (write_watch_test.cpp), optimised and not, each with clang++ (or $CXX). Light: fine in the cloud sandbox.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "${1:-$root/build/runtime-tests}" && cd "${1:-$root/build/runtime-tests}" && pwd)
cxx=${CXX:-clang++}
fails=0
for opt in -O2 "-O0 -g"; do
	tag=${opt%% *}; tag=${tag#-}
	$cxx -std=c++20 $opt -Wall -Wextra -Wno-unused-result -pthread -o "$out/write_watch_test-$tag" \
		"$here/write_watch_test.cpp" "$root/src/runtime/write_watch.cpp"
	echo "== write_watch_test $opt"
	"$out/write_watch_test-$tag" || fails=$((fails + 1))
done
[ $fails = 0 ] && echo "runtime tests: all ok" || { echo "runtime tests: $fails build(s) FAILED"; exit 1; }
