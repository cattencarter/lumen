# Contributing to Lumen

Lumen is one person's spare-time project. Issues and pull requests are welcome
here, on
[cattencarter/lumen-viewer](https://github.com/cattencarter/lumen-viewer).

**Not on Firestorm's tracker.** This viewer is not theirs and its bugs are not
their problem.

## Where the work goes

- The branch is **`ai-control`**. `upstream` is the Phoenix Firestorm
  repository, and this fork keeps their history so that merging from them
  stays possible.
- New behaviour goes in **new files** wherever it can. Every edit to a file
  Firestorm or Linden Lab owns is a merge conflict waiting to happen, so each
  one is bracketed by `<FS:AICtl>` comments and kept as small as it can be.
- Copyright and licence notices in existing files stay exactly as they are.
  The LGPL requires it, and it is the right thing regardless.

## Before opening a pull request

Two checks run without a viewer and without a build:

```sh
scripts/actions-check.py    # the tool surface's lists must all agree
scripts/bridge-check.py     # the LSL bridge still answers what the viewer asks
```

Both live in the [tooling repository](https://github.com/cattencarter/lumen).
There is no unit-test target: everything else here is checked against a running,
logged-in viewer, on the **beta grid**, because most of what this code does is
only observable in a real session.
