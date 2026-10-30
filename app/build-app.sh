#!/bin/bash
# build-app.sh: build "Mount Kindle.app" into dist/ (plus a .zip, and a .dmg if hdiutil works).
# Needs: Xcode command line tools (swiftc), macFUSE 5 (libfuse3), Homebrew libmtp 1.1.23.
# Run from the repo root:  app/build-app.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
NAME="Mount Kindle"
DIST="$ROOT/dist"
APP="$DIST/$NAME.app"
BREW="$(brew --prefix)"
MTP="$(brew --prefix libmtp)"
USB="$(brew --prefix libusb)"
MTPDY="$MTP/lib/libmtp.9.dylib"
USBDY="$USB/lib/libusb-1.0.0.dylib"
command -v swiftc >/dev/null || { echo "swiftc not found: install Xcode or the command line tools" >&2; exit 1; }
[ -f "$MTPDY" ] && [ -f "$USBDY" ] || { echo "need Homebrew libmtp and libusb (brew install libmtp)" >&2; exit 1; }
[ -f /usr/local/lib/libfuse3.4.dylib ] || { echo "need macFUSE 5 (https://macfuse.github.io/)" >&2; exit 1; }

echo "==> helper"
make -s clean >/dev/null
make -s BREW="$BREW"

echo "==> app bundle"
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Helpers" "$APP/Contents/Frameworks" "$APP/Contents/Resources/Licenses"
cp app/Info.plist "$APP/Contents/Info.plist"
swiftc -O -swift-version 5 -parse-as-library -target arm64-apple-macos14.0 \
  -framework SwiftUI -framework AppKit app/MountKindle.swift -o "$APP/Contents/MacOS/MountKindle"
cp kindle-mtpfs "$APP/Contents/Helpers/kindle-mtpfs"
cp "$MTPDY" "$USBDY" "$APP/Contents/Frameworks/"
chmod u+w "$APP/Contents/Frameworks/"*.dylib

echo "==> relink bundled libraries with @rpath"
F="$APP/Contents/Frameworks"; H="$APP/Contents/Helpers/kindle-mtpfs"
install_name_tool -id @rpath/libusb-1.0.0.dylib "$F/libusb-1.0.0.dylib"
install_name_tool -id @rpath/libmtp.9.dylib "$F/libmtp.9.dylib"
for dep in $(otool -L "$F/libmtp.9.dylib" | awk 'NR>1{print $1}' | grep 'libusb-1.0'); do
  install_name_tool -change "$dep" @rpath/libusb-1.0.0.dylib "$F/libmtp.9.dylib"; done
install_name_tool -add_rpath @loader_path "$F/libmtp.9.dylib"
for dep in $(otool -L "$H" | awk 'NR>1{print $1}' | grep 'libmtp'); do
  install_name_tool -change "$dep" @rpath/libmtp.9.dylib "$H"; done
install_name_tool -add_rpath @executable_path/../Frameworks "$H"
if otool -L "$H" "$F"/*.dylib | grep -q "$BREW"; then echo "Homebrew paths still linked:" >&2; otool -L "$H" "$F"/*.dylib >&2; exit 1; fi

echo "==> licenses"
cp LICENSE "$APP/Contents/Resources/Licenses/LICENSE-kindle-mtp-mount.txt"
for pair in "libmtp:$MTP" "libusb:$USB"; do
  n="${pair%%:*}"; d="${pair#*:}"; v="$(basename "$(readlink -f "$d")")"
  lic="$(ls "$d"/COPYING* "$d"/LICENSE* 2>/dev/null | head -1 || true)"
  if [ -n "$lic" ]; then cp "$lic" "$APP/Contents/Resources/Licenses/COPYING-$n.txt"
  else cp app/LGPL-2.1.txt "$APP/Contents/Resources/Licenses/COPYING-$n.txt"; fi
  echo "$n $v" >> "$APP/Contents/Resources/Licenses/versions.txt"
done
cp app/THIRD-PARTY-NOTICES.txt "$APP/Contents/Resources/Licenses/"

echo "==> ad-hoc codesign"
codesign --force --sign - "$F/libusb-1.0.0.dylib" "$F/libmtp.9.dylib" "$H"
codesign --force --sign - "$APP"
codesign --verify --strict --deep "$APP"

echo "==> packages"
VER="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' app/Info.plist)"
ZIP="$DIST/Mount-Kindle-$VER.zip"; DMG="$DIST/Mount-Kindle-$VER.dmg"
rm -f "$ZIP" "$DMG"
(cd "$DIST" && ditto -c -k --keepParent "$NAME.app" "$ZIP")
STAGE="$(mktemp -d)"; cp -R "$APP" "$STAGE/"; ln -s /Applications "$STAGE/Applications"
if hdiutil create -quiet -volname "$NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"; then :; else echo "hdiutil failed; skipping .dmg" >&2; fi
rm -rf "$STAGE"
ls -la "$DIST"
