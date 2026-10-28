# What Lumen changes in Firestorm

Lumen is a derivative of the **Phoenix Firestorm Viewer**, released under the
GNU Lesser General Public License version 2.1 — the same licence as the work it
is based on. The LGPL requires a derivative to state what it changed in that
work. This is that statement; it names upstream files and features where Lumen
changes them, which implies no involvement of the upstream projects in Lumen.

Upstream baseline: **`Firestorm_Release_7.2.5.81788`** (commit `4ac201fdc5`).
Every edit to a Firestorm or Linden Lab file is bracketed by `<Lumen>` comments
(the oldest few say `<FS:AICtl>`), and Lumen's own files are named `lumen*`.
Upstream file and symbol names that say Firestorm, such as `Info-Firestorm.plist`
and `ROOT_FIRESTORM_FOLDER`, are kept, so that merging from upstream stays simple.

To see the changes exactly, rather than taking this file's word for it,
compare with the upstream release Lumen is based on:

```sh
git fetch --no-tags https://github.com/FirestormViewer/phoenix-firestorm.git tag Firestorm_Release_7.2.5.81788
git diff Firestorm_Release_7.2.5.81788
```

Lumen is built for macOS on Apple silicon and for 64-bit Windows. The Linux
files stay in the tree, untouched apart from the icon's name.

## Identity

| Where | Change |
|---|---|
| `indra/llcommon/indra_constants.h`, `indra/cmake/BuildVersion.cmake` | the application name, and Lumen's own version number (`LUMEN_VERSION`), which is the viewer's version: what Second Life, the window title, About and Finder see. The build stops if it is not three numbers. Firestorm's number is kept from `VIEWER_VERSION_FS.txt` |
| `indra/newview/llversioninfo.{h,cpp}`, `fsversionvalues.h.in`, `llappviewer.cpp`, `skins/default/xui/*/strings.xml` | Firestorm's version kept beside Lumen's and shown in brackets in About; the release-notes link goes to Lumen's release; the "V7 style" windows keep Firestorm's generation |
| `indra/llfilesystem/lldir_mac.cpp` | a data folder of Lumen's own on macOS, as the application name gives it on Windows, so it never shares an installed Firestorm's settings, cache or chat logs |
| `indra/newview/CMakeLists.txt` | product name, bundle identifier, bundle name, icon, copyright |
| `indra/newview/English.lproj/InfoPlist.strings`, `Firestorm.xib` | the names macOS shows in the menu bar, the application menu and the About panel |
| `indra/newview/Info-Firestorm.plist` | the reasons macOS gives when it asks for the microphone, and for speech recognition |
| `indra/newview/viewer_manifest.py`, `fs_viewer_manifest.py`, `indra/lib/python/indra/util/llmanifest.py` | package as `Lumen.app` on macOS and under Lumen's name on Windows; the download is named `Lumen-<version>-<platform>`; release and symbol file names without "Phoenix"; the channel vendor base |
| `indra/newview/icons/*/lumen_icon.*`, `res/lumen_icon.*`, `lumen_icon.icns`, `installers/windows/lumen_icon.ico`, `skins/default/textures/windows/login_fs_logo.png` | Lumen's icon and logo |
| `indra/newview/installers/windows/*`, `res/viewerRes.rc`, `llvelopack.cpp` | the Windows installer, uninstaller and executable metadata: Lumen's name, publisher, registry key and links |
| `indra/newview/llappviewer.cpp` | the window title; "Lumen Viewer" on the login screen instead of a name ending in `_x64`; support links |
| `indra/llcommon/llerror.cpp`, `indra/llui/lltransutil.cpp`, `indra/llui/llurlentry.cpp` | the "missing files" message and Firestorm ticket ids typed in chat point at Lumen's releases and issues, not Firestorm's |
| `indra/newview/skins/default/xui/*/strings.xml`, `floater_about.xml`, `panel_login.xml`, `teleport_strings.xml`, `xui/*/*.xml` (all thirteen languages) | the name, download and release-notes addresses; About says Lumen is built on Firestorm and is not supported by them; account creation and support go to Second Life's own pages; "Firestorm" in labels and tooltips becomes the application name; Firestorm's own Preferences tab is called Extras |
| `autobuild.xml` | the licence and copyright text |

## Firestorm's servers and support

Lumen does not call the Phoenix Firestorm Project's servers and does not send
its users to their volunteers.

| Where | Change |
|---|---|
| `indra/newview/fsdata.cpp` | FSData fetches nothing: no message of the day (Second Life's own is shown instead), no blocked-release list, no staff badges, no shared asset blocklist, no RLVa compatibility list (a local one can still be filled in), no client-tag list |
| `indra/newview/llappviewer.cpp` | the remote settings-defaults override (`fsdata_defaults.<version>.xml`) is no longer loaded, even when a copy is left from a Firestorm install |
| `indra/newview/fsgridhandler.cpp`, `llviewernetwork.cpp`, `app_settings/grids.xml` | only Second Life's main and beta grids; no login page on Firestorm's server, and Linden Lab's own splash as the fallback |
| `indra/newview/app_settings/settings.xml` | no grid-list download, grid builder, wiki help or Jira addresses; Report a Bug opens Lumen's GitHub issues; the grid status feed is Linden Lab's own |
| `indra/newview/llviewerhelp.cpp`, `xui/en/notifications.xml` | a help topic never opens Firestorm's wiki; a notice says there are no help pages and the Assistant can answer instead |
| `indra/llui/llurlregistry.cpp`, `skins/default/textures/icons/fstrusted.png` | Firestorm's websites are no longer marked as trusted links with Firestorm's emblem |
| `indra/newview/skins/default/xui/*/menu_viewer.xml`, `menu_login.xml`, `floater_*.xml`, `panel_preferences_*.xml`, `notifications.xml` (all languages) | Firestorm's blog, social media, wiki, troubleshooting, support group, classes and events calendar are gone from the menus, and their wiki links from windows and notices; F1 opens Second Life's wiki |
| `indra/newview/skins/default/xui/*/panel_preferences_crashreports.xml` | the crash-report option no longer names firestormviewer.org; Lumen's builds are made without BugSplat, so no crash reports are sent |
| `indra/newview/app_settings/beams/Phoenix.xml`, `beamsColors/Phoenix.xml`, `panel_preferences_firestorm.xml` | the Phoenix-shaped selection beam is removed |
| `indra/newview/installers/windows/installer_template.nsi` | the AVX2 checks are removed: Lumen ships one Windows build, so there is no other build to offer or refuse |

## Interface

| Where | Change |
|---|---|
| `indra/newview/skins/lumen/`, `skins/skins.xml`, `skins/default/textures/skinspreview/lumen_teal.jpg`, `textures/widgets/PushButton_Login.png`, `app_settings/settings.xml`, `settings_lumen.xml` | one skin, Lumen's: Firestorm's skin with the orange accent turned to the icon's teal, with no sub-themes. It is the default |
| `indra/newview/skins/{ansastorm,ansastorm_modern,firestorm,metaharper,starlight,starlightcui,vintage}/`, `textures/skinspreview/*`, `textures.xml` | the upstream skins, their themes, textures and translations, and their previews, are not shipped (Firestorm's own skin became Lumen's) |
| `indra/newview/skins/default/html/en-us/lumen/`, `fspanellogin.{h,cpp}` | a local start page, shipped with the viewer, instead of a web page; `LumenUseStockLoginPage` brings the old one back |
| `indra/newview/app_settings/settings_lumen.xml`, `llappviewer.cpp`, `fspanellogin.{h,cpp}`, `xui/*/panel_fs_login.xml`, `panel_fs_nui_login.xml`, `fsdata.cpp` | one session settings file, Firestorm's renamed, instead of the login modes; the Mode picker is gone from the login screen and "Settings mode" from the viewer information. The other five mode files are deleted |
| `indra/newview/skins/default/toolbars.xml`, `app_settings/commands.xml`, `textures/toolbar_icons/assistant.png`, `skills.png`, `textures.xml`, `xui/en/strings.xml` | Assistant and Skills buttons; they replace How To and Voice on the default bottom toolbar |
| `indra/newview/skins/default/xui/en/menu_viewer.xml` | Assistant, Skills and Assistant Memory in the Comm menu |
| `indra/newview/skins/default/xui/en/menu_inventory_gear_default.xml` | Undo and Redo in the inventory's gear menu (see the Assistant) |
| `indra/newview/skins/default/xui/en/floater_preferences.xml`, `panel_preferences_ai.xml`, `panel_lumen_permission_row.xml` | an AI tab in Preferences: the provider, the permissions, the mic, answering while away |
| `indra/newview/app_settings/settings.xml` | `RenderHiDPI` on by default: with it off on a Retina display the Log In button sat off the right edge of the window |
| `indra/llui/lldraghandle.{h,cpp}` | room before a window's title for a small button of its own |
| `indra/newview/skins/default/colors.xml` | the colour of the assistant's name in its window |

## The Assistant

An assistant inside the viewer that does what the person asks in their own
words. It is off until somebody chooses an AI: `LumenAIProvider` is `none` by
default, and the viewer is then an ordinary viewer. Every tool goes through one
front door, the request handler in `lumenaictl.cpp`.

| Where | Change |
|---|---|
| `indra/newview/lumenaichat.{h,cpp}`, `skins/default/xui/en/floater_ai_chat.xml`, `llviewerfloaterreg.cpp` | the Assistant window: the conversation with the chosen AI, its tool calls, Clear, the mic button, a summary of what was waiting when it is first opened after a login (`LumenAICatchUpAtLogin`), and answering for the person while they are away. That is turned on by asking, expires (`LumenAIAutoRespond*`, per account), answers friends only by default, and is capped per person and in total |
| `indra/newview/lumenaictl.{h,cpp}` | the assistant's tools — inventory, outfits, chat, instant messages, groups, people, movement and teleports, the camera, photos and lighting, building, scripts and notecards, music, gestures, dialogues, search and the viewer's own settings — and a record of the last 200 things it did; a notice at login, until the person ticks it away, says what Lumen is and what an AI provider may be sent. The viewer asks the person before an act that speaks for them or changes something (say, send, give, delete, build, teleport and more), with Always allow, Ask and Always refuse for each kind in Preferences |
| `indra/newview/lumenaikeys.{h,cpp}` | the providers: Anthropic, OpenAI and Mistral with an API key, a model on this computer (Ollama, LM Studio), or the person's ChatGPT, Claude or Mistral subscription through Codex, Claude Code or Mistral Vibe. API keys are kept in the viewer's protected store, beside the Second Life password, never in `settings.xml` |
| `indra/newview/lumenaicodex.{h,cpp}`, `lumenaiclaude.{h,cpp}`, `lumenaivibe.{h,cpp}`, `lumenaiwin.h` | starting Codex, Claude Code and Mistral Vibe, which Lumen does not ship: each is the vendor's own program, signed in by the person, and is given Lumen's tools and no others. On Windows they run with no console window |
| `indra/newview/lumenaisetup.{h,cpp}`, `skins/default/xui/en/floater_ai_setup.xml` | "Set it up for me": installs and signs in Codex, Claude Code or Mistral Vibe, a step at a time, with the vendor's own installer. With no AI chosen, the Assistant offers this once, by itself, at the first login (`LumenAISetupOfferShown`) |
| `indra/newview/lumenaimemory.{h,cpp}`, `skins/default/xui/en/floater_ai_memory.xml` | what the assistant should know about the person: a short note, and things they asked it to remember, per account in `ai_memory.txt` and `ai_remembered.txt`, sent with each request |
| `indra/newview/lumenaiindex.{h,cpp}` | an inventory search that scores every item and returns the best matches, kept in memory |
| `indra/newview/lumenainotecache.{h,cpp}`, `sqlite/` | notecard text kept per account in `notecard_cache.db`, so searching inside notecards does not fetch every one again |
| `indra/newview/lumenaiundo.{h,cpp}`, `llviewermessage.cpp` | undo and redo for the inventory, for this login only and kept nowhere on disk: the assistant's changes and the person's own. One set of rules for what the assistant may do to an item: never the Library, Marketplace listings or the LSL bridge's folder, and nothing moved out of, renamed or deleted in a folder the person protected. Changing many items at once, emptying the Trash, and saving the scripts it writes are off until the person switches them on (`LumenAIAllowBulkInventory`, `LumenAIAllowSaveScripts`) |
| `indra/newview/lumenaisight.{h,cpp}` | pictures of the world for the model, rendered off-screen without any avatar, animated object, particles or worn light in them, objects numbered so the model can say which it means; the person's view does not move. `LumenAIShowPictures` shows them in the window |
| `indra/newview/lumenaispeech.h`, `lumenaispeech_mac.mm`, `lumenaispeech_win.cpp` | the mic button: talking instead of typing, into the Assistant's typing box. The Mac uses Apple's speech recognition; Windows uses Whisper on the computer (see Network). `LumenAISpeech*` |
| `indra/newview/llagentcamera.cpp` | when the assistant has framed a photograph, the avatar looks at the lens |
| `indra/newview/llselectmgr.{h,cpp}`, `llstartup.cpp` | object names, and the world directory's places and events, are passed to the assistant as well as to the windows that asked |
| `indra/newview/llviewermessage.cpp`, `llpreviewnotecard.h` | a notecard the assistant is still writing is not opened half-written |
| `indra/newview/llgiveinventory.h`, `llselectmgr.h` | made public, so a give or a delete the person confirmed in the conversation needs no second dialogue |
| `indra/llmessage/lliohttpserver.{h,cpp}` | `createSafe()`: a server that binds to the address it is given rather than every interface, and returns failure instead of ending the program when its port is taken |
| `indra/llcommon/llprocess.{h,cpp}` | on Windows, a program the viewer starts can be given no console window, nor can anything it starts |
| `indra/newview/llappviewer.cpp` | the Assistant started with the viewer, and stopped before anything it uses is taken down |
| `indra/newview/skins/default/xui/en/notifications.xml` | the Assistant's questions and notices |
| `indra/newview/app_settings/settings.xml`, `settings_per_account.xml` | the `Lumen*` settings |

## Skills

| Where | Change |
|---|---|
| `indra/newview/lumenaiskills.{h,cpp}`, `lltoolgrab.cpp` | a routine taught once by doing it — touching a HUD's button by part, face and spot, answering a menu, rezzing, sitting, accepting an offer, a web call — and carried out on request, each step checked before the next, stopping with the reason when one fails. Kept as a notecard in `#Lumen/#Skills`, so it follows the person. The viewer asks before a skill runs, and says so when its card was not written by Lumen on this computer |
| `indra/newview/skins/default/xui/en/floater_lumen_skills.xml`, `textures/icons/Lumen_Skills_List.png` | the Skills window: every skill, its inputs as lists, and Run |

## Inventory and the LSL bridge

| Where | Change |
|---|---|
| `indra/newview/fs_resources/lumen_bridge.lsltxt` | a bridge script written fresh, implementing the same command set as the upstream bridge, plus two commands of its own (`worn`, `object`). Firestorm's script stays in the tree, unused |
| `indra/newview/fslslbridge.{h,cpp}` | Lumen's bridge is "#Lumen LSL Bridge" in Lumen's own `#Lumen` folder; Firestorm's bridge, if it is worn, is taken off rather than mistaken for a broken copy of Lumen's; bridges Lumen made under Firestorm's name are cleaned up; the log says whether the bridge script compiled |
| `indra/newview/lumenfolders.{h,cpp}`, `llinventoryfunctions.h`, `llinventorybridge.cpp`, `llstartup.cpp` | the AO and the other features Lumen shares with Firestorm use an existing `#Firestorm` folder, untouched, where there is one, and `#Lumen` otherwise; `#Lumen` is kept in place like `#Firestorm` |

## Network

What Lumen adds that talks to the network, besides Second Life itself. With no
AI chosen, only the update message and the parcel-music retry can happen, and
"Set it up for me" when the person asks for it.

| What | Host | When, and what turns it off |
|---|---|---|
| The update message (`lumenupdate.{h,cpp}`, `llstartup.cpp`) | `needfulsl.com` (`LumenUpdateURL`) | one request for `version.json`, once a session, at the login screen. It only tells: a newer version is told once a day, with Download and Later. `LumenUpdateCheck` off: no request |
| The Assistant with an API key (`lumenaichat.cpp`) | `api.anthropic.com`, `api.openai.com` or `api.mistral.ai`, or another address (`LumenAIAnthropicURL`, `LumenAIOpenAIURL`, `LumenAIMistralURL`) | each turn of a conversation the person starts, and each reply made for them while they are away: the conversation, the memory note, what the tools found and pictures of the world they took. `LumenAIProvider` `none` (the default) |
| A model on this computer | `LumenAILocalURL`, `localhost` by default | as above; nothing leaves the computer unless the address is changed |
| Codex, Claude Code, Mistral Vibe | the vendor's own servers, reached by the vendor's own program with the person's own sign-in | as above. Lumen starts the program only when it is the chosen provider |
| The local endpoint (`lumenaictl.cpp`, `indra/llmessage/lliohttpserver.cpp`) | listens on `127.0.0.1` only, on a port picked at random each start | only while Codex, Claude Code or Mistral Vibe is the provider, so those programs can reach Lumen's tools; nothing on the network can reach it, and a request carrying a browser's `Origin` or `Sec-Fetch-Site` header is refused. `LumenAIControlPort`, for testing, fixes the port and keeps it listening whatever the provider |
| "Set it up for me" (`lumenaisetup.cpp`, `lumenaikeys.cpp`) | `chatgpt.com`, `claude.ai`, `mistral.ai`, and on Windows `astral.sh` for Mistral Vibe | only when the person asks: downloads and runs that vendor's installer, then its sign-in in the browser |
| Searching for places (`lumenaictl.cpp`) | `search.secondlife.com`, with an anonymous sign-in at `id.secondlife.com` (Linden Lab's own) | when the assistant searches for places; at most one search a second; the in-world directory is used when the site cannot be read |
| Where a person is on the web (`lumenaictl.cpp`) | `www.primfeed.com` and `marketplace.secondlife.com` | only when the assistant is asked about a person's Primfeed or Marketplace store. Primfeed learns that someone looked that person up |
| A skill's web step (`lumenaiskills.cpp`) | an https address the skill names | the person approves each address the first time; a key, if the site needs one, is typed by the person into the viewer's own window. No redirects, and the certificate must be that host's |
| The mic on a Mac (`lumenaispeech_mac.mm`) | Apple's speech recognition, which may send the audio to Apple | only while the mic button is listening |
| The mic on Windows (`lumenaispeech_win.cpp`) | `huggingface.co` | a one-time download of the Whisper speech model (190 MB, pinned and checked), after the person agrees to it; recognition then runs on the computer. Preferences can remove it |
| Parcel music (`llviewermedia_streamingaudio.{h,cpp}`) | the parcel's stream | a stream that will not open over https is tried once over plain http, and the log says so; a stream that drops is reconnected up to three times |

## Build

| Where | Change |
|---|---|
| `indra/cmake/Variables.cmake` | macOS deploy target raised to 12.0 for Xcode 27, with upstream's deprecation warnings kept as warnings; arm64 by default (override with `-DLUMEN_OSX_ARCH`) |
| `indra/cmake/00-Common.cmake` | the build machine's paths mapped to the checkout, and the linker's debug map made relative, so no home folder is named in the binaries |
| `indra/newview/CMakeLists.txt` | Lumen's sources; Apple's Speech and AVFoundation frameworks on macOS; `winmm` and `bcrypt` on Windows; SQLite from the system on macOS and Linux |
| `indra/newview/sqlite/` | the SQLite amalgamation (public domain), compiled on Windows only, where the system has none |
| `indra/newview/viewer_manifest.py` | no skin themes packaged; images beside local HTML pages packaged; Whisper packaged on Windows when the build made it |
| `.github/workflows/lumen-windows.yml` | the Windows build: the viewer, its installer, and Whisper (whisper.cpp v1.9.4) built from source. Upstream's workflows, pull-request template and `CONTRIBUTING.md` are not carried |
| `lumen/` | build, signing, release and check scripts, and the icon's sources; MIT, apart from the name and the icon (`lumen/NOTICE.md`) |
| `README.md`, `.gitignore` | Lumen's own |

## Fixes to upstream code

| Where | Change |
|---|---|
| `indra/llwebrtc/llwebrtc.cpp`, `llwebrtc_impl.h` | the microphone is prepared when the person first unmutes or tests it, not when voice connects, so macOS does not ask for the microphone before anybody speaks |
| `indra/newview/llviewermedia_streamingaudio.{h,cpp}` | a music stream that will not play says why, instead of the play button only flicking back; a parcel whose address says `https` for a server that only speaks http still plays (see Network); a dropped stream reconnects |
| `indra/newview/fslslbridge.cpp` | the server's answer to the bridge script upload is read, so a script that does not compile is reported rather than logged as "Bridge created" |
| `indra/newview/viewer_manifest.py` | packaging no longer fails when the channel has no suffix |
| `indra/newview/llselectmgr.cpp` | no "object not selected" warning for the property replies Lumen asked for, as for Area Search's |

## Not affiliated

This project is not affiliated with, endorsed by, or supported by the Phoenix
Firestorm Project, Inc. or Linden Lab. Please do not contact them about it.
