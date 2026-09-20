#!/usr/bin/env python3
"""Regenerate the extension manifest's tool list from the running viewer.

**Two scripts in this repo disagreed about what this array is FOR**, and only
one of them carried evidence. This one used to say it was display metadata --
"the bridge forwards tools/list, so the model never sees it" -- while
build-mcpb.py says the opposite and says how it was learned: the viewer
offered twenty tools and the installed extension kept showing three.

Nobody has opened Claude Desktop and settled it, so this now writes the FULL
description and the FULL input schema. Harmless if the array really is only a
page of blurbs; essential if it is not. The previous version wrote a name and
one sentence, which left four tools declaring **no parameters at all** against
a live surface of fifty-six actions -- the `value` and `shot` bug one layer
out, where nothing can call anything.

Start Lumen with the endpoint on, then run this.
"""
import json, pathlib, sys, urllib.request

port = sys.argv[1] if len(sys.argv) > 1 else "8787"
req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp",
                             data=json.dumps({"jsonrpc": "2.0", "id": 1,
                                              "method": "tools/list"}).encode(),
                             method="POST")
req.add_header("Content-Type", "application/json")
tools = json.loads(urllib.request.urlopen(req, timeout=15).read())["result"]["tools"]

path = pathlib.Path(__file__).resolve().parent.parent / "claude" / "mcpb" / "manifest.json"
manifest = json.loads(path.read_text())
def entry(t):
    out = {"name": t["name"], "description": t["description"]}
    schema = t.get("inputSchema") or t.get("input_schema")
    if schema:
        # Both spellings, because the manifest format and MCP itself do not
        # agree, and writing one of them is how this goes quietly wrong again.
        out["input_schema"] = schema
        out["inputSchema"] = schema
    return out

manifest["tools"] = [entry(t) for t in tools]
path.write_text(json.dumps(manifest, indent=2) + "\n")
print(f"{len(tools)} tools written to {path.name}")
for t in manifest["tools"]:
    props = (t.get("input_schema") or {}).get("properties", {})
    acts = len(props.get("action", {}).get("enum", []))
    print(f"  {t['name']:10} {acts:2d} actions, {len(props):2d} parameters, "
          f"{len(t['description']):5d} chars")
    if not acts:
        print("       WARNING: no actions declared -- the viewer answered without a schema")
