#!/usr/bin/env python3
"""Does the Lumen bridge still answer everything the viewer asks it?

Lumen carries its own LSL bridge instead of Firestorm's, because theirs puts
their name on the user's avatar and its licence could not be established. Ours
implements the same command set, and ten files in the viewer depend on it:
Area Search, the radar's altitudes, flight assist, movelock, the RLV attachment
rules and the inventory protections all speak that protocol.

**The risk this check exists for is a merge.** Firestorm adds a command, we
merge their source, the viewer starts sending something our script has never
heard of, and nothing errors -- the feature simply goes quiet. That is the
failure shape this project meets more than any other, and a note in a document
does not catch it. This does, and it needs no viewer and no network.

Run it after every upstream merge, and before believing the bridge is fine.
"""
import re, sys, pathlib

root   = pathlib.Path(__file__).resolve().parents[2]
viewer = root / "indra/newview"
script = viewer / "fs_resources/lumen_bridge.lsltxt"

if not script.exists():
    print("  NOT CHECKED  %s is missing" % script.name)
    sys.exit(1)

lsl = script.read_text(encoding="utf-8", errors="replace")

# Every command the viewer sends. They are all built as "name|" or bare, so the
# first token before a pipe or a quote is the command.
asked = set()
for cpp in viewer.glob("*.cpp"):
    if cpp.name.startswith("fslslbridge"):
        continue
    for m in re.finditer(r'viewerToLSL\(\s*"([^"|]+)', cpp.read_text(encoding="utf-8", errors="replace")):
        asked.add(m.group(1).strip())
# fslslbridge sends these two itself, and they are part of the handshake.
asked |= {"URL Confirmed"}
for m in re.finditer(r'viewerToLSL\(\s*"([^"|]+)', (viewer / "fslslbridge.cpp").read_text(encoding="utf-8", errors="replace")):
    asked.add(m.group(1).strip())
# updateBoolSettingValue() builds the command name from its argument.
for m in re.finditer(r'updateBoolSettingValue\(\s*"([^"]+)"', (viewer / "llviewercontrol.cpp").read_text(encoding="utf-8", errors="replace")):
    asked.add(m.group(1).strip())

answers = set(re.findall(r'cmd == "([^"]+)"', lsl))

fails = []
for cmd in sorted(asked - answers):
    fails.append('the viewer sends "%s" and the bridge does not answer it '
                 "(the feature goes quiet, with nothing saying why)" % cmd)

# The script-info reply is a positional contract: fslslbridge.cpp decodes
# certain fields as base64 BY INDEX, so a reordering breaks it silently and
# the panel fills with mojibake rather than erroring.
#
# Deriving those positions from the LSL by regex was tried and was worse than
# useless -- it produced confident wrong numbers, which is the one thing a
# check must never do. So the script STATES its contract in a marker line and
# this compares the viewer's numbers against that. If a merge changes the
# parser, this fails and a person re-reads both. That is the honest amount of
# certainty available here.
parser = (viewer / "fslslbridge.cpp").read_text(encoding="utf-8", errors="replace")
line = ""
for l in parser.splitlines():
    if l.count("scriptInfoArrayCount ==") > 2:
        line = l
        break
want = sorted(set(int(x) for x in re.findall(r'scriptInfoArrayCount == (\d+)', line)))

claim = re.search(r'BASE64-FIELDS:\s*([0-9,\s]+)', lsl)
if not want:
    fails.append("could not find the base64 field list in fslslbridge.cpp -- NOT CHECKED")
elif not claim:
    fails.append("the script does not state its BASE64-FIELDS contract")
else:
    have = sorted(int(x) for x in claim.group(1).replace(" ", "").strip(",").split(","))
    if have != want:
        fails.append("getScriptInfo base64 positions drifted: the viewer now decodes %s, "
                     "the script says it packs %s -- re-read both, the panel shows mojibake "
                     "rather than failing" % (want, have))

print("  viewer asks %d commands | the bridge answers %d" % (len(asked), len(answers)))
if fails:
    print()
    for f in fails:
        print("  FAIL  " + f)
    sys.exit(1)
print("  the bridge answers everything the viewer asks")
