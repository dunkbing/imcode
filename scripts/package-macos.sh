#!/bin/sh
# a release build as dist/ImCode.app and dist/imcode-macos-arm64.zip, for macos 14 and later.
# VCPKG_ROOT must point at vcpkg. the app is signed ad hoc: on another mac, open it the
# first time with right-click > Open (or clear the quarantine flag)
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
codesign --force --deep -s - "$app"
ditto -c -k --keepParent "$app" dist/imcode-macos-arm64.zip
echo "dist/imcode-macos-arm64.zip ($version)"
