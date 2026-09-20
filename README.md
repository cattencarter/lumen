# Lumen

**A Second Life viewer with an assistant built into it.** You ask for what you
want in ordinary words — find something in your inventory, put it on, frame a
photograph, work out where a setting lives — and the viewer does it.

It is a proof of concept rather than a viewer to move into.

Lumen is a derivative of the
[Phoenix Firestorm Viewer](https://www.firestormviewer.org), released under the
**GNU Lesser General Public License version 2.1** — the same licence as the work
it is based on.

> ### This is not Firestorm
>
> The Phoenix Firestorm Project has nothing to do with this viewer and does not
> support it. **Please do not take Lumen's problems to their volunteers, their
> tracker or their support groups.** Anything wrong here is ours, and it belongs
> in [this repository's issues](https://github.com/cattencarter/lumen-viewer/issues).
>
> If you want a viewer to live in, use
> [Firestorm](https://www.firestormviewer.org). Their work is the reason this
> one could exist at all.

## What is changed

[CHANGES-LUMEN.md](CHANGES-LUMEN.md) states what this derivative changes, which
the LGPL requires of it. In short: a new module carrying the assistant and a
loopback control endpoint, an LSL bridge of our own, a skin, and a few dozen
small edits to upstream files — each one bracketed by `<FS:AICtl>` comments.

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

## Contributing

Issues and pull requests belong here. See [CONTRIBUTING.md](CONTRIBUTING.md).

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
