# Driving Lumen from a program (LEAP)

Lumen can be driven by a program running beside it on your computer: one you
write, or one an AI assistant runs for you. It opens windows, changes any
setting, moves the camera and your avatar, reads your inventory, answers
dialogs and saves pictures. This is done through **LEAP**, the LLSD Event API
Plugin protocol that Second Life viewers carry. This page covers the whole
viewer over LEAP, with Lumen's own windows and settings where they belong.

Nothing in Lumen listens for it. A LEAP program is started *by the viewer*,
as its child, and the two talk over the program's own input and output.
Started without one, Lumen runs no program for it.

Lumen's own Assistant does not use LEAP: it calls the viewer directly, which
is how it can do what LEAP cannot, such as finding a person by name. See
*Lumen's Assistant* below.

> **Only run programs you trust.** A program started this way can do
> anything you can do in the viewer: change settings, move your avatar,
> send chat, spend L$ -- and answer the questions the viewer asks you before
> the Assistant acts.

## Starting Lumen with a program

Quit Lumen, then start it from Terminal with `--leap` and the command line
that runs your program. On a Mac:

```sh
open -n -a Lumen --args --leap "'/path/to/venv/bin/python3 /path/to/myscript.py'"
```

On Windows, from PowerShell:

```powershell
& "C:\Program Files\Lumen\Lumen.exe" --leap "'C:\path\to\python.exe C:\path\to\myscript.py'"
```

The command line is read as an LLSD string, so it has to be quoted twice:
in single quotes inside the shell's double quotes, as above. A path with a
space or a quote in it will not work; keep the program and its Python in a
plain path. The program runs until the viewer quits; the viewer then closes
its input. Anything the program writes to its standard error goes into the
viewer's log (`~/Library/Application Support/Lumen/logs/Lumen.log` on a Mac,
`%APPDATA%\Lumen\logs\Lumen.log` on Windows).

`--leap` can be given more than once, to start several programs.

## The protocol

The viewer and the program exchange LLSD messages, each written as its
length in bytes, a colon, and the message:

```
<length>:<message>
```

- The viewer writes to the program's **standard input**, in binary LLSD.
- The program writes to its **standard output**, in **notation** LLSD. Its
  standard output belongs to the protocol: print anything else to standard
  error.
- Every message is a map: `{"pump": <name>, "data": <map>}`. To ask an API
  for something, send to its pump a map with `"op"` naming the operation,
  and its arguments.
- The viewer's **first** message is a handshake. Its `"pump"` is the pump on
  which your program receives replies, and its `data["command"]` the LEAP
  command pump (below).
- To get an answer, add `"reply": <the reply pump>` and a `"reqid"` of your
  own to the request; the answer comes on the reply pump with the same
  `"reqid"`. Requests without a reply are fire-and-forget.

Linden Lab's `llsd` Python package reads and writes LLSD
(`pip install llsd`, in a virtual environment).

### A complete example

```python
#!/usr/bin/env python3
"""Started by Lumen with --leap: reads the handshake, asks for a setting,
waits until you are logged in, and saves a snapshot of the view."""
import itertools
import os
import sys
import time

import llsd  # pip install llsd

stdin, stdout = sys.stdin.buffer, sys.stdout.buffer


def read():
    length = b""
    while (c := stdin.read(1)) != b":":
        if not c:
            raise EOFError  # the viewer quit
        length += c
    remaining, data = int(length), b""
    while remaining:
        chunk = stdin.read(remaining)
        data += chunk
        remaining -= len(chunk)
    return llsd.parse(data)


def send(pump, data):
    body = llsd.format_notation({"pump": pump, "data": data})
    stdout.write(b"%d:" % len(body) + body)
    stdout.flush()


handshake = read()
reply_pump = handshake["pump"]
reqids = itertools.count(1)


def request(pump, data):
    """Send and wait for the answer to this request."""
    reqid = next(reqids)
    send(pump, dict(data, reply=reply_pump, reqid=reqid))
    while True:
        message = read()
        answer = message.get("data")
        if isinstance(answer, dict) and answer.get("reqid") == reqid:
            return answer


distance = request("LLViewerControl", {"op": "get", "group": "Global", "key": "RenderFarClip"})
print("draw distance is", distance.get("value"), file=sys.stderr)

# Wait for the login: LLAgent getID answers a null key until then.
while str(request("LLAgent", {"op": "getID"}).get("id", "")) in ("", "00000000-0000-0000-0000-000000000000"):
    time.sleep(2)

picture = os.path.expanduser("~/Desktop/lumen-leap.png")
saved = request("LLViewerWindow", {"op": "saveSnapshot", "filename": picture,
                                   "width": 1280, "height": 720})
print("snapshot:", saved, file=sys.stderr)
```

A program that should stay connected keeps reading from standard input;
when the viewer quits, `read()` raises `EOFError`.

### Asking the viewer what it offers

Send `{"op": "getAPIs"}` to the command pump from the handshake, with a
`reply` and `reqid`: the answer lists every API with its description.
`{"op": "getAPI", "api": "<name>"}` describes one API and each of its
operations and their arguments. That is always right for the build you
run; the list at the end of this page is Lumen 0.1.8's.

### Three things to know first

- **A UI call of a function the viewer does not have stops the viewer**
  (Linden Lab's own behaviour). Check a name against this page, or the
  viewer's menus and buttons, before calling it.
- **Clicking**: `LLWindow` `mouseDown` / `mouseUp` with only a `path` aim at
  the element's centre and may not press it. Give `x` and `y` as well: the
  centre of `getInfo`'s `rect`, times two on a Retina display (`getInfo`
  answers in interface points, the mouse ops take window pixels).
- **Everything in the world is real.** Chat is heard, teleports and
  movement are seen, and Linden Lab's rules apply to what a program does
  (see *Good manners*).

## Windows and buttons

| Pump | Op | Does |
|---|---|---|
| `LLFloaterReg` | `showInstance` (`name`, optionally `key`), `hideInstance`, `toggleInstance`, `toggleInstanceOrBringToFront` | Opens or closes a window by its name. |
| `LLFloaterReg` | `instanceVisible` (`name`, `reply`) | Whether it is open. |
| `LLFloaterReg` | `clickButton` (`name`, `button`) | Presses a named button in an open window. |
| `LLFloaterReg` | `getBuildMap` (`reply`) | Every window's name. |
| `LLWindow` | `getPaths` (optionally `under`), `getInfo` (`path`) | The interface's elements by path, and one element's class, value, rect and whether it is visible and enabled. |
| `LLWindow` | `mouseDown` / `mouseUp` / `mouseMove` (`x`, `y`, `button`, optionally `path`, `mask`), `mouseScroll` (`clicks`) | The mouse. |
| `LLWindow` | `keyDown` / `keyUp` (`keycode`, `keysym` or `char`, optionally `path`, `mask`), `pasteText` (`text`, optionally `path`) | The keyboard. Escape is keycode 134, Return 129. |
| `UI` | `call` (`function`, `parameter`) | Runs a command as its menu item or button does. |
| `UI` | `getValue` (`path`), `setSelectedByValue` (`path`, `value`) | Reads a control, or chooses in a menu. |

**Lumen's own windows**: `ai_chat` the Assistant, `lumen_skills` Skills,
`ai_memory` what the Assistant remembers, `ai_setup` the window that sets a
subscription up. On pump `UI`, op `call`, `Lumen.Inventory.Undo` and
`Lumen.Inventory.Redo` undo and redo this login's changes to your inventory,
as the inventory's gear menu does -- after asking you, as the menu does.

## Settings

Every setting the viewer has can be read and changed (pump
`LLViewerControl`, group `Global` for the viewer's settings, `PerAccount`
for the logged-in account's):

| Op | Arguments | Does |
|---|---|---|
| `get` | `group`, `key`, `reply` | The setting's `value`, `type` and `comment`. |
| `set` | `group`, `key`, `value` | Sets it; without `value`, back to its default. |
| `toggle` | `group`, `key` | Flips a true / false one. |
| `vars` | `group`, `reply` | Every setting in the group with its value and comment. |
| `groups` | `reply` | The groups' names. |

**Advanced > Show Debug Settings** lists them too. Everything Lumen adds
starts with `Lumen`, and its comment says what it does: `LumenAI...` for
the Assistant (`LumenAIProvider` the AI it uses), `LumenUpdate...` for the
update message.

## The view and the camera

| | Does |
|---|---|
| `LLAgent` `setCameraParams` (`camera_pos`, `focus_pos` in region metres, `camera_locked`, `focus_locked`, ...), then `setFollowCamActive` (`active` true) | Puts the camera there. |
| `LLAgent` `removeCameraParams`, `setFollowCamActive` (`active` false) | Gives the camera back. |
| `LLViewerControl` `CameraAngle` | The view angle, in radians. |
| `LLPipeline` `toggleRenderTypes` (`types`, e.g. `["water"]`), `hasRenderType` | Turns kinds of things off and on in the view, as the Advanced menu's Rendering Types do. |
| `LLViewerWindow` `saveSnapshot` (`filename`, `width`, `height`, `showui`, `showhud`, `rebuild`, `type` `"COLOR"` or `"DEPTH"`, `reply`) | Saves a snapshot of the view to a file. |

## Your avatar

| Pump | Op | Does |
|---|---|---|
| `LLAgent` | `getID`, `getPosition` (`reply`) | Your key; where you are, in region and global metres, and which way you face. |
| `LLAgent` | `requestSit` (`obj_uuid` or `position`), `requestStand` | Sits on a thing, or stands. |
| `LLAgent` | `requestTouch` (`obj_uuid` or `position`, optionally `face`) | Touches a thing, as a click on it does. |
| `LLAgent` | `lookAt` (`type`, `obj_uuid` or `position`), `resetAxes` | Where your avatar looks; facing straight again. |
| `LLAgent` | `playAnimation` (`item_id`, `inworld`), `stopAnimation`, `getAnimationInfo` | Plays an animation from your inventory, for you alone or for everyone. |
| `LLAgent` | `getNearbyAvatarsList`, `getNearbyObjectsList` (`dist`), `getAttachedObjectsList`, `getAgentScreenPos` | Who and what is near, what you wear, where an avatar is on screen. |
| `LLAgent` | `getGroups` (`reply`) | Your groups. |
| `LLAppearance` | `wearOutfit` (`folder_id` or `folder_name`, `append`), `wearItems`, `detachItems` (`items_id`), `getOutfitsList`, `getOutfitItems` | Outfits and what you wear. |
| `LLGesture` | `getActiveGestures`, `startGesture`, `stopGesture`, `isGesturePlaying` (`id`) | Gestures; there once you are logged in. |

## Moving and teleporting

| Pump | Op | Does |
|---|---|---|
| `LLAgent` | `requestTeleport` (`regionname`, `x`, `y`, `z`, optionally `skip_confirmation`) | Teleports, as a map or a location link does. |
| `LLTeleportHandler` | `teleport` (`regionname`, `x`, `y`, `z`; without `regionname`, global coordinates) | The same, lower down. |
| `LLAgent` | `startAutoPilot` (`target_global`, ...), `setAutoPilotTarget`, `stopAutoPilot`, `getAutoPilot` | Walks or flies you to a place. |
| `LLAgent` | `startFollowPilot` (`leader_id` or `avatar_name`) | Follows an avatar. |

> **One teleport at a time.** Wait for one to finish -- or fail -- before
> the next, keep a pause between them, and do not send many in a row: a
> teleport sent while another has not answered can be refused, and the
> grid may disconnect the viewer. Moving your avatar about while you are
> away makes it an automated avatar under Linden Lab's rules.

## Chat

| Pump | Op | Does |
|---|---|---|
| `GroupChat` | `startGroupChat`, `leaveGroupChat`, `sendGroupIM` (`group_id`, `message`) | Group chat. |

Nearby chat and instant messages have no LEAP API.

> **Others read it as you.** Do not let a program chat, answer or post on
> its own without your knowing; never spam a channel or a group.

## Inventory

`LLInventory` reads your inventory: `getBasicFolderID` (a folder type's
name, e.g. `"Textures"`), `getDirectDescendants` (`folder_id`),
`collectDescendantsIf` (`folder_id`, and `name`, `desc`, `type` to filter
by), `getItemsInfo` (`item_ids`), `getFolderTypeNames`,
`getAssetTypeNames`. Lumen keeps its own things in `#Lumen`, and the skills
you teach the Assistant in `#Lumen/#Skills`.

## Dialogs and notices

`LLNotifications`: `listChannels`, `listChannelNotifications` (`channel`),
`respond` (`uuid`, `response`) to answer a dialog as its buttons do,
`cancel` (`uuid`), `requestAdd` (`name`, `substitutions`, `payload`) to show
one, `ignore`, and `forward` to have a channel's notices sent to your
program.

> **The viewer's questions are yours.** Before the Assistant says
> something, gives something away, builds or deletes, the viewer asks you
> (notices named `LumenAsk...`). A program can answer them with `respond`.
> Let it only if you would have pressed the same button.

## The viewer itself

| Pump | Op | Does |
|---|---|---|
| `LLStats` | `getPerfData` (`reply`) | Frame times and other statistics. |
| `LLFloaterAbout` | `getInfo` (`reply`) | What the About window says: version, graphics, region. |
| `LLStartUp` | `getStateTable`, `postStartupState` | Where the login has got to. |
| `LLToolMgr` | `selectTool` (`tool_name`: Focus, Move, Edit, Create, Land), `openBuildFloater` | The build tools. |
| `LLURLDispatcher` | `dispatch` (`url`, `trusted`) | Opens a Second Life link (a `secondlife:///app/...` address) as a click on it would. |
| `LLCommandDispatcher` | `dispatch` (`cmd`, `params`, `query`), `enumerate` | The viewer's app commands, by name. |
| `LLViewerWindow` | `requestReshape` (`w`, `h`) | The window's layout size. |
| `LLAppViewer` | `requestQuit` | Quits as Quit does, logging out. |

**Logging in**: the login screen logs in the account it has selected when
Return is pressed in it (`LLWindow` `keyDown` keycode 129).

## Good manners

- **Logging in** uses the account and password saved on the login screen.
  Never put a password in a program, and check which account and grid the
  login screen has selected before pressing Return.
- **Linden Lab's rules apply** to anything a program does in Second Life,
  among them the rules for automated avatars if it acts while you are away.
- **Be gentle with the grid**: one teleport at a time, as above; do not log
  in and out repeatedly.
- **Quit, do not kill**, a logged-in viewer (`LLAppViewer` `requestQuit`):
  a killed viewer leaves the avatar standing in the region for a while.
  `LLAppViewer` `forceQuit` is that kill.

## Lumen's Assistant

Lumen has an assistant built in (**Comm > Assistant**): you ask it in your
own words, and it does it with Lumen's own tools, which call the viewer
directly. Those tools reach further than LEAP. LEAP cannot turn a name into
a person -- the viewer offers no name service over it -- so nothing
addressed to someone by name is in reach of a LEAP program, while the
Assistant finds them through the viewer's own name cache. The same goes
for instant messages, nearby chat, giving things, building and searching
your inventory by what things are.

The Assistant's tools are not offered over LEAP. If you want an AI to drive
Lumen, the Assistant is the way in; LEAP is for programs of your own.

## Every API in Lumen 0.1.8

Ask `getAPIs` and `getAPI` for the build you run; this is 0.1.8's, checked
against its `getAPIs`.

| Pump | Operations |
|---|---|
| (the command pump) | The handshake's `command` pump: `getAPIs`, `getAPI` (`api`), `getFeatures`, `getFeature`, `listen`, `stoplistening`, `newpump`, `ping`. |
| `GroupChat` | Group chat: `startGroupChat`, `leaveGroupChat`, `sendGroupIM`. |
| `LLAgent` | Your avatar and the camera: `getID`, `getPosition`, `getGroups`, `requestTeleport`, `requestSit`, `requestStand`, `requestTouch`, `resetAxes`, `lookAt`, `startAutoPilot`, `getAutoPilot`, `startFollowPilot`, `setAutoPilotTarget`, `stopAutoPilot`, `setCameraParams`, `setFollowCamActive`, `removeCameraParams`, `playAnimation`, `stopAnimation`, `getAnimationInfo`, `getNearbyAvatarsList`, `getNearbyObjectsList`, `getAttachedObjectsList`, `getAgentScreenPos`. |
| `LLAppearance` | Outfits: `wearOutfit`, `wearItems`, `detachItems`, `getOutfitsList`, `getOutfitItems`. |
| `LLAppViewer` | `requestQuit` (logs out), `forceQuit` (does not: avoid). |
| `LLCommandDispatcher` | App commands: `dispatch`, `enumerate`. |
| `LLFloaterAbout` | `getInfo`: what the About window shows. |
| `LLFloaterReg` | Windows: `showInstance`, `hideInstance`, `toggleInstance`, `toggleInstanceOrBringToFront`, `instanceVisible`, `clickButton`, `getBuildMap`. |
| `LLGesture` | Gestures, once logged in: `getActiveGestures`, `isGesturePlaying`, `startGesture`, `stopGesture`. |
| `LLInventory` | Inventory, read only: `getItemsInfo`, `getFolderTypeNames`, `getAssetTypeNames`, `getBasicFolderID`, `getDirectDescendants`, `collectDescendantsIf`. |
| `LLNotifications` | Dialogs and notices: `requestAdd`, `listChannels`, `listChannelNotifications`, `respond`, `cancel`, `ignore`, `forward`. |
| `LLPipeline` | What is drawn: `toggleRenderTypes`, `hasRenderType`, `disableAllRenderTypes`, `enableAllRenderTypes`, `toggleRenderFeatures`, `hasRenderFeature`, `disableAllRenderFeatures`, `enableAllRenderFeatures`, `toggleRenderInfoDisplays`, `hasRenderInfoDisplay`, `disableAllRenderInfoDisplays`, `enableAllRenderInfoDisplays`. |
| `LLStartUp` | The login's progress: `getStateTable`, `postStartupState`. |
| `LLStats` | `getPerfData`: frame times and statistics. |
| `LLTeleportHandler` | `teleport`. |
| `LLToolMgr` | Build tools: `selectTool`, `openBuildFloater`. |
| `LLURLDispatcher` | Second Life links: `dispatch`, `dispatchRightClick`, `dispatchFromTextEditor`. |
| `LLViewerControl` | Settings: `get`, `set`, `set_notation`, `toggle`, `vars`, `groups`. |
| `LLViewerWindow` | `saveSnapshot`, `requestReshape`. |
| `LLWindow` | Interface and input: `getPaths`, `getInfo`, `keyDown`, `keyUp`, `mouseDown`, `mouseUp`, `mouseMove`, `mouseScroll`, `pasteText`. |
| `UI` | `call`, `getValue`, `setSelectedByValue`. Lumen's functions for `call`: `Lumen.Inventory.Undo`, `Lumen.Inventory.Redo`. |
