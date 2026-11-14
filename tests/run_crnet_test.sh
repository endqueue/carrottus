#!/bin/sh
# Starts tests/crnet_test_server.py, runs crnet_test against it, and
# stops the server again.
#
#   tests/run_crnet_test.sh <path to crnet_test> [part]
#
# Needs python3 and the openssl command. Exits with crnet_test's status.
#
# CRNET_TEST_LAUNCHER, if set, is put in front of the program: for a build
# that runs somewhere other than this shell, but on this machine's loopback
# and file system. The iOS simulator is one:
#
#   CRNET_TEST_LAUNCHER="xcrun simctl spawn booted" \
#       tests/run_crnet_test.sh build-iossimulator-metal/Debug/crnet_test

set -u
if [ $# -lt 1 ]; then
	echo "usage: $0 <path to crnet_test> [instance|bases|records|tcp|wait|resolver|http|tls|proxy|websocket|resumable|internet]" >&2
	exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/crnet_test.XXXXXX")
info="$work/info"

python3 "$here/crnet_test_server.py" "$info" "$work" &
server=$!
trap 'kill $server 2>/dev/null; rm -rf "$work"' EXIT

# The server writes the info file once everything is listening.
tries=0
while [ ! -f "$info" ]; do
	tries=$((tries + 1))
	if [ $tries -gt 100 ] || ! kill -0 $server 2>/dev/null; then
		echo "run_crnet_test: the server didn't start" >&2
		exit 2
	fi
	sleep 0.1
done

program=$1
shift
# Unquoted on purpose: the launcher is a command and its arguments.
${CRNET_TEST_LAUNCHER:-} "$program" "$info" "$@"
