#!/bin/sh
# Set up Lumen on a fresh Mac and build it.
#
# This exists because doing it by hand took the better part of a day. Everything
# that cost a failed run is encoded here, with the reason, so it costs nobody a
# second one.
#
#   lumen/scripts/bootstrap-mac.sh              check the toolchain, then clone and build
#   lumen/scripts/bootstrap-mac.sh --check      check only, change nothing
#   lumen/scripts/bootstrap-mac.sh --no-build   set everything up, but stop before building
#   lumen/scripts/bootstrap-mac.sh --package    also build the .dmg installer (see below)
#
# The .dmg is NOT built by default. Building it drives Finder through AppleScript
# to lay out the disk image window, which needs a logged-in GUI session and a
# responsive Finder, and times out with "AppleEvent timed out (-1712)" when it
# does not get one. That failure arrives AFTER a completely successful build and
# reports the whole thing as failed, which is how a working viewer came to look
# like a broken one on a fresh machine. Nobody setting up a development machine
# wants a disk image anyway.
#
# It never installs anything itself. Missing tools are reported with the exact
# command to fix them, because changing someone's machine unasked is not this
# script's business.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# The repository IS the viewer now, so there is nothing to clone for it.
FORK="$ROOT"
VARS="$ROOT/fs-build-variables"
BREW_PY=/opt/homebrew/bin/python3
BREW_CMAKE=/opt/homebrew/bin/cmake
CHECK_ONLY=0; NO_BUILD=0; PACKAGE=OFF
for a in "$@"; do
    case "$a" in
        --check) CHECK_ONLY=1 ;;
        --no-build) NO_BUILD=1 ;;
        --package) PACKAGE=ON ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

fail=0
say()  { printf '  %s\n' "$*"; }
bad()  { printf '  MISSING  %s\n' "$*"; fail=1; }

echo
echo "Toolchain"
echo "---------"

# Xcode. The viewer needs the full toolchain, not just command line tools.
if xcodebuild -version >/dev/null 2>&1; then
    say "Xcode        $(xcodebuild -version | head -1 | cut -d' ' -f2)"
else
    bad "Xcode — install from the App Store, then: sudo xcode-select -s /Applications/Xcode.app"
fi

# CMake lives in at least three places on a Mac and only one of them is on PATH.
# Firestorm's own docs point at the CMake.app bundle; Homebrew puts it in
# /opt/homebrew/bin. One machine here had the app bundle recorded in its build
# cache and the app itself long gone. Look everywhere before declaring it absent.
CMAKE=""
for c in "$BREW_CMAKE" /Applications/CMake.app/Contents/bin/cmake \
         "$HOME/Applications/CMake.app/Contents/bin/cmake" /usr/local/bin/cmake; do
    [ -x "$c" ] && { CMAKE="$c"; break; }
done
[ -z "$CMAKE" ] && command -v cmake >/dev/null 2>&1 && CMAKE=$(command -v cmake)
if [ -n "$CMAKE" ]; then
    say "CMake        $("$CMAKE" --version | head -1 | cut -d' ' -f3)  ($CMAKE)"
    case "$CMAKE" in /Applications/*|"$HOME"/Applications/*)
        say "             note: not on PATH; the build below adds its directory" ;;
    esac
else
    bad "CMake — brew install cmake   (or install CMake.app; Firestorm asks 4.1.1+)"
fi

# Python. THE TRAP: /usr/bin/python3 is macOS's own 3.9.6. A venv built with it
# gets far enough to look like it worked. autobuild needs the Homebrew one.
PY=""
for c in "$BREW_PY" /usr/local/bin/python3 /Library/Frameworks/Python.framework/Versions/3.14/bin/python3; do
    if [ -x "$c" ]; then
        v=$("$c" -c 'import sys; print(sys.version_info[0]*100+sys.version_info[1])' 2>/dev/null || echo 0)
        [ "$v" -ge 310 ] && { PY="$c"; break; }
    fi
done
if [ -n "$PY" ]; then
    say "Python       $("$PY" --version | cut -d' ' -f2)  ($PY)"
else
    bad "Python 3.10+ — brew install python@3.14"
    say "             (/usr/bin/python3 is macOS's own 3.9.6; a venv built with it"
    say "              gets far enough to look correct and then misbehaves)"
fi

command -v git >/dev/null 2>&1 || bad "git"
command -v gh  >/dev/null 2>&1 || say "gh           not installed (only needed to clone private repos over https)"

# Apple silicon: the build is arm64-only by default (CLAUDE.md Decisions 30).
case "$(uname -m)" in
    arm64) say "Architecture arm64 — the default build target" ;;
    *)     say "Architecture $(uname -m) — NOTE: the default build is arm64 only."
           say "             For Intel: autobuild configure -c ReleaseOS -- -DLUMEN_OSX_ARCH=x86_64" ;;
esac

# Disk. A build tree reaches ~21 GB, most of it build output.
avail=$(df -g "$ROOT" | awk 'NR==2 {print $4}')
[ "$avail" -ge 40 ] && say "Disk         ${avail} GB free" \
                    || { bad "Disk — ${avail} GB free; a build tree needs ~25 GB and wants headroom"; }

if [ "$fail" -ne 0 ]; then
    echo
    echo "Install what is marked MISSING above, then run this again."
    exit 1
fi
[ "$CHECK_ONLY" -eq 1 ] && { echo; echo "Toolchain OK."; exit 0; }

echo
echo "Sources"
echo "-------"
# The viewer is not cloned any more: this repository IS the viewer, with our
# own scripts and assets under lumen/. What still has to be arranged is the
# upstream remote, so that merging from Firestorm stays possible and so the
# documented `git diff Firestorm_Release_...` works in a fresh clone.
git -C "$ROOT" remote add upstream https://github.com/FirestormViewer/phoenix-firestorm.git 2>/dev/null || true
if git -C "$ROOT" rev-parse -q --verify Firestorm_Release_7.2.4.80712 >/dev/null 2>&1; then
    say "upstream tag already present"
else
    say "fetching the upstream baseline tag"
    git -C "$ROOT" fetch --no-tags upstream tag Firestorm_Release_7.2.4.80712 >/dev/null 2>&1 \
        && say "upstream tag fetched" \
        || say "upstream tag NOT fetched -- \`git diff Firestorm_Release_7.2.4.80712\` will not work"
fi
# Identity is per-repo on purpose: this machine's global git user is someone else.

# Internal working documents: constraints, design notes, release gates. A
# separate private repository, so this one can be published without a deletion
# step anyone has to remember. Skipped without complaint if you cannot read it.
if [ -d "$ROOT/internal/.git" ]; then
    say "internal     already present"
elif git clone -q https://github.com/cattencarter/lumen-internal.git "$ROOT/internal" 2>/dev/null; then
    say "internal     cloned"
else
    say "internal     not available (private; skipping) — CLAUDE.md and docs/ will be absent"
fi
if [ -d "$ROOT/internal" ]; then
    ln -sfn internal/CLAUDE.md "$ROOT/CLAUDE.md"
    ln -sfn internal/docs "$ROOT/docs"
    say "             CLAUDE.md and docs/ linked"
fi

# Every repo, not just the fork -- and only now, once all of them have been
# cloned. This machine may have no global identity at all, or the wrong
# person's, and an author cannot be corrected afterwards without rewriting
# history, so it has to be right before the first commit. Two earlier bugs
# here: it was set on the fork alone, and setting it before internal/ existed
# silently skipped the repo these documents live in.
for r in "$ROOT" "$ROOT/internal" "$FORK"; do
    if git -C "$r" rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$r" config user.name  "cattencarter"
        git -C "$r" config user.email "136638600+cattencarter@users.noreply.github.com"
    fi
done
say "identity     $(git -C "$ROOT" config user.name) (every repo present)"

# The ChatGPT plugin names the bridge by absolute path. The relative form is
# the documented idiom and only works if the host sets a working directory;
# ChatGPT's desktop app does not, and the failure is silent -- the plugin
# installs, shows no tools, and says nothing about why.
if [ -x "$ROOT/scripts/stamp-plugin-path.py" ]; then
    "$BREW_PY" "$ROOT/scripts/stamp-plugin-path.py" >/dev/null 2>&1 \
        && say "plugin path  stamped for this machine" || true
fi

if [ -d "$VARS/.git" ]; then
    say "build vars   already present"
else
    say "cloning fs-build-variables"
    git clone https://github.com/FirestormViewer/fs-build-variables.git "$VARS"
fi

echo
echo "Build environment"
echo "-----------------"
# A venv hardcodes absolute paths in every script shebang, so it cannot be moved
# or copied between machines -- always build it in place.
if [ -x "$FORK/.venv/bin/autobuild" ]; then
    say "autobuild    $("$FORK/.venv/bin/autobuild" --version 2>/dev/null | head -1)"
else
    say "creating the virtualenv with $PY"
    rm -rf "$FORK/.venv"
    "$PY" -m venv "$FORK/.venv"
    "$FORK/.venv/bin/pip" install -q --disable-pip-version-check -r "$FORK/requirements.txt"
    say "autobuild    $("$FORK/.venv/bin/autobuild" --version 2>/dev/null | head -1)"
fi

[ "$NO_BUILD" -eq 1 ] && { echo; echo "Set up. Build with: lumen/scripts/bootstrap-mac.sh"; exit 0; }

echo
echo "Build"
echo "-----"
cd "$FORK"
PATH="$FORK/.venv/bin:$(dirname "$CMAKE"):/opt/homebrew/bin:$PATH"; export PATH
AUTOBUILD_VARIABLES_FILE="$VARS/variables"; export AUTOBUILD_VARIABLES_FILE

# Prebuilt package downloads fail intermittently and converge on retry. Test
# autobuild's OWN exit status -- a pipeline ending in `tail` always returns 0,
# which once made a failing configure report success.
n=0; ok=0
while [ "$n" -lt 4 ]; do
    n=$((n + 1))
    say "configure, attempt $n"
    # PACKAGE controls the .dmg target only. The .app is populated by a
    # separate POST_BUILD on the viewer binary that passes --actions=copy, so
    # turning this off costs nothing but the disk image. Verified against
    # newview/CMakeLists.txt rather than assumed.
    if autobuild configure -c ReleaseOS -- \
           -DVIEWER_CHANNEL:STRING=Lumen \
           -DPACKAGE:BOOL=$PACKAGE > /tmp/lumen-configure.log 2>&1; then
        ok=1; break
    fi
    grep -iE "CMake Error|Could not find" /tmp/lumen-configure.log | head -3 | sed 's/^/    /' || true
    say "  failed; retrying (package downloads converge)"
done
[ "$ok" -eq 1 ] || { echo "  configure failed four times; see /tmp/lumen-configure.log" >&2; exit 1; }
grep -i "Lumen: building for" /tmp/lumen-configure.log | sed 's/^/  /' || true

[ "$PACKAGE" = ON ] && say "packaging  the .dmg will be built too (--package)"
say "building (~11 minutes for arm64 on an M4; longer on fewer performance cores)"
if ! autobuild build -c ReleaseOS --no-configure > /tmp/lumen-build.log 2>&1; then
    # Distinguish a build that failed from one that built and then stumbled
    # while packaging -- they look identical in autobuild's exit code, and
    # conflating them sends someone hunting a compiler error that is not there.
    APP_BUILT="$FORK/build-darwin-universal/newview/Release/Lumen.app/Contents/MacOS/Lumen"
    if [ -x "$APP_BUILT" ] && ! grep -qE "^/.*:[0-9]+:[0-9]+: error:" /tmp/lumen-build.log; then
        echo "  the viewer BUILT, but a later step failed. Nothing was wrong with the code." >&2
        grep -iE "viewer_manifest.py failed|execution error|osascript" /tmp/lumen-build.log \
            | head -3 | sed 's/^/    /' >&2 || true
        echo "    the app is at $APP_BUILT" >&2
        echo "    full log: /tmp/lumen-build.log" >&2
    else
        echo "  build FAILED; see /tmp/lumen-build.log" >&2
        grep -E "^/.*:[0-9]+:[0-9]+: error:" /tmp/lumen-build.log | head -10 | sed 's/^/    /' || true
    fi
    exit 1
fi

APP="$FORK/build-darwin-universal/newview/Release/Lumen.app"
echo
echo "Result"
echo "------"
say "app          $APP"
say "archs        $(lipo -archs "$APP/Contents/MacOS/Lumen" 2>/dev/null)"
say "identifier   $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist" 2>/dev/null)"
say "icon         $([ -f "$APP/Contents/Resources/lumen_icon.icns" ] && echo present || echo MISSING)"
say "login page   $([ -f "$APP/Contents/Resources/skins/default/html/en-us/lumen/login.html" ] && echo present || echo MISSING)"
echo
echo "Done. Start it with the endpoint on:  scripts/try-endpoint.sh"
