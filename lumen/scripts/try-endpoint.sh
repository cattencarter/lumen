#!/bin/sh
# Start the locally built viewer with the control endpoint on, and exercise it.
#
# The first thing to run after a build: does the socket come up, does it answer,
# does it refuse what it must, and does it survive rubbish being thrown at it.
# The protocol and behaviour suites come after (mcp-check.py, tools-check.py,
# write-check.py).
#
# Uses --set, which the viewer documents as overriding all other settings, so
# nothing is written to your Lumen configuration. The endpoint is off by
# default, so neither this build nor the installed one changes behaviour when
# this script is not used.
#
# There is no token, and this script does not invent one. See Decisions 4: a
# secret stored on the same computer cannot defend against software already
# running as you, and requiring one cost every user a setup step they could not
# be expected to understand. What DOES defend the endpoint is that it is bound
# to loopback and refuses anything a browser sends -- and those are the two
# things checked below, because they are the two things that are load-bearing.
#
# Usage:  scripts/try-endpoint.sh [--no-launch]
#
# --no-launch skips starting the viewer, for when one is already running.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
app="$here/build-darwin-universal/newview/Release/Lumen.app"
bin="$app/Contents/MacOS/Lumen"
port=8787
url="http://127.0.0.1:$port/mcp"

if [ ! -x "$bin" ]; then
    echo "No viewer at $bin" >&2
    echo "Build it first: scripts/bootstrap-mac.sh" >&2
    exit 1
fi

if [ "${1:-}" != "--no-launch" ]; then
    echo "Starting the viewer with the endpoint enabled on port $port..."
    "$bin" --set FSAIControlEnabled TRUE \
           --set FSAIControlPort "$port" >/dev/null 2>&1 &
    echo "  pid $!"
    echo
    echo "Waiting for it to listen (the viewer takes a while to get going)..."
    i=0
    while [ "$i" -lt 120 ]; do
        curl -fsS "$url" >/dev/null 2>&1 && break
        i=$((i + 1))
        sleep 1
    done
fi

rpc() { curl -sS -X POST "$url" -H 'Content-Type: application/json' "$@"; }

echo
echo "--- health ---"
curl -sS "$url" || echo "  (not listening)"

echo
echo "--- status: a local caller needs no credential ---"
rpc -d '{"jsonrpc":"2.0","id":1,"method":"status"}'

echo
echo
echo "--- carrying a browser Origin: MUST be refused ---"
echo "    (a page on any site can POST to 127.0.0.1 from your own browser;"
echo "     the browser is obliged to mark the request, and we refuse it)"
rpc -H 'Origin: https://example.com' \
    -d '{"jsonrpc":"2.0","id":2,"method":"status"}'

echo
echo
echo "--- carrying Sec-Fetch-Site: MUST be refused ---"
rpc -H 'Sec-Fetch-Site: cross-site' \
    -d '{"jsonrpc":"2.0","id":3,"method":"status"}'

echo
echo
echo "--- an unknown method: must be a JSON-RPC error, not a crash ---"
rpc -d '{"jsonrpc":"2.0","id":4,"method":"no_such_method"}'

echo
echo
echo "--- malformed JSON: must be a parse error, not a crash ---"
rpc -d '{not json'

echo
echo
echo "--- still alive after all of that ---"
rpc -d '{"jsonrpc":"2.0","id":5,"method":"ping"}'

echo
echo
echo "--- reachable from off this machine? It MUST NOT be ---"
# Ask every interface, not just en0. A Mac on Ethernet, or on a Studio/Pro with
# different interface numbering, has no en0 address at all -- and the old script
# then printed "(no en0 address to test with)" and moved on, silently skipping
# the single most important check in this file.
tested=0
for iface in $(ifconfig -l 2>/dev/null); do
    [ "$iface" = "lo0" ] && continue
    ip=$(ipconfig getifaddr "$iface" 2>/dev/null || true)
    [ -n "$ip" ] || continue
    tested=1
    if curl -fsS --max-time 3 "http://$ip:$port/mcp" >/dev/null 2>&1; then
        echo "  FAIL: answered on $ip ($iface) — it is bound to more than loopback"
    else
        echo "  good: refused on $ip ($iface)"
    fi
done
if [ "$tested" -eq 0 ]; then
    echo "  NOT CHECKED: this machine has no non-loopback address right now."
    echo "  That is not a pass. Re-run it on a network before trusting it."
fi
