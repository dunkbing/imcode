#!/bin/sh
# a release build as dist/ImCode.app and dist/imcode-macos-arm64.zip, for macos 14 and later.
# VCPKG_ROOT must point at vcpkg. signed and notarized with the variables below; without them
# it is signed ad hoc, and another mac opens it only after right-click > Open
# auto-update: IMCODE_APPCAST_URL and IMCODE_UPDATE_KEY (the ed25519 public key) go into
# Info.plist with Sparkle embedded; without them the app does not update itself.
# IMCODE_VERSION overrides the project's version (a test build of an update)
set -e
cd "$(dirname "$0")/.."
: "${VCPKG_ROOT:?set VCPKG_ROOT}"
version=${IMCODE_VERSION:-$(sed -n 's/^project(imcode VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=arm64-osx-release -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets \
    -DIMCODE_UPDATER=ON -DIMCODE_VERSION="$version"
cmake --build build-release -j 4 --target imcode

app=dist/ImCode.app
rm -rf dist && mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks"
cp build-release/imcode "$app/Contents/MacOS/imcode"
strip -x "$app/Contents/MacOS/imcode"
# sparkle from the build's download, found through the bundle's Frameworks
ditto build-release/_deps/sparkle-src/Sparkle.framework "$app/Contents/Frameworks/Sparkle.framework"
install_name_tool -add_rpath @executable_path/../Frameworks "$app/Contents/MacOS/imcode"
updates=""
if [ -n "$IMCODE_APPCAST_URL" ] && [ -n "$IMCODE_UPDATE_KEY" ]; then
    updates="<key>SUFeedURL</key><string>$IMCODE_APPCAST_URL</string>
    <key>SUPublicEDKey</key><string>$IMCODE_UPDATE_KEY</string>
    <key>SUEnableAutomaticChecks</key><true/>"
fi
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>ImCode</string>
    <key>CFBundleDisplayName</key><string>ImCode</string>
    <key>CFBundleIdentifier</key><string>io.github.dunkbing.imcode</string>
    <key>CFBundleExecutable</key><string>imcode</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
    $updates
</dict>
</plist>
PLIST
# signing. IMCODE_SIGN_IDENTITY: a "Developer ID Application: ..." identity in the keychain;
# unset: ad hoc. sparkle's parts are signed inside out, as its docs ask
identity=${IMCODE_SIGN_IDENTITY:--}
if [ "$identity" = "-" ]; then
    codesign --force --deep -s - "$app"
else
    sign() { codesign --force --timestamp --options runtime -s "$identity" "$@"; }
    fw="$app/Contents/Frameworks/Sparkle.framework"
    sign "$fw/Versions/B/XPCServices/Installer.xpc"
    sign --preserve-metadata=entitlements "$fw/Versions/B/XPCServices/Downloader.xpc"
    sign "$fw/Versions/B/Autoupdate"
    sign "$fw/Versions/B/Updater.app"
    sign "$fw"
    sign "$app"
fi
ditto -c -k --keepParent "$app" dist/imcode-macos-arm64.zip
# notarizing: IMCODE_NOTARY_PROFILE (a notarytool keychain profile, on a mac of your own), or
# an app store connect api key in IMCODE_NOTARY_KEY (the .p8 file), _KEY_ID and _ISSUER (ci)
if [ -n "$IMCODE_NOTARY_PROFILE" ]; then
    notary="--keychain-profile $IMCODE_NOTARY_PROFILE"
elif [ -n "$IMCODE_NOTARY_KEY" ]; then
    notary="--key $IMCODE_NOTARY_KEY --key-id $IMCODE_NOTARY_KEY_ID --issuer $IMCODE_NOTARY_ISSUER"
fi
if [ -n "$notary" ]; then
    # shellcheck disable=SC2086
    xcrun notarytool submit dist/imcode-macos-arm64.zip $notary --wait
    xcrun stapler staple "$app"
    rm dist/imcode-macos-arm64.zip
    ditto -c -k --keepParent "$app" dist/imcode-macos-arm64.zip
fi
echo "dist/imcode-macos-arm64.zip ($version)"
