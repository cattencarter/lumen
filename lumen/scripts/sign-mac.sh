#!/bin/sh
# Sign, and optionally notarise, the built Lumen.app.
#
# Why this is a script of our own rather than viewer_manifest.py's signing:
# upstream's path wants a dedicated `viewer.keychain`, a build-secrets checkout
# and a password file on disk -- their CI's shape. Ours uses the login keychain
# and the identity already installed, so it needs no infrastructure, and it
# touches no upstream file so it adds nothing to the merge burden.
#
# The ORDER and the two different treatments are copied from
# viewer_manifest.py, because they are not obvious and they are load-bearing:
# the dylibs are signed plainly, and only SLPlugin, SLVoice and the app itself
# get the entitlements and the hardened runtime. CEF needs JIT and unsigned
# executable memory; the viewer does not, and asking for entitlements you do
# not need is how a notarisation gets refused.
#
#   scripts/sign-mac.sh                 sign only
#   scripts/sign-mac.sh --notarize      sign, notarise, staple
#   scripts/sign-mac.sh --dmg           also build a signed disk image
#
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
FORK="$REPO"
APP="$FORK/build-darwin-universal/newview/Release/Lumen.app"
ENTITLEMENTS="$FORK/indra/newview/slplugin.entitlements"
PROFILE="${LUMEN_NOTARY_PROFILE:-lumen-notary}"

NOTARIZE=0
MAKE_DMG=0
for a in "$@"; do
    case "$a" in
        --notarize|--notarise) NOTARIZE=1 ;;
        --dmg) MAKE_DMG=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

[ -d "$APP" ] || { echo "No build at $APP -- build the viewer first." >&2; exit 1; }
[ -f "$ENTITLEMENTS" ] || { echo "Missing $ENTITLEMENTS" >&2; exit 1; }

# The identity is found rather than hardcoded: a Developer ID is tied to a
# person, and this file is bound for a public repository.
IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null \
           | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)
if [ -z "$IDENTITY" ]; then
    cat >&2 <<'EOS'
No "Developer ID Application" identity in the keychain.

  Apple Development and iPhone Developer certificates will NOT do -- those
  sign for your own machines and for the App Store. Gatekeeper on somebody
  else's Mac wants Developer ID specifically.

  Check with:  security find-identity -v -p codesigning
EOS
    exit 1
fi
echo "==> signing as: $IDENTITY"

# ---------------------------------------------------------------- plain -----
# Everything that is merely loaded. No entitlements, no hardened runtime.
echo "==> dylibs"
R="$APP/Contents/Resources"
# shellcheck disable=SC2086
find "$R"/llplugin "$R"/llplugin/lib "$R" "$APP/Contents/Frameworks" \
     "$R/SLPlugin.app/Contents/Frameworks/Chromium Embedded Framework.framework/Libraries" \
     -maxdepth 1 -name '*.dylib' -type f -print0 2>/dev/null \
  | xargs -0 -n1 -P4 codesign --force --timestamp --sign "$IDENTITY" 2>/dev/null || true
echo "    $(find "$APP" -name '*.dylib' | wc -l | tr -d ' ') dylibs in the bundle"

# ------------------------------------------------------------------ deep ----
# The three that actually RUN, with the entitlements CEF needs and the
# hardened runtime notarisation requires.
#
# The timestamp server refuses under load often enough that upstream retries
# three times with a growing wait, and so does this.
echo "==> SLPlugin, SLVoice, and the app"
for target in \
    "$R/SLPlugin.app/Contents/MacOS/SLPlugin" \
    "$R/SLVoice" \
    "$APP"
do
    [ -e "$target" ] || { echo "    skipped (absent): $(basename "$target")"; continue; }
    wait_s=15
    n=1
    while : ; do
        if codesign --verbose --deep --force \
                    --entitlements "$ENTITLEMENTS" \
                    --options runtime --timestamp \
                    --sign "$IDENTITY" "$target" 2>&1 | sed 's/^/    /'
        then
            break
        fi
        [ "$n" -ge 3 ] && { echo "    codesign failed three times: $target" >&2; exit 1; }
        echo "    codesign failed, waiting ${wait_s}s" >&2
        sleep "$wait_s"
        wait_s=$((wait_s * 2))
        n=$((n + 1))
    done
done

# --------------------------------------------------------------- verify -----
echo "==> verifying"
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/    /'
echo "    authority: $(codesign -dvvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)"
echo "    hardened runtime: $(codesign -d --entitlements - "$APP" >/dev/null 2>&1 && codesign -dvvv "$APP" 2>&1 | grep -q 'flags=.*runtime' && echo yes || echo NO)"

# spctl says "rejected" for anything not yet notarised, which is expected and
# is NOT a statement about whether the app launches. Reported, not asserted.
echo "    spctl: $(spctl -a -vv "$APP" 2>&1 | tail -1 | sed 's/^ *//')"

if [ "$NOTARIZE" -eq 0 ]; then
    cat <<EOS

Signed, not notarised. On another Mac this still gets a Gatekeeper prompt.

To notarise, store credentials ONCE (this is yours to run -- it asks for an
app-specific password from appleid.apple.com, which must not pass through
anything else):

  xcrun notarytool store-credentials "$PROFILE" \\
      --apple-id <your-apple-id> --team-id 78Z5JA4TFR

then:  scripts/sign-mac.sh --notarize
EOS
    exit 0
fi

# ------------------------------------------------------------ notarise ------
echo "==> notarising (a few minutes)"
ZIP=$(mktemp -d)/Lumen.zip
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait 2>&1 | sed 's/^/    /'
echo "==> stapling"
xcrun stapler staple "$APP" 2>&1 | sed 's/^/    /'
echo "    spctl after stapling: $(spctl -a -vv "$APP" 2>&1 | tail -1 | sed 's/^ *//')"

if [ "$MAKE_DMG" -eq 1 ]; then
    echo "==> disk image"
    # hdiutil rather than the viewer's own installer-dmg.applescript, which
    # arranges icons by driving Finder and needs an automation grant this
    # machine does not have. The layout is cosmetic.
    STAGE=$(mktemp -d)
    cp -R "$APP" "$STAGE/"
    ln -s /Applications "$STAGE/Applications"
    OUT="$REPO/Lumen.dmg"
    rm -f "$OUT"
    hdiutil create -volname Lumen -srcfolder "$STAGE" -ov -format UDZO "$OUT" >/dev/null
    codesign --force --timestamp --sign "$IDENTITY" "$OUT"

    # The DISK IMAGE needs its own notarisation, and this is easy to get wrong:
    # the app inside is already notarised and stapled, so it is tempting to
    # staple the dmg and be done. That fails with "Record not found", because
    # a dmg has its own hash and Apple has never seen it.
    #
    # It matters rather than being tidiness. The dmg is what gets downloaded,
    # so the dmg is what carries the quarantine flag and what Gatekeeper judges
    # first. An unnotarised dmg reports `rejected / Unnotarized Developer ID`
    # and warns on open, however good the app inside it is.
    echo "    notarising the image too"
    xcrun notarytool submit "$OUT" --keychain-profile "$PROFILE" --wait 2>&1 | sed 's/^/    /'
    xcrun stapler staple "$OUT" 2>&1 | sed 's/^/    /'
    echo "    verdict: $(spctl -a -t open --context context:primary-signature -vv "$OUT" 2>&1 | tr '\n' ' ')"
    echo "    $OUT"
fi
