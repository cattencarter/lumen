#!/usr/bin/env python3
"""Every action must be registered, advertised, and dispatched.

Three separate lists have to agree, and they are maintained by hand in three
places in lumenaictl.cpp:

    groupAction()      maps a group + action to a method   -- registered
    *_actions[]        the enum a host is shown            -- advertised
    dispatch()         the handler that does the work      -- dispatched
    *_props[...]       the input schema a host is shown     -- declared

Each pairing has failed in production at least once:

  registered, not dispatched   `fly` and `turn` answered "Method not found"
                               for code plainly in the file, because the shared
                               movement branch was never widened (Findings 38).
                               `lighting` repeated it in the same `if`.
  registered, not advertised   `lighting` worked when called by hand and was
                               invisible to every model, so the assistant told
                               the author it could not change the lighting
                               while the code to do it was running.

  registered, not declared     `set_setting` was registered, advertised,
                               dispatched AND phrased -- all four agreed -- and
                               the assistant still could not use it, because
                               `value` was not in the schema. It reported the
                               gap accurately: "the tool doesn't have a direct
                               way to specify a numeric value."
                               Then `camera` repeated it with FIVE parameters at
                               once (`shot`, `angle`, `height`, `gaze`,
                               `person`), so a model could only ever get the
                               default body shot and every framing this project
                               tested had been tested by curl.

None of these is a compile error and none shows up at runtime unless somebody
asks for exactly the missing thing. This is a source check, so it needs no
viewer and can run before a build.

**An undeclared parameter does not fail, it is silently ignored** -- passing
`framing` instead of `shot` returns a confident body shot -- which is why this
has to be checked rather than noticed.
"""
import re, sys, pathlib

src = pathlib.Path(__file__).resolve().parents[2] / \
      "indra/newview/lumenaictl.cpp"
s_src = src.read_text(encoding="utf-8", errors="replace")
s = s_src

ga = s[s.index("std::string groupAction"):s.index("bool isGroup")]
registered = set(re.findall(r'action == "([a-z_]+)"', ga))
methods    = set(re.findall(r'return "([a-z_]+)";', ga))

advertised = set()
for m in re.finditer(r'static const char\* const \w+_actions\[\] =\s*\{(.*?)\};', s, re.S):
    advertised |= set(re.findall(r'"([a-z_]+)"', m.group(1)))

dispatched = set(re.findall(r'method == "([a-z_]+)"', s))


# A hand-written length beside a hand-written array is its own failure, and it
# is invisible to the comparison above: `actionProperty(view_actions, 7, ...)`
# against an array of eight advertised seven of them, so `lighting` existed,
# dispatched, and was unreachable by any model. The array is right, the list is
# right, and the tool surface is still wrong.
counts = []
# A SIXTH list, and the one that let `build` through: isGroup() decides whether
# a tool name is accepted at all, before dispatch is ever reached. A group
# missing from it is advertised to the model, reached for correctly, and then
# refused with nothing useful said -- which is what the author saw.
#
# Decisions 97 again, one layer further out: a consistency check is only as
# wide as the thing it compares, and the interesting failure lives in whatever
# it does not look at.
ig = s_src[s_src.index("bool isGroup"):]
ig = ig[:ig.index("}")]
accepted = set(re.findall(r'name == "([a-z]+)"', ig))
# scoped to the function that builds the tool list, or an unrelated
# ["name"] elsewhere in the file is read as a tool.
tl = s_src[s_src.index("LLSD tools = LLSD::emptyArray();"):s_src.index("return tools;")]
declared = set(re.findall(r'\w+\["name"\] = "([a-z]+)";', tl))
for g in sorted(declared - accepted):
    counts.append("group %s is advertised but isGroup() rejects it "
                    "(the model reaches for it and is refused)" % g)
for g in sorted(accepted - declared):
    counts.append("isGroup() accepts %s but no tool declares it" % g)

for g in re.finditer(r'static const char\* const (\w+)_actions\[\] =\s*\{(.*?)\};', s_src, re.S):
    name, body = g.group(1), g.group(2)
    n = len(re.findall(r'"[a-z_]+"', body))
    call = re.search(r'actionProperty\(%s_actions,\s*([^,]+),' % name, s_src)
    if not call:
        counts.append("%s_actions is never advertised at all" % name)
        continue
    arg = call.group(1).strip()
    if arg.isdigit() and int(arg) != n:
        counts.append("%s_actions has %d entries but %s are advertised"
                      % (name, n, arg))
    elif arg.isdigit():
        counts.append("%s_actions advertises a hand-written %s; use LL_ARRAY_SIZE"
                      % (name, arg))

# A FOURTH list, added after the author saw "chat.read_history" in the
# assistant's status bar. humanAction() in lumenaichat.cpp turns an action into
# the phrase shown while it runs, and its fallback is the raw group.action --
# chosen deliberately as "honest and ugly beats silent, and a standing reminder
# to add a phrase". A standing reminder nobody is shown is not one: eight
# actions had reached the user that way, two of them weeks old.
chat_src = src.parent / "lumenaichat.cpp"
phrased = set()
if chat_src.exists():
    hs = chat_src.read_text(encoding="utf-8", errors="replace")
    i = hs.find("std::string humanAction")
    if i >= 0:
        block = hs[i:hs.index("std::string toolLabel", i)]
        phrased = set(re.findall(r'action == "([a-z_]+)"', block))

# A FIFTH list: every parameter a handler READS must be declared in some
# tool's input schema, or no model can pass it. Read from the source rather
# than a live tools/list, so this still runs with no viewer.
declared = set(re.findall(r'\w+_props\["([a-z_0-9]+)"\]', s_src))
used = set(re.findall(r'params\.has\("([a-z_0-9]+)"\)', s_src)) | \
       set(re.findall(r'params\["([a-z_0-9]+)"\]', s_src))
# Not tool parameters: MCP's own envelope, which the server reads off the
# request rather than off the tool call.
PROTOCOL = {"arguments", "capabilities", "name", "protocolVersion", "clientInfo"}
undeclared = sorted(used - declared - PROTOCOL)

fails = list(counts)
for p in undeclared:
    fails.append("a handler READS `%s` but no tool declares it  "
                 "(a model cannot pass it, and passing it wrongly is ignored "
                 "in silence)" % p)
# A SIXTH thing, and it is not about actions at all -- it lives here because
# this is the check that needs no viewer and runs before a build.
#
# **`near` is a macro in the Windows SDK** (`#define near` in minwindef.h,
# a survivor of 16-bit segmented memory), so `const std::string near = ...`
# compiles to `const std::string = ...` there and every later use of the name
# vanishes. It builds perfectly on macOS. On Windows it produced 27 errors
# across two files, none of which mentions a macro -- "no variable declared
# before '='", "function does not take 0 arguments", "syntax error: '.'" --
# and it cost **an hour and a half of CI on a runner billed at double rate**
# before anybody saw them.
#
# Only identifiers count: the same words in prose are what comments are for.
WIN_MACROS = ["near", "far", "small", "IN", "OUT", "CONST", "interface"]

def code_only(text):
    """Strip // and /* */ comments and string literals, keeping line count."""
    out, i, n = [], 0, len(text)
    in_block = in_str = in_chr = False
    while i < n:
        c = text[i]
        two = text[i:i+2]
        if in_block:
            if two == "*/": in_block = False; out.append("  "); i += 2; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if in_str:
            if c == "\\": out.append("  "); i += 2; continue
            if c == '"': in_str = False
            out.append(" "); i += 1; continue
        if in_chr:
            if c == "\\": out.append("  "); i += 2; continue
            if c == "'": in_chr = False
            out.append(" "); i += 1; continue
        if two == "/*": in_block = True; out.append("  "); i += 2; continue
        if two == "//":
            while i < n and text[i] != "\n": out.append(" "); i += 1
            continue
        if c == '"': in_str = True; out.append(" "); i += 1; continue
        if c == "'": in_chr = True; out.append(" "); i += 1; continue
        out.append(c); i += 1
    return "".join(out)

# **And the narrowing MSVC calls an error and clang does not mention.**
# `LLSD::size()` returns `size_t`. Assigning that, or arithmetic on it, to an
# `S32` is C4267 on Windows, where warnings are errors -- and it cost a SECOND
# two-hour CI round after the `near` one, for a single line.
#
# Deliberately only the ASSIGNMENT shape, not comparisons. A first version
# flagged both and cried wolf on `const S32 cap = (word.size() <= 5) ? 1 : 2;`,
# which is a comparison and perfectly fine. A check with a false positive in a
# codebase this size is a check people learn to scroll past.
# What decides it is what FOLLOWS the call. `x.size() - limit` is a size_t and
# narrows; `x.size() <= 5` is a bool and does not. So the tail of the statement
# must be free of comparison and ternary operators -- which is what separates
# the line that broke the build from the one that never could.
NARROWING = re.compile(
    r"\b(?:S32|int)\s+\w+\s*=\s*(?!\s*\((?:S32|int)\))[^;]*?\.(?:size|length)\(\)"
    r"(?P<tail>[^;]*);")

ours = sorted(src.parent.glob("lumenai*.cpp")) + sorted(src.parent.glob("lumenai*.h")) \
     + sorted(src.parent.glob("lumen*.cpp")) + sorted(src.parent.glob("lumen*.h"))
for f in ours:
    stripped = code_only(f.read_text(encoding="utf-8", errors="replace"))
    for num, line in enumerate(stripped.split("\n"), 1):
        m = NARROWING.search(line)
        if m and not re.search(r"[<>?]|==|!=", m.group("tail")):
            fails.append("%s:%d assigns a `.size()` to an S32 without a cast -- "
                         "size_t narrows, which clang ignores and MSVC makes an "
                         "ERROR (C4267) two hours into a Windows build"
                         % (f.name, num))
        for w in WIN_MACROS:
            if re.search(r"\b%s\b" % w, line):
                fails.append("%s:%d uses `%s`, which is a MACRO in the Windows SDK -- "
                             "it compiles here and breaks there, in errors that never "
                             "name it" % (f.name, num, w))

if phrased:
    for a in sorted(advertised - phrased):
        fails.append("advertised but has NO status phrase: %s  "
                     "(the assistant shows the raw name, which reads as debug output)" % a)
else:
    fails.append("could not read humanAction() in lumenaichat.cpp -- status phrases NOT CHECKED")

for a in sorted(registered - advertised):
    fails.append("registered but NOT advertised: %s  (no model can see it)" % a)
for a in sorted(advertised - registered):
    fails.append("advertised but NOT registered: %s  (host offers what does not map)" % a)
for m in sorted(methods - dispatched):
    fails.append("mapped but NOT dispatched: %s  (answers Method not found)" % m)

# ---------------------------------------------------------------------------
# Every XUI file must PARSE, and no comment may contain a double hyphen.
#
# Written after doing it six times in one session. XML forbids `--` inside a
# comment, so an em dash in an explanation makes the whole panel fail to load --
# and the viewer does not say so usefully: the window simply comes up missing
# whatever that file described. It is the failure shape this project keeps
# recording, arriving as an absence, and it costs a build every time.
#
# Resolving to be careful did not work. A check does.
import xml.etree.ElementTree as ET

xui = src.parent / "skins" / "default" / "xui" / "en"
for p in sorted(xui.rglob("*.xml")):
    try:
        ET.parse(p)
    except ET.ParseError as e:
        fails.append("XUI does not parse: %s  (%s)" % (p.name, e))

print("  registered %d | advertised %d | methods %d | dispatched %d | phrased %d | params %d"
      % (len(registered), len(advertised), len(methods), len(dispatched), len(phrased),
         len(declared)))
if fails:
    print()
    for f in fails:
        print("  FAIL  " + f)
    sys.exit(1)
print("  all six lists agree, no Windows-reserved identifier, and every English XUI file parses")
