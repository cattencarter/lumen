#!/usr/bin/env python3
"""Build the Claude Desktop extension from the current manifest and bridge.

Claude Desktop advertises the tools listed in the package's own manifest -- NOT
the live tools/list from the server. So a viewer that grows new tools does
nothing for anyone until this is rebuilt and reinstalled. That was learned the
hard way: the viewer offered twenty tools and the extension kept showing three,
because it was still the package installed weeks earlier.

Run scripts/sync-manifest.py first, with Lumen running, so the manifest matches
the viewer. Then this. Then reinstall the .mcpb in Claude Desktop.
"""
import json, pathlib, sys, zipfile

root = pathlib.Path(__file__).resolve().parent.parent
manifest_path = root / "claude" / "mcpb" / "manifest.json"
bridge_path = root / "claude" / "bridge.mjs"
manifest = json.loads(manifest_path.read_text())

if len(sys.argv) > 1:
    manifest["version"] = sys.argv[1]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

version = manifest["version"]
out = root / "claude" / f"lumen-{version}.mcpb"

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
    z.writestr("server/bridge.mjs", bridge_path.read_text())

print(f"{out.name}  {out.stat().st_size} bytes")
print(f"  version {version}, {len(manifest['tools'])} tools declared")
for t in manifest["tools"]:
    print(f"    {t['name']}")
