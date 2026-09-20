# What Lumen changes in Firestorm

Lumen is a derivative of the **Phoenix Firestorm Viewer**, released under the
GNU Lesser General Public License version 2.1 — the same licence as the work it
is based on. The LGPL requires a derivative to state what it changed. This is
that statement.

Upstream baseline: **`Firestorm_Release_7.2.4.80712`** (commit `10bd3c9f93`).
All Lumen changes live on the branch `ai-control`, and every edit to a Firestorm
or Linden Lab file is bracketed by `<Lumen>` comments.

To see the changes exactly:

```sh
git diff Firestorm_Release_7.2.4.80712
```

## What was added

Two new files implementing a local control endpoint:

| File | What it is |
|---|---|
| `indra/newview/fsaictl.h` | the endpoint's interfaces and contracts |
| `indra/newview/fsaictl.cpp` | an HTTP server on `127.0.0.1` speaking JSON-RPC, and the tools beneath it |

The endpoint is **off by default**, binds to loopback only, and refuses any
request carrying browser headers. It exposes four tools — chat, inventory,
movement and viewer status — so an assistant can operate the viewer on the
user's behalf.

## What was changed in existing files

| File | Change |
|---|---|
| `indra/llmessage/lliohttpserver.{h,cpp}` | added `createSafe()`: binds to a given address rather than all interfaces, and returns failure instead of terminating the process when a port is busy |
| `indra/newview/llappviewer.cpp` | start the endpoint with the viewer; stop answering before shutdown |
| `indra/newview/llviewermessage.cpp` | suppress auto-opening a notecard while it is still being written |
| `indra/newview/llselectmgr.cpp` | record object names as the server reports them, for "what is nearby" |
| `indra/newview/llpreviewnotecard.h` | made one method public so a preview can be refreshed |
| `indra/newview/viewer_manifest.py` | package as `Lumen.app`; ship images alongside local HTML |
| `indra/newview/fspanellogin.cpp` | show a local start page instead of Firestorm's |
| `indra/newview/app_settings/settings.xml` | three settings: enable, port, optional token |
| `indra/llcommon/indra_constants.h` | the application name |
| `indra/llfilesystem/lldir_mac.cpp` | the macOS data directory |
| `indra/cmake/Variables.cmake` | macOS builds arm64 by default; override with `-DLUMEN_OSX_ARCH` |
| `indra/newview/CMakeLists.txt` | the new files, and the product name, bundle identifier and icon |

Artwork and interface changes: a new application icon, a local login page, and
the login panel and strings that go with it.

## Not affiliated

This project is not affiliated with, endorsed by, or supported by the Phoenix
Firestorm Project, Inc. Please do not contact them about it.
