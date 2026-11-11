#!/bin/sh
# a release build as dist/ImCode.app and dist/imcode-macos-arm64.zip, for macos 14 and later.
# VCPKG_ROOT must point at vcpkg. the app is signed ad hoc: on another mac, open it the
# first time with right-click > Open (or clear the quarantine flag)
set -e
cd "$(dirname "$0")/.."
: "${VCPKG_ROOT:?set VCPKG_ROOT}"
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=arm64-osx-release -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets
cmake --build build-release -j 4 --target imcode

app=dist/ImCode.app
rm -rf dist && mkdir -p "$app/Contents/MacOS"
cp build-release/imcode "$app/Contents/MacOS/imcode"
strip -x "$app/Contents/MacOS/imcode"
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
    <key>CFBundleShortVersionString</key><string>0.0.1</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force --deep -s - "$app"
ditto -c -k --keepParent "$app" dist/imcode-macos-arm64.zip
echo "dist/imcode-macos-arm64.zip"
