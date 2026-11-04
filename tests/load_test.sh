#!/usr/bin/env bash
#
# load_test.sh - Simulate many students using SQMS at the same time.
#
# Every simulated student is a separate sqms_client process, so N students
# are N independent TCP clients hitting the server concurrently. Each one:
#   1. registers            POST /register   (gets a session cookie)
#   2. asks a question      POST /ask        (302 -> /query?id=N)
#   3. polls for the draft  GET /api/query   (until LLM_ANSWERED)
#
# Usage (with the server already running in another terminal):
#   bash tests/load_test.sh [N]          default N = 20
#   HOST=10.0.0.5 PORT=8080 bash tests/load_test.sh 50
#
# Do not set RATE_LIMIT on the server for this test: all simulated students
# come from the same IP address and would be rate limited.

set -u
N=${1:-20}
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8080}
TIMEOUT=${TIMEOUT:-180}

cd "$(dirname "$0")/.." || exit 1
CLIENT=./sqms_client
[ -x "$CLIENT" ] || { echo "sqms_client not found: run 'make' first"; exit 1; }

OUT=tests/out
rm -rf "$OUT"
mkdir -p "$OUT"
RUN=$(date +%s | tail -c 6)         # makes usernames unique per run

if ! "$CLIENT" -H "$HOST" -p "$PORT" -c "$OUT/probe.cookie" get /status >/dev/null 2>&1; then
    echo "Server not reachable at $HOST:$PORT. Start it first: ./sqms_server"
    exit 1
fi

student() {
    local i=$1
    local user="lt${RUN}_$i"
    local cookie="$OUT/s$i.cookie"
    local err="$OUT/s$i.err"
    local t0 t1 id

    t0=$(date +%s.%N)
    "$CLIENT" -H "$HOST" -p "$PORT" -c "$cookie" register "$user" "secret$i" >/dev/null 2>"$err" \
        || { echo "FAIL $i register"; return; }

    id=$("$CLIENT" -H "$HOST" -p "$PORT" -c "$cookie" \
            ask "Student $i asks: what is the TCP three-way handshake?" 2>>"$err" \
         | grep -o '#[0-9]*' | tr -d '#')
    [ -n "$id" ] || { echo "FAIL $i ask"; return; }

    "$CLIENT" -H "$HOST" -p "$PORT" -c "$cookie" wait "$id" LLM_ANSWERED "$TIMEOUT" \
        >/dev/null 2>>"$err" || { echo "FAIL $i wait #$id"; return; }

    t1=$(date +%s.%N)
    echo "OK $i $id $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }')"
}

echo "Starting $N simulated students against $HOST:$PORT at the same moment..."
START=$(date +%s.%N)
for i in $(seq 1 "$N"); do
    student "$i" > "$OUT/r$i.txt" &
done
wait
END=$(date +%s.%N)

cat "$OUT"/r*.txt | awk -v total="$N" -v start="$START" -v end="$END" '
    $1 == "OK"   { ok++; sum += $4; if ($4 > max) max = $4 }
    $1 == "FAIL" { fail++; print "  " $0 }
    END {
        printf "\nStudents:          %d\n", total
        printf "Succeeded:         %d\n", ok
        printf "Failed:            %d\n", fail
        if (ok > 0) {
            printf "Average time:      %.2f s (register + ask + wait for AI draft)\n", sum / ok
            printf "Slowest student:   %.2f s\n", max
        }
        printf "Whole test:        %.2f s\n", end - start
        exit (ok == total) ? 0 : 1
    }'
STATUS=$?

echo
echo "Details: $OUT/  (per-student results and error output)"
echo "Server side: see server.log, or open /status in the browser."
exit $STATUS
