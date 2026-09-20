#!/usr/bin/env python3
"""Prove what `say` and the action log actually do, against a live session.

Run this on the beta grid, or somewhere nobody is standing. It speaks out loud
several times on purpose: that is the thing being tested.

What it establishes, in order:

  1. say speaks, and the words come back through read_chat. The endpoint does
     not claim success; the echo is the proof.
  2. The same request_id twice says it once. This is the case that matters --
     a caller that timed out and is retrying.
  3. A different request_id with identical words says it twice, because `say`
     opts out of the fingerprint window on purpose. People repeat themselves.
  4. read_actions shows that it happened, and does not show what was said.

Standard library only.

    scripts/write-check.py [--port 8787]
"""

import argparse
import json
import sys
import time
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
        print(f"  FAIL  {label}" + (f"  -- {detail}" if detail else ""))


def call(url, method, params=None, _id=[0]):
    _id[0] += 1
    payload = {"jsonrpc": "2.0", "id": _id[0], "method": method}
    if params is not None:
        payload["params"] = params
    req = urllib.request.Request(url, data=json.dumps(payload).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            raw = r.read()
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except Exception:
            return {}


def lines_since(url, seq):
    """The text of every chat line after `seq`, once the echo has had time."""
    # The viewer's own speech goes to the server and comes back, so it is not
    # in the stream the instant say returns. Poll rather than guess a delay.
    deadline = time.time() + 8
    texts = []
    while time.time() < deadline:
        body = call(url, "read_chat", {"since": seq, "limit": 50})
        entries = body.get("result", {}).get("entries", [])
        texts = [e.get("message", e.get("text", "")) for e in entries]
        if texts:
            break
        time.sleep(0.5)
    return texts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    args = ap.parse_args()
    url = f"http://127.0.0.1:{args.port}/mcp"

    st = call(url, "status").get("result", {})
    if not st.get("logged_in"):
        print(f"Not logged in ({st.get('startup_state')}). Log in first; this test speaks "
              f"out loud and needs a session.")
        return 2
    print(f"Logged in, region {st.get('region')}. This will speak out loud.\n")

    marker = f"lumen write-check {int(time.time()) % 100000}"

    # --- 1. it speaks, and the echo proves it ----------------------------
    rid = "write-check-A"
    r = call(url, "say", {"message": marker, "request_id": rid}).get("result", {})
    check("say returns chat_seq_before rather than claiming success",
          "chat_seq_before" in r, str(r)[:160])
    seq0 = r.get("chat_seq_before", 0)

    said = lines_since(url, seq0)
    check("the words come back through read_chat",
          any(marker in t for t in said), str(said)[:200])
    spoken_once = sum(1 for t in said if marker in t)
    check("said exactly once", spoken_once == 1, f"counted {spoken_once}")

    # --- 2. the same id again must not speak again -----------------------
    seq_before_retry = call(url, "read_chat", {"since": 0, "limit": 1}) \
        .get("result", {}).get("latest_seq", 0)
    r2 = call(url, "say", {"message": marker, "request_id": rid}).get("result", {})
    check("a repeated request_id is reported as a replay", r2.get("replayed") is True, str(r2)[:200])
    check("the replay returns the original answer",
          r2.get("chat_seq_before") == seq0, str(r2)[:200])

    time.sleep(3)
    after = call(url, "read_chat", {"since": seq_before_retry, "limit": 50}) \
        .get("result", {}).get("entries", [])
    again = sum(1 for e in after if marker in e.get("message", e.get("text", "")))
    check("nothing was said a second time", again == 0, f"{again} new copies appeared")

    # --- 3. a new id with the same words must speak again ----------------
    # say passes a window of 0 on purpose: repeating yourself in local chat is
    # normal, and refusing to is a worse failure than the duplicate.
    seq_before_repeat = call(url, "read_chat", {"since": 0, "limit": 1}) \
        .get("result", {}).get("latest_seq", 0)
    r3 = call(url, "say", {"message": marker, "request_id": "write-check-B"}).get("result", {})
    check("a new request_id is not treated as a replay", r3.get("replayed") is None, str(r3)[:160])

    repeated = lines_since(url, seq_before_repeat)
    check("a deliberate repeat does get said",
          any(marker in t for t in repeated), str(repeated)[:200])

    # --- 4. the log shows it happened, not what was said -----------------
    log = call(url, "read_actions", {"limit": 20}).get("result", {})
    entries = log.get("entries", [])
    says = [e for e in entries if e.get("tool") == "say"]
    check("read_actions recorded the writes", len(says) >= 2, str(log)[:200])
    check("each entry says when and how it went",
          all(e.get("at") and e.get("outcome") for e in says), str(says)[:200])
    check("the replay was not logged as a second action",
          len(says) == 2, f"{len(says)} say entries for 2 real writes")

    blob = json.dumps(log)
    check("the log does NOT contain the words that were said",
          marker not in blob, "the message text leaked into read_actions")
    check("the log does report how much was said",
          any("characters" in json.dumps(e.get("detail", {})) for e in says), str(says)[:200])

    print(f"\n  {PASS} passed, {FAIL} failed")
    if says:
        print(f"\n  what the user would see in read_actions:")
        for e in says:
            print(f"    {e.get('at')}  {e.get('tool')}  {e.get('outcome')}  {e.get('detail')}")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
