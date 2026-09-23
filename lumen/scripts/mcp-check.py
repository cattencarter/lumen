#!/usr/bin/env python3
"""Drive the viewer's endpoint as an MCP client would, and report what it does.

This exists so the endpoint can be proven without involving anyone's ChatGPT or
Claude account. It speaks the same sequence a real host speaks — initialize,
the initialized notification, tools/list, tools/call — and checks the shape of
every answer rather than only that something came back.

Standard library only. Nothing to install.

    scripts/mcp-check.py [--port 8787]
"""

import argparse

# The all-zero uuid: names no object, so any handler that takes one refuses
# before it does anything. See the build entries in SAFE below.
NULL_UUID = "00000000-0000-0000-0000-000000000000"
import json
import sys
import urllib.error
import urllib.request

PASS = 0
FAIL = 0


def check(label, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  pass  {label}")
    else:
        FAIL += 1
        print(f"  FAIL  {label}" + (f"  — {detail}" if detail else ""))


def call(url, payload, expect_body=True, headers=None):
    """One JSON-RPC request. Returns (status, parsed_body_or_None)."""
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            raw = r.read()
            if not expect_body:
                return r.status, None
            return r.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except Exception:
            return e.code, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    args = ap.parse_args()

    url = f"http://127.0.0.1:{args.port}/mcp"
    print(f"Driving {url} as an MCP client would.\n")

    # --- the handshake ---------------------------------------------------
    status, body = call(url, {
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp-check", "version": "1"},
        },
    })
    check("initialize answers 200", status == 200, f"got {status}")
    result = (body or {}).get("result", {})
    check("initialize returns a protocolVersion", bool(result.get("protocolVersion")))
    check("initialize declares tools capability", "tools" in result.get("capabilities", {}))
    check("initialize names the server",
          bool(result.get("serverInfo", {}).get("name")), str(result.get("serverInfo")))

    # A notification: no id, and the server must not send a JSON-RPC reply.
    status, body = call(url,
                        {"jsonrpc": "2.0", "method": "notifications/initialized"},
                        expect_body=False)
    check("initialized notification is accepted with no body", status in (200, 202), f"got {status}")

    # --- tools -----------------------------------------------------------
    status, body = call(url, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = (body or {}).get("result", {}).get("tools", [])
    check("tools/list returns tools", len(tools) > 0, str(body)[:120])

    names = [t.get("name") for t in tools]
    # Grouped on purpose: a host asks permission per tool name, so one tool per
    # operation meant twenty-one prompts before anything worked.
    for expected in ("inventory", "chat", "movement", "viewer"):
        check(f"tools/list includes {expected}", expected in names, str(names))
    check("the surface stays small enough to approve", len(tools) <= 6, f"{len(tools)} tools")

    for t in tools:
        acts = t.get("inputSchema", {}).get("properties", {}).get("action", {}).get("enum")
        check(f"{t.get('name')} offers an action enum", bool(acts), str(t.get("name")))

    for t in tools:
        n = t.get("name", "?")
        check(f"{n} has a description a model can act on",
              len(t.get("description", "")) > 40)
        check(f"{n} has an object inputSchema",
              t.get("inputSchema", {}).get("type") == "object")

    # --- calling a tool --------------------------------------------------
    status, body = call(url, {
        "jsonrpc": "2.0", "id": 3, "method": "tools/call",
        "params": {"name": "viewer", "arguments": {"action": "status"}},
    })
    res = (body or {}).get("result", {})
    check("tools/call status succeeds", status == 200 and "content" in res, str(body)[:160])
    content = res.get("content", [])
    check("tools/call returns text content",
          len(content) > 0 and content[0].get("type") == "text")
    check("tools/call also returns structuredContent", "structuredContent" in res)
    sc = res.get("structuredContent", {})
    check("status reports the viewer", bool(sc.get("viewer")), str(sc)[:120])
    check("status reports the startup state", bool(sc.get("startup_state")))
    if sc.get("logged_in"):
        print(f"        logged in as {sc.get('agent_id')} in {sc.get('region')}")
    else:
        print(f"        not logged in yet ({sc.get('startup_state')})")

    # --- a stream --------------------------------------------------------
    status, body = call(url, {
        "jsonrpc": "2.0", "id": 4, "method": "tools/call",
        "params": {"name": "chat", "arguments": {"action": "read_chat", "since": 0}},
    })
    sc = (body or {}).get("result", {}).get("structuredContent", {})
    check("read_chat returns a stream result", "entries" in sc and "latest_seq" in sc, str(sc)[:160])
    check("read_chat reports it is subscribed", sc.get("subscribed") is True, str(sc)[:160])
    print(f"        {len(sc.get('entries', []))} entries held, latest_seq {sc.get('latest_seq')}")

    # --- failures are results, not collapses ------------------------------
    status, body = call(url, {
        "jsonrpc": "2.0", "id": 5, "method": "tools/call",
        "params": {"name": "no_such_tool", "arguments": {}},
    })
    res = (body or {}).get("result", {})
    check("an unknown tool is an error result, not a protocol error",
          res.get("isError") is True, str(body)[:160])

    # A grouped tool with a bad action must fail the same gentle way.
    status, body = call(url, {
        "jsonrpc": "2.0", "id": 51, "method": "tools/call",
        "params": {"name": "inventory", "arguments": {"action": "fly"}},
    })
    res = (body or {}).get("result", {})
    check("an action the group cannot do is an error result, not a crash",
          res.get("isError") is True, str(body)[:160])

    status, body = call(url, {
        "jsonrpc": "2.0", "id": 52, "method": "tools/call",
        "params": {"name": "inventory", "arguments": {}},
    })
    res = (body or {}).get("result", {})
    check("a missing action is an error result that says so",
          res.get("isError") is True, str(body)[:160])

    # Every advertised action must actually resolve to something.
    #
    # **This sweep CALLS each action, and some of them act.** The author watched
    # his avatar take off during a run: `fly` with no `enabled` defaults to true,
    # so a protocol check was flying her into the air, and `lighting` with no
    # `preset` was replacing her sky. It had done that on every run since those
    # actions existed, and nothing said so -- the check passed, and the side
    # effect was invisible unless somebody happened to be looking at the screen.
    #
    # So: a neutral argument for every action that would otherwise do something,
    # chosen to be a no-op rather than a reversal.
    SAFE = {
        ("movement", "fly"):            {"enabled": False},   # landing while landed
        ("movement", "camera"):         {"shot": "reset"},    # Decisions 92
        ("viewer",   "lighting"):       {"preset": "region"}, # the place's own light
        ("viewer",   "answer_while_away"): {"on": False},

        # `build` makes things other people can see, and `remove` deletes
        # whatever is selected -- which, on a run while the user had something
        # selected, would be their own object. Decisions 107 with worse
        # consequences than a flying avatar.
        #
        # There is no harmless rez, so this one is made to refuse: an unknown
        # shape is rejected BY THE HANDLER, which is exactly what this sweep
        # needs to prove -- the action was reached. The null uuid does the same
        # for the other five, because an object_id that cannot be resolved is
        # an error for all of them before anything is touched.
        ("build",    "rez"):            {"shape": "__check_only__"},
        ("build",    "select"):         {"object_id": NULL_UUID},
        ("build",    "set"):            {"object_id": NULL_UUID},
        ("build",    "remove"):         {"object_id": NULL_UUID},
        ("build",    "link"):           {"object_id": NULL_UUID},
        ("build",    "unlink"):         {"object_id": NULL_UUID},

        # There is no harmless landmark either -- every call makes one. A name
        # past Second Life's 63-character limit is refused by the handler
        # before anything is created, which proves the action is reached.
        ("movement", "landmark"):       {"name": "x" * 70},
    }

    # And a net, because the table above is hand-written and the next action
    # with a harmless-looking default will not be in it. Decisions 97 is the
    # same lesson: a list maintained by hand is a list that goes stale.
    def agent_state():
        _s, b = call(url, {"jsonrpc": "2.0", "id": 90, "method": "tools/call",
                           "params": {"name": "viewer", "arguments": {"action": "status"}}})
        try:
            r = (b or {}).get("result", {})
            d = r.get("structuredContent") or json.loads(r["content"][0]["text"])
            return (d.get("flying"), d.get("sitting"),
                    [float(v) for v in d.get("position", [])])
        except Exception:
            return None

    before = agent_state()

    for t in tools:
        acts = t.get("inputSchema", {}).get("properties", {}).get("action", {}).get("enum") or []
        for a in acts:
            args = {"action": a}
            args.update(SAFE.get((t["name"], a), {}))
            status, body = call(url, {
                "jsonrpc": "2.0", "id": 53, "method": "tools/call",
                "params": {"name": t["name"], "arguments": args},
            })
            res = (body or {}).get("result", {})
            text = (res.get("content") or [{}])[0].get("text", "")
            # Two ways an advertised action can be a lie, and the second one
            # cost a build: the group may not map it, OR the group maps it and
            # the handler is never reached, because the dispatch branch that
            # guards those verbs was not widened. The second returns "Method
            # not found" for code that plainly exists.
            unknown = ("is not something" in text
                       or "needs an `action`" in text
                       or "Method not found" in text)
            check(f"{t['name']}/{a} reaches a real handler", not unknown, text[:90])

    after = agent_state()
    if before is None or after is None:
        check("the sweep left the avatar as it found it", False,
              "could not read status before and after -- NOT CHECKED")
    else:
        # Position with a tolerance, not equality. The first version rounded to
        # 10 cm and failed intermittently -- on her own idle animation, which
        # shifts an avatar a few centimetres while she stands still. A check
        # that fails at random is worse than no check, because people learn to
        # ignore it. Flying and sitting stay exact: those are the state changes
        # this is actually watching for, and neither drifts.
        moved = 0.0
        if before[2] and after[2] and len(before[2]) == len(after[2]):
            moved = max(abs(a - b) for a, b in zip(before[2], after[2]))
        same = (before[0] == after[0] and before[1] == after[1] and moved < 0.5)
        check("the sweep left the avatar as it found it", same,
              f"flying {before[0]}->{after[0]} sitting {before[1]}->{after[1]} moved {moved:.2f}m")

    # --- a web page must not be able to drive the viewer ------------------
    # There is no token: the endpoint is loopback-only and every program on
    # this machine runs as the same user anyway. The barrier that does matter
    # is the browser one. A page on any site can POST to 127.0.0.1, but the
    # browser is obliged to mark that request, and these are those marks.
    for header, value in (("Origin", "https://example.com"),
                          ("Sec-Fetch-Site", "cross-site")):
        status, body = call(url,
                            {"jsonrpc": "2.0", "id": 6, "method": "tools/list"},
                            headers={header: value})
        check(f"a request carrying {header} is refused",
              (body or {}).get("error", {}).get("code") == -32001, str(body)[:120])

    # --- a write refuses rather than half-happening -----------------------
    # Only the refusal path is exercised. The success path speaks out loud in
    # whatever region the avatar is standing in, which is not something a test
    # script gets to do to a live session.
    status, body = call(url, {
        "jsonrpc": "2.0", "id": 7, "method": "tools/call",
        "params": {"name": "say", "arguments": {"message": ""}},
    })
    res = (body or {}).get("result", {})
    check("say with an empty message is refused",
          res.get("isError") is True or (body or {}).get("error"), str(body)[:160])

    # --- a capability the description does not mention does not exist ------
    # ChatGPT was asked to find items by creator, answered that "Lumen's
    # inventory connection omits creator metadata", and went looking for
    # permission to read the screen instead. It was wrong -- creator was in
    # every result and was a parameter -- but the description enumerated what
    # search returns and creator was not in the list. An incomplete enumeration
    # does not read as silence; it reads as a denial.
    by_name = {t["name"]: t for t in tools}
    for tool, words in (
        ("inventory", ["creator", "worn", "folder", "notecard"]),
        ("chat",      ["group", "friend", "instant message"]),
        ("movement",  ["fly", "turn", "landmark", "parcel"]),
        ("viewer",    ["read_dialogues", "read_actions"]),
    ):
        d = by_name.get(tool, {}).get("description", "").lower()
        for w in words:
            check(f"{tool} tells the model it can do \"{w}\"", w.lower() in d,
                  f"not mentioned in the description the model reads")

    # --- Streamable HTTP, which is what a URL-based host speaks ------------
    # A ChatGPT custom connector takes a URL, not a command, so the endpoint
    # has to behave like an MCP Streamable HTTP server rather than merely
    # answering JSON-RPC by POST.
    import urllib.request as _u
    def raw(method, headers):
        r = _u.Request(url, method=method)
        for k, v in headers.items():
            r.add_header(k, v)
        try:
            with _u.urlopen(r, timeout=15) as resp:
                return resp.status, dict(resp.headers), resp.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    code, hdrs, _ = raw("GET", {"Accept": "text/event-stream"})
    check("a GET asking for a stream is refused with 405, not a stray document",
          code == 405, f"got {code}")
    check("and it says which method to use",
          hdrs.get("Allow") == "POST", str(hdrs.get("Allow")))

    code, hdrs, body = raw("GET", {"Accept": "application/json"})
    check("a plain GET is still the health check", code == 200 and b'"ok"' in body,
          f"{code} {body[:60]}")

    status, body = call(url, {
        "jsonrpc": "2.0", "id": 60, "method": "initialize",
        "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "mcp-check", "version": "1"}},
    }, headers={"Accept": "application/json, text/event-stream",
                "MCP-Protocol-Version": "2025-06-18"})
    info = (body or {}).get("result", {}).get("serverInfo", {})
    check("initialize works with the Accept header a spec client sends",
          status == 200 and bool(info), str(body)[:120])
    check("the server names itself Lumen, not Firestorm",
          info.get("name") == "lumen", str(info))

    # --- and it is still alive -------------------------------------------
    status, body = call(url, {"jsonrpc": "2.0", "id": 8, "method": "ping"})
    check("still answering after all of that", status == 200, f"got {status}")

    print(f"\n  {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
