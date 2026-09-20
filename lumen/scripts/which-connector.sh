#!/bin/sh
# Answer "is it actually working, and which part isn't?" without guessing.
#
# Four things have to be true before an assistant can use Lumen, and when it
# cannot, the message you get back is usually about the last one while the fault
# is in an earlier one. This checks each separately and says so.
#
# It reads. It changes nothing, and it never touches a host application's
# configuration.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT="${LUMEN_PORT:-8787}"
URL="http://127.0.0.1:$PORT/mcp"
BRIDGE="$ROOT/chatgpt/plugins/second-life/second-life-bridge.sh"
ok()   { printf '  yes   %s\n' "$*"; }
no()   { printf '  NO    %s\n' "$*"; }
info() { printf '        %s\n' "$*"; }

echo
echo "1. Is the viewer running?"
if pgrep -f 'Lumen.app/Contents/MacOS/Lumen' >/dev/null 2>&1; then
    ok "Lumen is running"
else
    no "Lumen is not running. Start it."
fi

echo
echo "2. Is its endpoint switched on?"
if curl -fsS --max-time 3 "$URL" >/dev/null 2>&1; then
    ok "listening on 127.0.0.1:$PORT"
else
    no "nothing is listening on 127.0.0.1:$PORT"
    info "Preferences > Advanced > 'Allow an AI assistant to control this"
    info "viewer', then restart Lumen. The setting is read at startup."
fi

echo
echo "3. Does the bridge reach it?"
if [ -x "$BRIDGE" ]; then
    reply=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
            | /bin/sh "$BRIDGE" 2>/dev/null || true)
    case "$reply" in
        *UNAVAILABLE*) no "the bridge runs, but the viewer is not reachable (see 2)" ;;
        *'"inventory"'*) ok "four tools, live from the viewer" ;;
        *) no "the bridge answered something unexpected" ; info "$reply" ;;
    esac
else
    no "no bridge at $BRIDGE"
fi

echo
echo "4. Which host applications are configured to use it?"
MCPB=$(ls "$ROOT"/claude/*.mcpb 2>/dev/null | tail -1 || true)
[ -n "$MCPB" ] && info "Claude Desktop: install $(basename "$MCPB")" \
               || info "Claude Desktop: no .mcpb built"
info "ChatGPT: it must be a PLUGIN, not only an MCP server."
info "  A server under 'Servers' cannot be attached in the composer."
info "  Add > Add a marketplace > $ROOT"
echo
echo "The test that cannot lie"
echo "------------------------"
echo "  In the assistant, ask:"
echo
echo "      Use the viewer tool, action status, and tell me the exact"
echo "      session_check it returns."
echo
if curl -fsS --max-time 3 "$URL" >/dev/null 2>&1; then
    body=$(curl -sS --max-time 5 -X POST "$URL" -H 'Content-Type: application/json' \
           -d '{"jsonrpc":"2.0","id":1,"method":"status"}' 2>/dev/null)
    chk=$(printf '%s' "$body" | sed -n 's/.*"session_check":"\([^"]*\)".*/\1/p')
    echo "  It must answer exactly:  $chk"
    echo
    echo "  That string is random and was made when this viewer started. An"
    echo "  assistant that called the tool can repeat it. One that is guessing"
    echo "  cannot -- there is nothing to guess from."
    echo
    echo "  This matters more than it sounds. An assistant has already answered"
    echo "  a question about a worn item confidently and WRONGLY, from earlier"
    echo "  conversation rather than from the viewer. Nothing in the reply said"
    echo "  so. Ask for the check string when the answer matters."
else
    echo "  (start the viewer first, then run this again to see the string)"
fi
echo
