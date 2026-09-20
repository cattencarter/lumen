#!/usr/bin/env python3
"""Exercise every tool against a live session. Beta grid only.

This changes things on purpose: it takes clothing off and puts it back on, it
sends an instant message, and it teleports. All of it is reversible and all of
it is restored, but do not point it at a main-grid session.

It deliberately does NOT print inventory item names. The point of the endpoint
is that an assistant can read someone's inventory; that is not a reason for the
contents to end up in a terminal transcript. Counts, kinds and ids only.

    scripts/tools-check.py [--port 8787]
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

PASS = FAIL = 0
URL = ""

# Every request_id in this file is suffixed with this. They must not repeat
# across runs: a reused id is deliberately replayed rather than carried out, so
# a second run of a fixed-id test does nothing and then fails on its own
# success. That is the idempotency working; the test has to account for it.
RUN = str(int(time.time()))


def rid(label):
    return f"tools-check-{label}-{RUN}"


def check(label, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  pass  {label}")
    else:
        FAIL += 1
        print(f"  FAIL  {label}" + (f"  -- {detail}" if detail else ""))
    return ok


def call(method, params=None, _id=[0]):
    _id[0] += 1
    payload = {"jsonrpc": "2.0", "id": _id[0], "method": method}
    if params is not None:
        payload["params"] = params
    req = urllib.request.Request(URL, data=json.dumps(payload).encode(), method="POST")
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


def result(method, params=None):
    return call(method, params).get("result", {})


def err(method, params=None):
    return call(method, params).get("error", {})


def worn_state(item_id, want, timeout=25):
    """Poll until the item's worn flag reaches `want`. Appearance is async."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ids = {it["id"] for it in result("search_inventory", {"worn": True}).get("items", [])}
        if (item_id in ids) == want:
            return True
        time.sleep(1.5)
    return False


def main():
    global URL
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--give", action="store_true",
                    help="Also offer an inventory item to a real person. Off by default: an offer "
                         "puts a dialogue in front of them.")
    ap.add_argument("--send-notice", action="store_true",
                    help="Also send a real group notice. Off by default: a notice reaches every "
                         "member of the group and cannot be recalled.")
    args = ap.parse_args()
    URL = f"http://127.0.0.1:{args.port}/mcp"

    st = result("status")
    if not st.get("logged_in"):
        print(f"Not logged in ({st.get('startup_state')}).")
        return 2
    me = st.get("agent_id")
    home_region = st.get("region")
    print(f"Logged in, region {home_region}.\n")

    # ---------- search_inventory ----------
    print("search_inventory")
    inv = result("search_inventory", {"limit": 100})
    check("inventory is readable", "items" in inv, str(inv)[:160])
    # The search stops early once it has enough, so a capped result reports a
    # floor rather than inventing an exact total it never counted.
    total = inv.get("matched", inv.get("matched_at_least", 0))
    check("it reports how many it found", total > 0, str(inv)[:160])
    check("it honours the limit", len(inv.get("items", [])) <= 100, str(len(inv.get('items', []))))
    check("a capped search says so rather than claiming an exact total",
          ("matched_at_least" in inv) != ("matched" in inv), str(inv)[:200])
    check("it says when it truncated", inv.get("truncated") is True, str(inv)[:200])
    kinds = {}
    for it in inv.get("items", []):
        kinds[it["kind"]] = kinds.get(it["kind"], 0) + 1
    print(f"        {total} items matched; first 100 by kind: "
          + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))

    cl = result("search_inventory", {"kind": "clothing", "limit": 50})
    got = cl.get("items", [])
    check("the kind filter returns only that kind",
          got and all(i["kind"] == "clothing" for i in got), str(kinds))

    # Who made it. Without this an assistant asked "which of these did so-and-so
    # make" has only the name to go on, and may resort to reading the viewer's
    # own windows off the screen -- which is the thing this project removes.
    # Not *every* item: a few carry a null creator, and reporting a null id as
    # if it were an answer would be worse than leaving the field off.
    items = inv.get("items", [])
    with_creator = [i for i in items if "creator" in i]
    check("items say who created them",
          len(with_creator) > len(items) * 0.8,
          f"only {len(with_creator)} of {len(items)}")
    check("and a creator is a real id, never a null one",
          all(i["creator"] != "00000000-0000-0000-0000-000000000000" for i in with_creator))

    me_person = result("find_person", {"name": "catten"}).get("people", [])
    if me_person:
        cid = me_person[0]["agent_id"]
        byid = result("search_inventory", {"creator": cid, "limit": 20})
        check("search by creator id returns only that creator's items",
              byid.get("items") and all(i.get("creator") == cid for i in byid["items"]),
              str(byid.get("items", [])[:1])[:160])
        check("and their name comes back with them",
              any(i.get("creator_name") for i in byid.get("items", [])), str(byid)[:160])

    byname = result("search_inventory", {"creator": "catten", "kind": "clothing", "limit": 20})
    check("search by creator name works too", "items" in byname, str(byname)[:160])
    if "creators_not_yet_known" in byname:
        check("and it says how many creators it could not check",
              "creator_note" in byname, str(byname)[:200])
        # Two cautions used to share one `note` and the later one won.
        check("the creator caution does not clobber the truncation one",
              not ("note" in byname and byname.get("creator_note") == byname.get("note")),
              "both notes are the same string")

    nobody = result("search_inventory", {"creator": "zzzqqq-nobody-made-this", "limit": 10})
    check("an unknown creator returns nothing rather than everything",
          len(nobody.get("items", [])) == 0, str(nobody)[:160])

    # The worn list has to come from the outfit folder, not from scanning
    # inventory: with 65,000 items the worn ones are not in the first hundred.
    wornlist = result("search_inventory", {"worn": True})
    witems = wornlist.get("items", [])
    check("worn: true returns what is actually being worn", len(witems) > 0, str(wornlist)[:160])
    check("everything it returns is marked worn",
          all(i["worn"] for i in witems), str(len(witems)))
    wkinds = {}
    for i in witems:
        wkinds[i["kind"]] = wkinds.get(i["kind"], 0) + 1
    print(f"        {len(witems)} worn: " + ", ".join(f"{k} {v}" for k, v in sorted(wkinds.items())))

    check("an unloaded/absent match is an empty list, not an error",
          "items" in result("search_inventory", {"query": "zzzqqq-no-such-item"}))

    # ---------- folder paths and browsing ----------
    print("\nfolders")
    check("every item says which folder it is in",
          all("folder" in i for i in inv.get("items", [])), str(inv.get("items", [])[:1])[:160])

    top = result("list_folder")
    check("list_folder with no arguments lists the top of inventory",
          "folders" in top and len(top.get("folders", [])) > 0, str(top)[:200])
    print(f"        {len(top.get('folders', []))} top-level folders, "
          f"{top.get('item_count')} loose items")

    if top.get("folders"):
        sub = top["folders"][0]
        inside = result("list_folder", {"folder_id": sub["folder_id"]})
        check("list_folder descends by id", inside.get("folder_id") == sub["folder_id"], str(inside)[:160])
        check("it reports the folder's path", "path" in inside, str(inside)[:160])
        check("its items carry folder paths too",
              all("folder" in i for i in inside.get("items", [])))

    e = err("list_folder", {"name": "zzzqqq-no-such-folder"})
    check("an unknown folder name is a clean error", e.get("code") == -32000, str(e)[:160])

    # ---------- wear / detach round trip ----------
    print("\nwear / detach")
    # Clothing only. A body part cannot be taken off, and an attachment is a
    # slower round trip; clothing is the honest, reversible case.
    worn = [i for i in witems if i["kind"] == "clothing"]
    target = None
    if not worn:
        print("  skip  nothing worn that is clothing; cannot test the round trip")
    else:
        target = worn[0]
        tid = target["id"]
        print(f"        using a worn clothing item, id {tid[:8]}... "
              f"(name withheld, {len(target['name'])} chars)")

        d = result("detach", {"item_id": tid, "request_id": rid("detach")})
        check("detach does not claim success", "confirm_with" in d, str(d)[:160])
        check("detach actually took it off", worn_state(tid, False), "still worn after 25s")

        d2 = result("detach", {"item_id": tid, "request_id": rid("detach")})
        check("repeating the detach request_id replays", d2.get("replayed") is True, str(d2)[:160])

        d3 = result("detach", {"item_id": tid, "request_id": rid("detach-2")})
        check("detaching something already off is reported, not an error",
              d3.get("already_off") is True, str(d3)[:160])

        w = result("wear", {"item_id": tid, "request_id": rid("wear")})
        check("wear does not claim success", "confirm_with" in w, str(w)[:160])
        check("wear put it back on", worn_state(tid, True), "not worn again after 25s")

        w2 = result("wear", {"item_id": tid, "request_id": rid("wear-2")})
        check("wearing something already worn is reported, not an error",
              w2.get("already_worn") is True, str(w2)[:160])

    e = err("wear", {"name": "zzzqqq-no-such-item"})
    check("wearing something that does not exist is a clean error",
          e.get("code") == -32000, str(e)[:160])
    e = err("wear", {})
    check("wear with neither id nor name is a clean error", e.get("code") == -32602, str(e)[:160])

    # ---------- send_im ----------
    # To the user's own avatar: a real send down the real path, to the one
    # recipient who cannot be bothered by a test.
    print("\nsend_im")

    e = err("send_im", {"agent_id": me, "message": "x"})
    check("messaging yourself is refused rather than silently doing nothing",
          e.get("code") == -32000, str(e)[:200])

    people = result("find_person", {"name": "catten"}).get("people", [])
    check("find_person resolves a friend by name", len(people) == 1, str(people)[:200])
    if not people:
        print("  skip  no recipient; cannot test a real send")
        recipient = None
    else:
        recipient = people[0]["agent_id"]
        print(f"        sending to {people[0]['name']}")

    if recipient:
        text = f"Lumen tools-check {int(time.time()) % 100000} -- automated test, please ignore"
        im = result("send_im", {"name": "catten", "message": text,
                                "request_id": rid("im")})
        check("send_im accepts a name, not just a UUID", im.get("sent_to") == recipient, str(im)[:200])
        # The thing the earlier version of this test got wrong: our own copy of
        # the message appears locally whether or not it was delivered, so it
        # proves nothing about arrival. The tool must say so itself.
        check("send_im states plainly that delivery is NOT confirmed",
              im.get("delivery_confirmed") is False, str(im)[:200])

        im2 = result("send_im", {"name": "catten", "message": text,
                                 "request_id": rid("im")})
        check("the same request_id does not send twice", im2.get("replayed") is True, str(im2)[:200])

        # The one say deliberately does not do.
        im3 = result("send_im", {"name": "catten", "message": text,
                                 "request_id": rid("im-DIFFERENT")})
        check("an identical message with a NEW id is caught by the 60s window",
              im3.get("replayed") is True, str(im3)[:200])

        im4 = result("send_im", {"name": "catten", "message": text + " (second)",
                                 "request_id": rid("im-4")})
        check("different wording is NOT suppressed", im4.get("replayed") is None, str(im4)[:200])

        e = err("send_im", {"name": "catten", "message": ""})
        check("an empty message is refused", e.get("code") == -32602, str(e)[:160])

    e = err("send_im", {"name": "zzzqqq-nobody", "message": "x"})
    check("an unknown recipient is a clean error", e.get("code") == -32000, str(e)[:160])
    e = err("send_im", {"message": "x"})
    check("no recipient at all is a clean error", e.get("code") == -32602, str(e)[:160])

    # ---------- groups ----------
    # The guards only. Sending is left out by default: a group notice reaches
    # every member and cannot be recalled, and a test suite does not get to do
    # that on every run. --send-notice opts in.
    print("\ngroups")
    g = result("list_groups")
    check("list_groups answers", "groups" in g, str(g)[:160])
    check("it says whether notices may be sent in each",
          all("can_send_notices" in x for x in g.get("groups", [])), str(g)[:200])
    check("it distinguishes sending from receiving",
          all("accepts_notices" in x for x in g.get("groups", [])), str(g)[:200])
    can = [x for x in g.get("groups", []) if x["can_send_notices"]]
    cannot = [x for x in g.get("groups", []) if not x["can_send_notices"]]
    print(f"        {len(g.get('groups', []))} groups, notices allowed in {len(can)}")

    e = err("send_group_notice", {"group": "zzzqqq-no-such-group", "subject": "x", "message": "y"})
    check("an unknown group is a clean error", e.get("code") == -32000, str(e)[:160])
    if can:
        e = err("send_group_notice", {"group_id": can[0]["group_id"], "message": "y"})
        check("a notice with no subject is refused", e.get("code") == -32602, str(e)[:160])
        e = err("send_group_notice", {"group_id": can[0]["group_id"], "subject": "x",
                                      "message": "y", "item_id": "00000000-0000-0000-0000-000000000001"})
        check("an attachment that does not exist is refused", e.get("code") == -32602, str(e)[:160])
        e = err("send_group_notice", {"group_id": can[0]["group_id"], "subject": "x" * 200,
                                      "message": "y" * 5000})
        check("an over-long notice is refused rather than truncated by the server",
              e.get("code") == -32602, str(e)[:160])
    if cannot:
        e = err("send_group_notice", {"group_id": cannot[0]["group_id"],
                                      "subject": "x", "message": "y"})
        check("a group the user may not post to is REFUSED",
              e.get("code") == -32000, str(e)[:200])
    else:
        print("  skip  no group without send-notice power to refuse")

    if args.send_notice and can:
        target = can[0]
        n = result("send_group_notice", {"group_id": target["group_id"],
                                         "subject": f"Lumen check {RUN[-5:]}",
                                         "message": "Automated test notice, please ignore.",
                                         "request_id": rid("notice")})
        check("send_group_notice reports the group", n.get("group") == target["name"], str(n)[:200])
        check("it does not claim delivery", n.get("delivery_confirmed") is False, str(n)[:200])
        check("the same request_id does not send twice",
              result("send_group_notice", {"group_id": target["group_id"],
                                           "subject": f"Lumen check {RUN[-5:]}",
                                           "message": "Automated test notice, please ignore.",
                                           "request_id": rid("notice")}).get("replayed") is True)

    # ---------- giving something to a person ----------
    # Guards only by default. An offer puts a dialogue in front of a real
    # person, which a test suite does not get to do on every run; --give opts in.
    print("\ngive_item")
    e = err("give_item", {"agent_id": me, "item": "anything"})
    check("giving to yourself is refused", e.get("code") == -32000, str(e)[:160])
    e = err("give_item", {"name": "catten"})
    check("no item named is a clean error", e.get("code") == -32602, str(e)[:160])
    e = err("give_item", {"name": "zzzqqq-nobody", "item": "x"})
    check("an unknown recipient is a clean error", e.get("code") == -32000, str(e)[:160])

    # A real no-copy item of the user's. Found here rather than borrowed from
    # the delete section below, which runs later.
    nocopy = None
    for kind in ("object", "clothing", "bodypart", "animation", "texture", "landmark", "notecard"):
        for i in result("search_inventory", {"kind": kind, "limit": 100}).get("items", []):
            if not i.get("copyable") and not i.get("worn"):
                nocopy = i
                break
        if nocopy:
            break

    if nocopy:
        e = err("give_item", {"agent_id": recipient or me, "item_id": nocopy["id"]})
        check("a NO-COPY item needs the user's word first",
              e.get("code") == -32000, str(e)[:200])
        check("and it says so in the conversation, not in the viewer",
              "ask" in e.get("message", "").lower() and "confirm" in e.get("message", "").lower(),
              e.get("message", "")[:140])
        check("the refusal carries the item name so the user can be asked about it",
              (e.get("data") or {}).get("needs_confirmation") is True, str(e.get("data"))[:120])
        e = err("give_item", {"agent_id": recipient or me, "item_id": nocopy["id"],
                              "confirm": "not the right name"})
        check("a confirm that does not match the item is refused",
              e.get("code") == -32000, str(e)[:160])
        still = result("search_inventory", {"kind": nocopy["kind"], "limit": 100}).get("items", [])
        check("and the no-copy item did not move",
              any(i["id"] == nocopy["id"] for i in still), "it left inventory")

    wornc = [i for i in result("search_inventory", {"worn": True}).get("items", [])
             if i.get("copyable")]
    if wornc and recipient:
        e = err("give_item", {"agent_id": recipient, "item_id": wornc[0]["id"]})
        check("a worn item is refused until detached", e.get("code") == -32000, str(e)[:160])

    if args.give and recipient and made:
        g = result("give_item", {"agent_id": recipient, "item_id": made["id"],
                                 "request_id": rid("give")})
        check("give_item reports who it went to", g.get("offered_to") == recipient, str(g)[:200])
        check("it says offered, not delivered", g.get("delivery_confirmed") is False, str(g)[:200])
        check("the same request_id does not offer twice",
              result("give_item", {"agent_id": recipient, "item_id": made["id"],
                                   "request_id": rid("give")}).get("replayed") is True)
        check("the same item to the same person again is caught by the 60s window",
              result("give_item", {"agent_id": recipient, "item_id": made["id"],
                                   "request_id": rid("give-2")}).get("replayed") is True)

    # ---------- teleport ----------
    # Within the region already occupied: the full named-region lookup and
    # teleport path, with nowhere unexpected to end up.
    print("\nteleport")
    before = result("status").get("position", {})
    tp = result("teleport", {"region": home_region, "x": 40, "y": 40, "z": 30,
                             "request_id": rid("tp")})
    check("teleport does not claim arrival", "confirm_with" in tp, str(tp)[:200])
    check("teleport echoes the destination", tp.get("destination") == home_region, str(tp)[:200])

    moved = False
    for _ in range(20):
        time.sleep(2)
        now = result("status")
        pos = now.get("position", {})
        if now.get("region") == home_region and pos and pos != before:
            moved = True
            break
    check("the avatar actually moved", moved, f"position unchanged after 40s")

    e = err("teleport", {})
    check("teleport with no destination is a clean error", e.get("code") == -32602, str(e)[:160])

    # ---------- movement: walk, sit, stand ----------
    print("\nwalking and sitting")

    # Start from a known state.
    if result("status").get("sitting"):
        result("stand")
        time.sleep(3)

    st0 = result("status")
    check("status reports sitting, walking and flying",
          all(k in st0 for k in ("sitting", "walking", "flying")), str(sorted(st0))[:200])
    check("stand while already standing is reported, not an error",
          result("stand").get("already_standing") is True)

    nearby = result("look_nearby", {"radius": 30})
    check("look_nearby answers with people and objects",
          "people" in nearby and "objects" in nearby, str(nearby)[:160])
    print(f"        {len(nearby.get('people', []))} people, "
          f"{len(nearby.get('objects', []))} objects within 30m")

    # Sit on the ground: the one sit that cannot be refused by an object.
    sg = result("sit", {"ground": True, "request_id": rid("sit")})
    check("sit does not claim to have sat", "confirm_with" in sg, str(sg)[:160])
    sat = False
    for _ in range(10):
        time.sleep(1.5)
        if result("status").get("sitting"):
            sat = True
            break
    check("the avatar actually sat down", sat, "status never reported sitting")

    check("sitting again while seated is reported, not an error",
          result("sit", {"ground": True}).get("already_sitting") is True)
    e = err("walk_to", {"x": 128, "y": 128})
    check("walking while seated is refused with a reason", e.get("code") == -32000, str(e)[:160])

    result("stand", {"request_id": rid("stand")})
    stood = False
    for _ in range(10):
        time.sleep(1.5)
        if not result("status").get("sitting"):
            stood = True
            break
    check("the avatar stood back up", stood, "still sitting")

    # Walk a short distance from wherever it is now.
    here = result("status").get("position", [128, 128, 25])
    tx = max(5.0, min(250.0, here[0] + 12.0))
    ty = max(5.0, min(250.0, here[1] + 12.0))
    wk = result("walk_to", {"x": tx, "y": ty, "request_id": rid("walk")})
    check("walk_to does not claim arrival", "confirm_with" in wk, str(wk)[:200])
    check("walk_to reports the distance", "distance" in wk, str(wk)[:200])

    walking = False
    for _ in range(8):
        time.sleep(1)
        if result("status").get("walking"):
            walking = True
            break
    check("status shows the avatar walking", walking, "autopilot never reported")

    moved = False
    for _ in range(20):
        time.sleep(1.5)
        now = result("status").get("position", here)
        if abs(now[0] - here[0]) > 2 or abs(now[1] - here[1]) > 2:
            moved = True
            break
    check("the avatar actually walked somewhere", moved, "position unchanged")

    sw = result("stop_walking")
    check("stop_walking answers", sw.get("stopped") is True, str(sw)[:160])
    time.sleep(2)
    check("and the avatar is no longer walking",
          result("status").get("walking") is False, str(result("status"))[:200])
    check("stop_walking when not walking is harmless",
          result("stop_walking").get("stopped") is True)

    # Facing, and the relative movement it makes possible. Without a heading,
    # "move forward" cannot be answered at all -- which is how this was found.
    st = result("status")
    check("status reports which way the avatar faces",
          "facing" in st and "heading_degrees" in st, str(sorted(st))[:200])

    t = result("turn", {"direction": "east", "request_id": rid("turn-e")})
    check("turn faces a compass direction", t.get("facing") == "east", str(t)[:200])
    check("and reports the bearing", 80 <= t.get("heading_degrees", -1) <= 100, str(t)[:200])

    before = result("status")["position"]
    w = result("walk_to", {"direction": "forward", "distance": 10, "request_id": rid("walk-f")})
    check("walk_to accepts a direction and a distance", "walking_to" in w, str(w)[:200])
    moved_e = moved_n = 0
    for _ in range(20):
        time.sleep(1.5)
        now = result("status")["position"]
        moved_e, moved_n = now[0] - before[0], now[1] - before[1]
        if abs(moved_e) > 2 or abs(moved_n) > 2:
            break
    check("it walked, and 'forward' meant the way it was facing",
          abs(moved_e) > abs(moved_n) * 2, f"{moved_e:+.1f} east, {moved_n:+.1f} north")
    result("stop_walking")

    t = result("turn", {"direction": "north", "request_id": rid("turn-n")})
    check("turn north faces north", t.get("facing") == "north", str(t)[:200])
    e = err("turn", {"direction": "sideways"})
    check("a direction that is not one is refused", e.get("code") == -32602, str(e)[:160])
    e = err("turn", {})
    check("turn with nothing to turn to is refused", e.get("code") == -32602, str(e)[:160])

    # Flying. Restored afterwards, like everything else here.
    was_flying = result("status").get("flying")
    result("fly", {"enabled": True, "request_id": rid("fly-on")})
    flying = False
    for _ in range(10):
        time.sleep(1.5)
        if result("status").get("flying"):
            flying = True
            break
    check("fly takes off", flying, "status never reported flying")
    check("asking to fly while already flying is reported, not an error",
          result("fly", {"enabled": True}).get("already") is True)
    result("fly", {"enabled": False, "request_id": rid("fly-off")})
    for _ in range(10):
        time.sleep(1.5)
        if not result("status").get("flying"):
            break
    check("and lands again", result("status").get("flying") is False)
    if was_flying:
        result("fly", {"enabled": True})

    e = err("walk_to", {"x": 10, "y": 10, "z": 20000})
    check("an absurd distance is refused rather than attempted",
          e.get("code") == -32000, str(e)[:160])
    e = err("walk_to", {})
    check("walk_to with no destination is a clean error", e.get("code") == -32602, str(e)[:160])
    e = err("sit", {})
    check("sit with neither object nor ground is a clean error",
          e.get("code") == -32602, str(e)[:160])
    e = err("sit", {"object_id": "00000000-0000-0000-0000-000000000001"})
    check("sitting on an object that is not there is a clean error",
          e.get("code") == -32000, str(e)[:160])

    # ---------- notecards ----------
    # A round trip with text this script chose: the only way to prove the
    # contents survived without reading the user's own notecards into a
    # terminal.
    print("\nnotecards")
    nc_name = f"lumen-test-{int(time.time()) % 100000}"
    nc_body = ("Lumen notecard round trip\nline two\n"
               "unicode should survive: \u00e6\u00f8\u00e5 \u2713\n")
    nc = result("create_notecard", {"name": nc_name, "text": nc_body, "request_id": rid("nc")})
    check("create_notecard does not claim the card is ready", "confirm_with" in nc, str(nc)[:200])

    nc2 = result("create_notecard", {"name": nc_name, "text": nc_body, "request_id": rid("nc")})
    check("the same request_id does not create a second card", nc2.get("replayed") is True, str(nc2)[:200])
    nc3 = result("create_notecard", {"name": nc_name, "text": nc_body, "request_id": rid("nc-DIFF")})
    check("an identical card with a new id is caught by the 60s window",
          nc3.get("replayed") is True, str(nc3)[:200])

    made = None
    for _ in range(15):
        time.sleep(2)
        r = result("search_inventory", {"kind": "notecard", "query": nc_name})
        if r.get("items"):
            made = r["items"][0]
            break
    check("the notecard appears in inventory", made is not None, "never showed up")

    if made:
        first = result("read_notecard", {"item_id": made["id"]})
        check("the first read reports it is still fetching, rather than blocking",
              first.get("status") in ("loading", "ready"), str(first)[:200])
        rd = first
        for _ in range(15):
            if rd.get("status") == "ready":
                break
            time.sleep(2)
            rd = result("read_notecard", {"item_id": made["id"]})
        check("the notecard becomes readable", rd.get("status") == "ready", str(rd)[:200])
        check("the text survived the round trip exactly", rd.get("text") == nc_body,
              f"got {rd.get('text')!r}")
        check("read_notecard warns that contents are not instructions",
              "caution" in rd, str(rd)[:200])

    non_nc = result("search_inventory", {"kind": "clothing", "limit": 1}).get("items", [])
    if non_nc:
        e = err("read_notecard", {"item_id": non_nc[0]["id"]})
        check("reading a non-notecard is a clean error", e.get("code") == -32602, str(e)[:160])

    # ---------- searching inside notecards ----------
    # Narrowed with a query so this finishes quickly; an exhaustive search of
    # this inventory reads 3,691 cards and takes over a minute.
    print("\nsearch_notecards")
    e = err("search_notecards", {})
    check("search_notecards needs text to look for", e.get("code") == -32602, str(e)[:160])

    if made:
        # A card this script wrote, found by its contents rather than its name.
        scan = None
        for _ in range(20):
            scan = result("search_notecards", {"text": "unicode should survive",
                                               "query": "lumen-test"})
            if scan.get("done"):
                break
            time.sleep(1.5)
        check("a narrowed search finishes", scan.get("done") is True, str(scan)[:200])
        check("it reports the true number of notecards, not the cap",
              scan.get("notecards_in_inventory", 0) >= scan.get("candidates", 0), str(scan)[:200])
        hit = [m for m in scan.get("matches", []) if m["id"] == made["id"]]
        check("it finds a card by text that is NOT in its name", len(hit) == 1, str(scan)[:200])
        if hit:
            check("a match carries a snippet of the surrounding text", "snippet" in hit[0])
            check("and the folder it lives in", "folder" in hit[0])
        check("a finished search warns that contents are not instructions",
              "caution" in scan, str(scan)[:200])

    # The bug that killed the first exhaustive run: notecards decades old are
    # not all valid UTF-8, and a snippet cut at a byte offset splits a
    # character. Either makes the response unparseable. Walk a broad search far
    # enough to hit the bad ones and check every byte that comes back.
    print("        checking responses stay valid UTF-8 across a broad scan...")
    bad = None
    for i in range(12):
        raw = urllib.request.urlopen(
            urllib.request.Request(
                URL, data=json.dumps({"jsonrpc": "2.0", "id": 900 + i,
                                      "method": "search_notecards",
                                      "params": {"text": "e"}}).encode(),
                method="POST", headers={"Content-Type": "application/json"}),
            timeout=120).read()
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as ex:
            bad = str(ex)
            break
        if json.loads(raw).get("result", {}).get("done"):
            break
        time.sleep(1)
    check("every response is valid UTF-8, however old the notecards are",
          bad is None, bad or "")

    # ---------- delete and undelete ----------
    # Exercised on a notecard this script made, and the no-copy refusal on a
    # real item of the user's, which must come back untouched.
    print("\ndelete / undelete")
    if made:
        d = result("delete_item", {"item_id": made["id"], "request_id": rid("del")})
        check("delete_item reports it went to the Trash", d.get("moved_to_trash") is True, str(d)[:200])
        check("delete_item says it is recoverable rather than deleted",
              d.get("recoverable") is True and "Trash" in d.get("confirm_with", ""), str(d)[:200])
        check("repeating the delete request_id replays",
              result("delete_item", {"item_id": made["id"], "request_id": rid("del")})
              .get("replayed") is True)
        check("deleting something already in the Trash is reported, not an error",
              result("delete_item", {"item_id": made["id"]}).get("already_in_trash") is True)

        u = result("undelete_item", {"item_id": made["id"], "request_id": rid("undel")})
        check("undelete_item restores it", u.get("restored") is True, str(u)[:200])
        time.sleep(3)
        check("and it is out of the Trash again",
              any(i["id"] == made["id"]
                  for i in result("search_inventory",
                                  {"kind": "notecard", "query": nc_name}).get("items", [])))
        check("undeleting something not deleted is reported, not an error",
              result("undelete_item", {"item_id": made["id"]}).get("not_deleted") is True)

    check("search_inventory reports copyability",
          all("copyable" in i for i in result("search_inventory", {"limit": 10}).get("items", [])))

    # The guard the whole tool rests on. Same item the give_item section found.
    if nocopy:
        e = err("delete_item", {"item_id": nocopy["id"]})
        check("deleting a no-copy item needs the user's word first",
              e.get("code") == -32000, str(e)[:200])
        check("the refusal is machine-readable, not only prose",
              (e.get("data") or {}).get("reason") == "no-copy", str(e.get("data"))[:120])
        e = err("delete_item", {"item_id": nocopy["id"], "confirm": "wrong"})
        check("a confirm that does not match is refused", e.get("code") == -32000, str(e)[:160])
        still = result("search_inventory", {"kind": nocopy["kind"], "limit": 100}).get("items", [])
        check("and the no-copy item is untouched",
              any(i["id"] == nocopy["id"] for i in still), "it moved")

    # The bug the confirmation work uncovered: error data was dropped on every
    # path, so "ask which one" never came with anything to ask about.
    amb = result("search_inventory", {"query": "a", "limit": 3}).get("items", [])
    if len(amb) > 1:
        e = err("wear", {"name": "a"})
        check("an ambiguous name returns the candidates, not just advice to ask",
              isinstance(e.get("data"), list) and len(e["data"]) > 1, str(e.get("data"))[:140])
    else:
        print("  skip  no no-copy item in the sample to refuse")

    if worn:
        e = err("delete_item", {"item_id": target["id"]})
        check("a worn item is refused until it is detached", e.get("code") == -32000, str(e)[:160])

    # ---------- the six that answer "what is happening to me" ----------
    print("\nwhat is happening")
    d = result("read_dialogues")
    check("read_dialogues answers", "dialogues" in d and "count" in d, str(d)[:160])
    print(f"        {d.get('count')} waiting for an answer")
    for one in d.get("dialogues", []):
        check("each dialogue carries its id and its choices",
              "id" in one and "choices" in one, str(one)[:160])
    e = err("answer_dialogue", {})
    check("answering with nothing is a clean error", e.get("code") == -32602, str(e)[:160])
    e = err("answer_dialogue", {"id": "00000000-0000-0000-0000-000000000001", "choice": "OK"})
    check("answering a dialogue that is not there is a clean error",
          e.get("code") == -32000, str(e)[:160])

    f = result("list_friends")
    check("list_friends separates online from offline",
          "online" in f and "offline" in f and "total" in f, str(f)[:160])
    print(f"        {f.get('total')} friends, {len(f.get('online', []))} online")

    w = result("where_am_i")
    check("where_am_i says what the parcel allows",
          "parcel_allows" in w and "flying" in w.get("parcel_allows", {}), str(w)[:200])
    check("and it explains why that matters", "note" in w, str(w)[:160])

    outfits = result("list_folder", {"name": "My Outfits"})
    check("outfits are reachable as folders", len(outfits.get("folders", [])) > 0, str(outfits)[:160])
    e = err("wear_outfit", {})
    check("wear_outfit needs a name, and says where to find one",
          e.get("code") == -32602 and "My Outfits" in e.get("message", ""), str(e)[:200])
    e = err("wear_outfit", {"name": "zzzqqq-no-such-outfit"})
    check("an outfit that does not exist is a clean error", e.get("code") == -32000, str(e)[:160])

    e = err("teleport", {"landmark": "zzzqqq-no-such-landmark"})
    check("teleporting to a landmark that does not exist is a clean error",
          e.get("code") == -32000, str(e)[:160])
    nc = result("search_inventory", {"kind": "notecard", "limit": 1}).get("items", [])
    if nc:
        e = err("teleport", {"landmark": nc[0]["name"]})
        check("and a non-landmark is refused as one", e.get("code") in (-32000, -32602), str(e)[:160])

    e = err("send_group_message", {"group": "zzzqqq-no-such-group", "message": "x"})
    check("group chat to an unknown group is a clean error", e.get("code") == -32000, str(e)[:160])
    e = err("send_group_message", {"group": "testlumengroup", "message": ""})
    check("an empty group message is refused", e.get("code") == -32602, str(e)[:160])

    # ---------- read_actions ----------
    print("\nread_actions")
    log = result("read_actions", {"limit": 50})
    entries = log.get("entries", [])
    tools_seen = {e_["tool"] for e_ in entries}
    expected = ({"teleport", "create_notecard", "sit", "stand", "walk_to", "delete_item",
                 "turn", "fly"}
                | ({"send_im"} if recipient else set()))
    check("every write reached the log", expected <= tools_seen, str(sorted(tools_seen)))
    # The log is session-wide and outlives a single run of this script, so
    # count only what this run put there. Every request_id carries RUN.
    mine = [e_ for e_ in entries
            if e_["tool"] == "send_im" and e_.get("request_id", "").endswith(RUN)]
    check("replays were not logged as extra actions",
          len(mine) == (2 if recipient else 0),
          f"{len(mine)} send_im entries from this run for 2 real sends")
    logblob = json.dumps(log)
    if recipient:
        check("the log does NOT contain the message text", text not in logblob,
              "message text leaked into read_actions")
        check("the log DOES record who was messaged", recipient in logblob, str(log)[:200])

    # ---------- put everything back ----------
    # A failed run must not leave the avatar sitting on the ground or missing
    # a garment. Restoring is not a test; it runs regardless.
    print("\nrestoring")
    result("stop_walking")
    if result("status").get("sitting"):
        result("stand")
        for _ in range(10):
            time.sleep(1.5)
            if not result("status").get("sitting"):
                break
    check("the avatar is standing again", result("status").get("sitting") is False)
    if target:
        back = {i["id"] for i in result("search_inventory", {"worn": True}).get("items", [])}
        if target["id"] not in back:
            result("wear", {"item_id": target["id"]})
            for _ in range(15):
                time.sleep(2)
                back = {i["id"] for i in result("search_inventory", {"worn": True}).get("items", [])}
                if target["id"] in back:
                    break
        check("the detached item is back on", target["id"] in back, "left off the avatar")

    # ---------- and it is still alive ----------
    print()
    check("viewer still answering after all of that",
          result("status").get("logged_in") is True)

    print(f"\n  {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
