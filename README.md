# Lumen

**A Second Life viewer with an assistant built into it.** You ask for what you
want in ordinary words — find something in your inventory, put it on, frame a
photograph, work out where a setting lives — and the viewer does it.

Lumen is a derivative of the
[Phoenix Firestorm Viewer](https://www.firestormviewer.org), released under the
**GNU Lesser General Public License version 2.1** — the same licence as the work
it is based on.

> ### A proof of concept, delivered as is
>
> There is no support for this. It is made in spare time, it has rough edges,
> and it may simply stop working. Use it if it is useful to you — that is the
> whole of what it asks.
>
> **Please do not contact Firestorm about it.** The Phoenix Firestorm Project
> has no affiliation with Lumen beyond having written the excellent source base
> it is built on. Its problems are not theirs, and their volunteers did not
> sign up for them.

## What is changed

[CHANGES-LUMEN.md](CHANGES-LUMEN.md) states what this derivative changes, which
the LGPL requires of it. In short: a new module carrying the assistant and a
loopback control endpoint, an LSL bridge of our own, a skin, and a few dozen
small edits to upstream files — each one bracketed by `<Lumen>` comments.

To see the difference exactly, rather than taking this file's word for it:

```sh
git fetch --no-tags upstream tag Firestorm_Release_7.2.4.80712
git diff Firestorm_Release_7.2.4.80712
```

## Download

Built versions are on the
[releases page](https://github.com/cattencarter/lumen/releases).

## Building

Everything needed to build Lumen — toolchain checks, the build variables and
the build itself — is one command:

```sh
lumen/scripts/bootstrap-mac.sh
```

It reports what is missing rather than installing anything behind your back.
`--check` inspects and changes nothing; `--no-build` stops after setup.

Firestorm's own per-platform notes still describe the viewer underneath and are
kept here unchanged: [Windows](doc/building_windows.md) ·
[Mac](doc/building_macos.md) · [Linux](doc/building_linux.md). Where they point
at Firestorm's wiki or their self-compilers group, those are Firestorm's
resources for Firestorm's viewer — they are not a support route for this one.

## What is ours, and where

Everything Lumen adds lives under **`lumen/`** — the scripts, the icon, the
licence covering them. Everything else in this tree is Firestorm's and Linden
Lab's, and is left as it was found.

| | |
|---|---|
| `lumen/scripts/` | build, release and live-check scripts |
| `lumen/assets/` | the icon and its iconset |
| `indra/newview/fsai*`, `lumenfolders.*`, `lumen_bridge.lsltxt` | the assistant itself |

## Contributions

None, thank you. This is one person's experiment rather than a project looking
for a team, and it is published so that people can use it and read it — not so
that it acquires obligations. Issues are turned off and pull requests are not
taken.

The source is here and the licence is generous. If you want it to do something
else, fork it; that costs nobody anything.

## Licence

**The viewer is LGPL 2.1**, inherited from Firestorm and from Linden Lab's
Second Life viewer. The copyright notices throughout the source belong to their
authors and are deliberately left intact.

**`lumen/scripts/` and `lumen/assets/` are MIT** — see
[lumen/LICENSE](lumen/LICENSE). Two licences in one repository because the
viewer arrived under one and our own tooling was written under another; the
boundary is the `lumen/` directory and nothing else.

Second Life is a trademark of Linden Research, Inc. Lumen is not affiliated
with Linden Research or with the Phoenix Firestorm Project.
