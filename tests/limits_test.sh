#!/usr/bin/env bash
#
# limits_test.sh - Show the server's protective limits in action.
#
#   bash tests/limits_test.sh connections   more idle connections than
#                                           MAX_CONNECTIONS (64) -> 503
#   bash tests/limits_test.sh ratelimit     rapid requests from one IP -> 429
#                                           (start the server with RATE_LIMIT=5)
#
# HOST and PORT can be set as for load_test.sh.

set -u
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8080}
cd "$(dirname "$0")/.." || exit 1
CLIENT=./sqms_client
[ -x "$CLIENT" ] || { echo "sqms_client not found: run 'make' first"; exit 1; }

status_of() {
    # Print just the status code of GET $1, e.g. "200".
    "$CLIENT" -H "$HOST" -p "$PORT" -c /dev/null get "$1" 2>/dev/null | head -1 | awk '{ print $2 }'
}

case "${1:-}" in
connections)
    COUNT=${2:-70}
    echo "Opening $COUNT TCP connections that never send a request..."
    fds=()
    for i in $(seq 1 "$COUNT"); do
        # bash opens a TCP connection (full three-way handshake) on /dev/tcp.
        exec {fd}<>"/dev/tcp/$HOST/$PORT" || break
        fds+=("$fd")
    done
    echo "Opened ${#fds[@]}. The first 64 each hold a worker thread; the rest were refused."
    sleep 1
    echo "One more real request while the server is full: HTTP $(status_of /status)  (expected 503)"

    echo "Closing the idle connections..."
    for fd in "${fds[@]}"; do
        exec {fd}>&-
    done
    sleep 1
    echo "Same request again:                          HTTP $(status_of /status)  (expected 200)"
    echo "Check /status: 'Rejected (server full)' has gone up."
    ;;
ratelimit)
    COUNT=${2:-30}
    echo "Sending $COUNT requests as fast as possible from one IP..."
    for i in $(seq 1 "$COUNT"); do
        status_of /status
    done | sort | uniq -c | awk '{ printf "  HTTP %s: %d responses\n", $2, $1 }'
    echo "If the server runs with RATE_LIMIT=5 you should see some 429 (Too Many Requests)."
    echo "Without RATE_LIMIT every response is 200."
    ;;
*)
    echo "usage: bash tests/limits_test.sh connections|ratelimit"
    exit 1
    ;;
esac
