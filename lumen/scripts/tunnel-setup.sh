#!/bin/sh
# Point OpenAI's Secure MCP Tunnel at Lumen, so ordinary ChatGPT chat can reach
# it. Read chatgpt/tunnel/README.md first -- this changes what leaves your
# machine, and for most people Claude Desktop is the better answer.
#
#   scripts/tunnel-setup.sh <tunnel_id>
#
# Needs CONTROL_PLANE_API_KEY in the environment. It is never written to disk by
# this script and never printed.

set -eu

[ $# -eq 1 ] || { echo "usage: scripts/tunnel-setup.sh <tunnel_id>" >&2; exit 2; }
TUNNEL_ID=$1
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BRIDGE="$ROOT/chatgpt/plugins/second-life/second-life-bridge.sh"

command -v tunnel-client >/dev/null 2>&1 || {
    echo "tunnel-client is not installed." >&2
    echo "  brew install openai/tools/tunnel-client" >&2
    echo "Do not install it by downloading a release zip: those are not notarised," >&2
    echo "and working around Gatekeeper with xattr or spctl is not the fix." >&2
    exit 1
}
[ -x "$BRIDGE" ] || { echo "No bridge at $BRIDGE" >&2; exit 1; }
[ -n "${CONTROL_PLANE_API_KEY:-}" ] || {
    echo "Set CONTROL_PLANE_API_KEY first (a runtime API key from OpenAI Platform)." >&2
    echo "  export CONTROL_PLANE_API_KEY='...'" >&2
    exit 1
}

echo
echo "This will run:"
echo "    tunnel-client init --profile lumen --tunnel-id $TUNNEL_ID \\"
echo "                       --mcp-command \"/bin/sh $BRIDGE\""
echo "    tunnel-client doctor --profile lumen --explain"
echo "    tunnel-client run    --profile lumen"
echo
echo "The bridge is the same one Codex uses. Your viewer's endpoint stays on"
echo "127.0.0.1 -- the tunnel carries the bridge, not the port."
echo
printf "Go ahead? [y/N] "
read -r answer
case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing was changed."; exit 0 ;; esac

tunnel-client init --profile lumen --tunnel-id "$TUNNEL_ID" \
                   --mcp-command "/bin/sh $BRIDGE"
tunnel-client doctor --profile lumen --explain

echo
echo "Now leave this running. In ChatGPT: Plugins, create a developer-mode app,"
echo "Connection: Tunnel, and pick this one."
echo
exec tunnel-client run --profile lumen
