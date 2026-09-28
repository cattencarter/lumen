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

def code_only(text):
    """The source without its comments, so a commented-out send is not counted.
    Strings are kept whole (raw ones too), and newlines are kept, so line
    numbers still point at the right place."""
    token = re.compile(r'(?P<raw>(?<!\w)R"(?P<d>[^(\s"\\]*)\(.*?\)(?P=d)")'
                       r'|(?P<str>"(?:\\.|[^"\\\n])*")'
                       r"|(?P<chr>'(?:\\.|[^'\\\n])*')"
                       r'|(?P<com>//[^\n]*|/\*.*?\*/)', re.S)
    return token.sub(lambda m: re.sub(r'[^\n]', '', m.group(0)) if m.group("com")
                     else m.group(0), text)

# Every command the viewer sends, from EVERY call site. The command is the text
# before the first pipe, and it reaches viewerToLSL in three shapes: a literal
# ("worn|..."), llformat("ExternalIntegration|%d|%d", ...), or a constant
# declared in the same file (fsradar's prefix = "getZOffsets|"). A send this
# cannot read is a failure, not a skip: a check that quietly misses a command
# is how the radar altitudes could stop after a merge with this still passing.
asked  = set()
unread = []
for cpp in sorted(viewer.glob("*.cpp")):
    src = code_only(cpp.read_text(encoding="utf-8", errors="replace"))
    for pat in (r'\bviewerToLSL\(\s*', r'\bupdateBoolSettingValue\(\s*'):
        for m in re.finditer(pat, src):
            if src[max(0, m.start() - 2):m.start()] == "::":
                continue   # the definition, not a call
            rest = src[m.end():]
            name = None
            lit = re.match(r'(?:llformat\(\s*)?"([^"|]*)', rest)
            if lit:
                name = lit.group(1)
            else:
                ident = re.match(r'([A-Za-z_]\w*)\s*[+,)]', rest)
                if ident:
                    var = ident.group(1)
                    decl = re.search(r'\b' + var + r'\s*(?:=|\{|\()\s*"([^"|]*)', src)
                    if decl:
                        name = decl.group(1)
                    elif cpp.name == "fslslbridge.cpp" and var == "msgVal":
                        # updateBoolSettingValue() forwards its argument as the
                        # command name; its callers are read by the second pattern.
                        continue
            if name and name.strip():
                asked.add(name.strip())
            else:
                unread.append("%s:%d" % (cpp.name, src.count("\n", 0, m.start()) + 1))

answers = set(re.findall(r'cmd == "([^"]+)"', lsl))

fails = []
for site in unread:
    fails.append("cannot tell which command %s sends -- NOT CHECKED. Build it from a literal, "
                 "or teach this check where its name comes from" % site)
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
