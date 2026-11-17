#!/bin/bash
# Signs Lathe.app with the Developer ID certificates, has Apple notarize it, and makes the downloads
# in dist/:
#   Lathe-<version>.dmg   open it and drag Lathe to Applications
#   Lathe-<version>.pkg   an installer that puts Lathe in Applications
#   Lathe-mac.zip         the app on its own
#   SIGNING.txt           what was signed and notarized
#
# The certificates and Apple ID come from the repository's secrets (see README, "Signing"):
#   MACOS_CERTS_P12       base64 of a .p12 holding "Developer ID Application" (and, for the .pkg,
#                         "Developer ID Installer") with their private keys
#   MACOS_CERTS_PASSWORD  the .p12's password
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD   for notarization (an app-specific password)
# Without them the app gets an ad-hoc signature as before, so a build never stops for want of a
# certificate; macOS then asks for right-click > Open the first time.
set -euo pipefail

APP=$(find build -name "Lathe.app" -type d | grep Release | head -1)
test -n "$APP"
VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")
ENT="resources/Lathe.entitlements"
TMP="${RUNNER_TEMP:-$(mktemp -d)}"
rm -rf dist
mkdir -p dist

APP_ID=""
INST_ID=""
NOTARIZE=0

if [ -n "${MACOS_CERTS_P12:-}" ]; then
    KC="$TMP/lathe-signing.keychain-db"
    KCPASS=$(uuidgen)
    security create-keychain -p "$KCPASS" "$KC"
    security set-keychain-settings -lut 21600 "$KC"
    security unlock-keychain -p "$KCPASS" "$KC"
    # Apple's Developer ID intermediate certificates, so the identities are complete.
    for ca in DeveloperIDG2CA DeveloperIDCA; do
        if curl -fsSL --retry 3 -o "$TMP/$ca.cer" "https://www.apple.com/certificateauthority/$ca.cer"; then
            security import "$TMP/$ca.cer" -k "$KC" >/dev/null 2>&1 || true
        fi
    done
    printf '%s' "$MACOS_CERTS_P12" | base64 --decode > "$TMP/certs.p12"
    security import "$TMP/certs.p12" -k "$KC" -P "${MACOS_CERTS_PASSWORD:-}" -T /usr/bin/codesign -T /usr/bin/pkgbuild -T /usr/bin/productsign
    rm -f "$TMP/certs.p12"
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KCPASS" "$KC" >/dev/null
    # shellcheck disable=SC2046
    security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
    IDS=$(security find-identity -v "$KC" || true)
    APP_ID=$(printf '%s\n' "$IDS" | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"' || true)
    INST_ID=$(printf '%s\n' "$IDS" | grep -o '"Developer ID Installer: [^"]*"' | head -1 | tr -d '"' || true)
    if [ -z "$APP_ID" ]; then
        echo "::error::The certificates secret holds no valid \"Developer ID Application\" identity. Found:"
        printf '%s\n' "$IDS"
        exit 1
    fi
    echo "Signing as: $APP_ID"
    [ -n "$INST_ID" ] && echo "Installer signed as: $INST_ID"
    if [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then NOTARIZE=1; fi
fi

# Sends a file to Apple and waits for the verdict; prints Apple's log and stops the build if refused.
notarize() {
    local file="$1" out status id
    echo "Notarizing $(basename "$file")..."
    out=$(xcrun notarytool submit "$file" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" \
          --wait --timeout 60m --output-format json) || true
    echo "$out"
    status=$(printf '%s' "$out" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("status",""))
except Exception: print("")')
    id=$(printf '%s' "$out" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("id",""))
except Exception: print("")')
    if [ "$status" != "Accepted" ]; then
        echo "::error::Apple did not accept $(basename "$file") (status: ${status:-no answer})."
        [ -n "$id" ] && xcrun notarytool log "$id" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" || true
        exit 1
    fi
}

# hdiutil on build machines sometimes reports the disk busy: try a few times.
make_dmg() {
    local src="$1" out="$2" i
    for i in 1 2 3 4 5; do
        if hdiutil create -volname "Lathe $VERSION" -srcfolder "$src" -ov -fs HFS+ -format UDZO "$out"; then return 0; fi
        echo "hdiutil failed (attempt $i); retrying"
        sleep 5
    done
    return 1
}

# ---- the app
if [ -n "$APP_ID" ]; then
    codesign --force --timestamp --options runtime --sign "$APP_ID" "$APP/Contents/Resources/lame"
    codesign --force --timestamp --options runtime --entitlements "$ENT" --sign "$APP_ID" "$APP"
else
    codesign --force --deep --sign - "$APP"
fi
codesign --verify --strict --deep --verbose=2 "$APP"
if [ -n "$APP_ID" ] && ! codesign -d --entitlements - "$APP" 2>&1 | grep -q "audio-input"; then
    echo "::error::The signed app is missing its audio input entitlement: recording would be silent."
    exit 1
fi

if [ "$NOTARIZE" = 1 ]; then
    ditto -c -k --keepParent "$APP" "$TMP/Lathe-notarize.zip"
    notarize "$TMP/Lathe-notarize.zip"
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    spctl --assess --type execute --verbose=2 "$APP"
fi
ditto -c -k --keepParent "$APP" dist/Lathe-mac.zip

# ---- the disk image: the app and a shortcut to Applications to drag it onto
STAGE="$TMP/dmg"
rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/Lathe.app"
ln -s /Applications "$STAGE/Applications"
DMG="dist/Lathe-$VERSION.dmg"
make_dmg "$STAGE" "$DMG"
if [ -n "$APP_ID" ]; then codesign --force --timestamp --sign "$APP_ID" "$DMG"; fi
if [ "$NOTARIZE" = 1 ]; then
    notarize "$DMG"
    xcrun stapler staple "$DMG"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
fi

# ---- the installer package: always into /Applications (never "upgrading" a copy found elsewhere)
ROOT="$TMP/pkgroot"
rm -rf "$ROOT"
mkdir -p "$ROOT"
ditto "$APP" "$ROOT/Lathe.app"
pkgbuild --analyze --root "$ROOT" "$TMP/components.plist"
plutil -replace 0.BundleIsRelocatable -bool NO "$TMP/components.plist"
PKG="dist/Lathe-$VERSION.pkg"
PKGARGS=(--root "$ROOT" --component-plist "$TMP/components.plist" --install-location /Applications
         --identifier com.newconstellation.lathe.pkg --version "$VERSION")
if [ -n "$INST_ID" ]; then
    pkgbuild "${PKGARGS[@]}" --sign "$INST_ID" --timestamp "$PKG"
else
    pkgbuild "${PKGARGS[@]}" "$PKG"
fi
if [ "$NOTARIZE" = 1 ] && [ -n "$INST_ID" ]; then
    notarize "$PKG"
    xcrun stapler staple "$PKG"
    spctl --assess --type install --verbose=2 "$PKG"
fi

# ---- what was done
{
    echo "Lathe $VERSION"
    if [ -n "$APP_ID" ]; then echo "App and disk image signed: $APP_ID"; else echo "App: ad-hoc signature only (no certificate in the repository's secrets)"; fi
    if [ "$NOTARIZE" = 1 ]; then echo "Notarized by Apple and stapled: app, disk image"; else echo "Not notarized"; fi
    if [ -n "$INST_ID" ]; then
        echo "Installer package signed: $INST_ID$( [ "$NOTARIZE" = 1 ] && echo ", notarized and stapled")"
    else
        echo "Installer package: unsigned (needs a Developer ID Installer certificate)"
    fi
} > dist/SIGNING.txt
cat dist/SIGNING.txt
ls -la dist
