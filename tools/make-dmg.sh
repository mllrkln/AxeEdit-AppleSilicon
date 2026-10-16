#!/bin/bash
# Builds Axe-Edit and packs it into a disk image (dist/Axe-Edit-arm64-<version>.dmg) that other people
# install the usual Mac way: open it and drag Axe-Edit onto the Applications shortcut.
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION="1.0.191"
APP="build/AxeEdit_artefacts/Release/Axe-Edit.app"
DMG="dist/Axe-Edit-arm64-$VERSION.dmg"

[ -f build/CMakeCache.txt ] || cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Ad-hoc signature (no Apple Developer account): required for arm64 code; Gatekeeper still asks the
# user to approve the app once, see "Opening it the first time" in README.md.
codesign --force --deep -s - "$APP"
codesign --verify --deep --strict "$APP"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/Axe-Edit.app"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/Read Me First.txt" <<'EOF'
Axe-Edit for Apple Silicon Macs (Axe-Fx Standard / Ultra)

1. Drag "Axe-Edit" onto the "Applications" folder in this window.
2. Eject this disk image (the eject button next to it in the Finder sidebar).
3. Open Axe-Edit from your Applications folder.

The first time, macOS says it can't verify the app. Click "Done", then open
System Settings > Privacy & Security, scroll down, click "Open Anyway" next to
the Axe-Edit message, and confirm with your password. After that it opens normally.

Before uploading anything to your Axe-Fx, use Axe-Manage > Sync & Backup to save
a copy of all your presets.
EOF

mkdir -p dist
rm -f "$DMG"
hdiutil create -volname "Axe-Edit $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
echo "Created $DMG"
