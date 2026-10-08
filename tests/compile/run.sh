#!/bin/sh
# Usage: run.sh EXPECT CC ARGS...; an empty EXPECT means the code must compile.
export LC_ALL=C
expect=$1
shift
if [ -z "$expect" ]; then
	exec "$@"
fi
if out=$("$@" 2>&1); then
	echo "compiled, expected: $expect"
	exit 1
fi
case $out in
*"$expect"*) exit 0 ;;
esac
printf '%s\n' "$out"
echo "expected: $expect"
exit 1
