#!/usr/bin/env python3
"""Write this machine's absolute path to the bridge into the plugin manifests.

The manifests shipped with a relative command -- `./second-life-bridge.sh` with
`cwd: "."` -- which is the documented idiom and works only if the host sets the
working directory. ChatGPT's desktop app does not, so the server never started
and the plugin appeared with no tools and no error.

An absolute path always works and is inherently machine-specific, so it is
stamped rather than committed. bootstrap-mac.sh runs this; run it by hand after
moving the checkout.
"""
import json, pathlib, sys

root = pathlib.Path(__file__).resolve().parent.parent
bridge = root / "chatgpt" / "plugins" / "second-life" / "second-life-bridge.sh"
if not bridge.is_file():
    sys.exit(f"no bridge at {bridge}")

# Generate each manifest from its .template, so the real path -- which contains
# the user's home directory -- exists only on this machine. The templates carry
# __BRIDGE_ABSOLUTE_PATH__ and are the tracked files; the stamped ones are
# gitignored. This was not always so: the stamped manifests were committed once,
# carrying the author's username into the repository.
for name in (".mcp.json", "mcp.json"):
    p = bridge.parent / name
    template = bridge.parent / (name + ".template")
    if template.is_file():
        p.write_text(template.read_text().replace("__BRIDGE_ABSOLUTE_PATH__", str(bridge)))
    if not p.is_file():
        continue
    d = json.loads(p.read_text())
    srv = d["mcpServers"]["second-life"]
    srv["command"] = "/bin/sh"
    srv["args"] = [str(bridge)]
    srv.pop("cwd", None)          # nothing to be relative to any more
    p.write_text(json.dumps(d, indent=2) + "\n")
    print(f"  {name}: /bin/sh {bridge}")
