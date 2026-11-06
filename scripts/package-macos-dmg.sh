#!/bin/sh
# Assemble PIET.app from the executables scripts/build-macos.sh produced and
# wrap it in a .dmg. Self-contained: everything it copies is in this tree.
#
# Usage (from the repository root, after scripts/build-macos.sh):
#   sh scripts/package-macos-dmg.sh [LABEL]
# LABEL (optional) is appended to the image name, e.g. 20261106.
#
# Output: dist/PIET-<version>[-LABEL]-macos-arm64.dmg and its .sha256.
#
# Signing: with PIET_CODESIGN_IDENTITY set to a Developer ID, the bundle is
# signed with the hardened runtime; otherwise it is ad-hoc signed (-s -),
# which is what the project's own downloads are today. An ad-hoc signed app
# is not notarized, so macOS Gatekeeper asks before the first launch; the
# README's "Download for macOS" section says what to click.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd -P)
label="${1:-}"

test "$(uname -s)" = Darwin || { echo "This packager runs on macOS." >&2; exit 1; }
arch=$(uname -m)
for file in build/cobra piet/native/build/piet_native; do
  test -x "$file" || { echo "Missing $file; run sh scripts/build-macos.sh first." >&2; exit 1; }
done
for file in LICENSE.txt NOTICE.txt piet/packaging/sample/fatalities.csv \
            piet/packaging/native_bundle/Info.plist.in piet/packaging/native_bundle/CoBrA.icns; do
  test -s "$file" || { echo "Missing distribution file: $file" >&2; exit 1; }
done

# Versions come from the binaries themselves, never typed by hand.
piet_version=$(./piet/native/build/piet_native --version | python3 -c 'import json,sys; print(json.load(sys.stdin)["piet_version"])')
contract_a=$(./piet/native/build/piet_native --version | python3 -c 'import json,sys; print(json.load(sys.stdin)["contract_a"])')
contract_b=$(./piet/native/build/piet_native --version | python3 -c 'import json,sys; print(json.load(sys.stdin)["contract_b"])')
cobra_version=$(./build/cobra version --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["cobra_version"])')
commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)

tmp=$(mktemp -d "${TMPDIR:-/tmp}/piet-dmg.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
app="$tmp/image/PIET.app"
contents="$app/Contents"
resources="$contents/Resources"
mkdir -p "$contents/MacOS" "$resources/sample" dist

cp piet/native/build/piet_native "$contents/MacOS/piet_native"
cp build/cobra "$resources/cobra"
cp -R piet/frontend "$resources/frontend"
cp piet/packaging/sample/fatalities.csv "$resources/sample/"
cp piet/packaging/native_bundle/CoBrA.icns "$resources/CoBrA.icns"
cp LICENSE.txt NOTICE.txt "$resources/"

# THIRD_PARTY_NOTICES.txt: the same concatenation the About panel reads, one
# part per file in piet/packaging/licenses/NN-*.txt, lexical order.
{
  echo "PIET $piet_version — THIRD-PARTY NOTICES"
  echo "(generated at package time from piet/packaging/licenses/ — do not edit in place)"
  for part in piet/packaging/licenses/[0-9][0-9]-*.txt; do
    echo; echo "────────────────────────────────────────────────────────────────────"; echo
    cat "$part"
  done
} > "$resources/THIRD_PARTY_NOTICES.txt"
grep -q "StataCorp LLC" "$resources/THIRD_PARTY_NOTICES.txt" || { echo "Notices lack the non-affiliation statement." >&2; exit 1; }
grep -q "Niels Lohmann" "$resources/THIRD_PARTY_NOTICES.txt" || { echo "Notices lack a bundled component (nlohmann/json)." >&2; exit 1; }

sed \
  -e "s/@PIET_BUNDLE_VERSION@/$piet_version/g" \
  -e "s/@PIET_BUNDLE_COBRA_VERSION@/$cobra_version/g" \
  -e "s/@PIET_BUNDLE_CONTRACT_A@/$contract_a/g" \
  -e "s/@PIET_BUNDLE_CONTRACT_B@/$contract_b/g" \
  -e "s|<string>11.0</string>|<string>13.3</string>|" \
  piet/packaging/native_bundle/Info.plist.in > "$contents/Info.plist"
plutil -lint "$contents/Info.plist" >/dev/null

identity="${PIET_CODESIGN_IDENTITY:--}"
if [ "$identity" = "-" ]; then
  echo "[package] ad-hoc signing (no Developer ID identity set)"
  codesign --force --deep --sign - "$app"
else
  echo "[package] signing with $identity (hardened runtime)"
  codesign --force --deep --options runtime \
    --entitlements piet/packaging/native_bundle/piet_native.entitlements \
    --sign "$identity" "$app"
fi
codesign --verify --deep --strict "$app"

# The packaged app must still answer for itself.
"$contents/MacOS/piet_native" --version | grep -q "\"piet_version\":\"$piet_version\""

ln -s /Applications "$tmp/image/Applications"
name="PIET-${piet_version}${label:+-$label}-macos-${arch}.dmg"
hdiutil create -quiet -volname "PIET $piet_version" -srcfolder "$tmp/image" -format UDZO -ov "dist/$name"
(cd dist && shasum -a 256 "$name" > "$name.sha256")
echo "Built dist/$name (PIET $piet_version, CoBrA $cobra_version, source commit $commit, $arch)"
cat "dist/$name.sha256"
if [ "$identity" = "-" ]; then
  echo "Note: ad-hoc signed, not notarized. First launch: right-click PIET.app > Open, or allow it under"
  echo "      System Settings > Privacy & Security after the first blocked attempt."
fi
