#!/bin/bash
# Package a finished scripts/build-windows.sh build into what a Windows user
# downloads, under dist/windows-amd64-piet/:
#
#   PIET-<version>-windows-x64-portable.zip   unzip anywhere and run PIET.exe
#   PIET-<version>-setup.exe                  the installer (needs Inno Setup 6.3+)
#
# Run it from an MSYS2 MINGW64 shell, after build-windows.sh, with rsync and zip
# installed:  pacman -S --needed rsync zip
#
# Environment:
#   ISCC                 path to Inno Setup's ISCC.exe (default: look on PATH, then
#                        in the standard install folder). Not found: the zip is
#                        still produced, and the script says the installer was not.
#   WEBVIEW2_BOOTSTRAPPER  a local MicrosoftEdgeWebview2Setup.exe to embed. Without
#                        it, Microsoft's Evergreen bootstrapper is downloaded.
set -euo pipefail
cd "$(dirname "$0")/.."

case "${MSYSTEM:-}" in
  MINGW64) ;;
  *) echo "FATAL: run this from an MSYS2 MINGW64 shell (MSYSTEM is '${MSYSTEM:-unset}')" >&2; exit 1 ;;
esac

app=piet/native/build/piet_native.exe
engine=build/cobra.exe
for exe in "$app" "$engine"; do
  [[ -f "$exe" ]] || { echo "FATAL: $exe not found; run scripts/build-windows.sh first" >&2; exit 1; }
done

# A binary that needs a DLL from the build machine would start here and fail on
# every other PC, so anything outside Windows itself stops the package.
allowed='^(ADVAPI32|dwmapi|GDI32|KERNEL32|msvcrt|ole32|OLEAUT32|SHELL32|SHLWAPI|USER32|VERSION)\.dll$'
for exe in "$app" "$engine"; do
  foreign=$(objdump -p "$exe" | sed -n 's/.*DLL Name: //p' | tr -d '\r' | grep -Eiv "$allowed" || true)
  if [[ -n "$foreign" ]]; then
    echo "FATAL: $exe depends on DLLs a clean Windows does not have: $foreign" >&2
    exit 1
  fi
done

version=$("$app" --version | sed -n 's/.*"piet_version":"\([^"]*\)".*/\1/p' | tr -d '\r')
[[ -n "$version" ]] || { echo "FATAL: could not read the version from $app --version" >&2; exit 1; }
echo "[package] PIET $version"

out=dist/windows-amd64-piet
staged="$out/PIET-windows"
mkdir -p "$out/build" dist/windows-amd64
strip -o "$out/build/piet_native.exe" "$app"
strip -o dist/windows-amd64/cobra.exe "$engine"

PIET_NATIVE_EXE="$PWD/$out/build/piet_native.exe" COBRA_EXE="$PWD/dist/windows-amd64/cobra.exe" \
  bash piet/packaging/stage_native_windows.sh "$PWD/$staged"

zip_name="PIET-$version-windows-x64-portable.zip"
rm -f "$out/$zip_name"
(cd "$staged" && zip -qr "../$zip_name" .)
echo "[package] built $out/$zip_name"

iscc="${ISCC:-}"
if [[ -z "$iscc" ]]; then
  iscc=$(command -v ISCC.exe || true)
fi
if [[ -z "$iscc" && -n "${PROGRAMFILES:-}" ]]; then
  for candidate in "${PROGRAMFILES}/Inno Setup 7/ISCC.exe" "${PROGRAMFILES} (x86)/Inno Setup 6/ISCC.exe"; do
    if [[ -f "$candidate" ]]; then iscc=$candidate; break; fi
  done
fi
if [[ -z "$iscc" ]]; then
  echo "[package] Inno Setup not found (set ISCC): the installer was NOT built" >&2
  exit 0
fi

# The installer embeds Microsoft's bootstrapper, so one that fails to arrive must
# stop the build: piet_setup.iss would otherwise compile a setup that can leave a
# machine without the runtime the app cannot start without.
bootstrapper=piet/packaging/MicrosoftEdgeWebview2Setup.exe
if [[ -n "${WEBVIEW2_BOOTSTRAPPER:-}" ]]; then
  cp "$WEBVIEW2_BOOTSTRAPPER" "$bootstrapper"
elif [[ ! -f "$bootstrapper" ]]; then
  powershell.exe -NoProfile -Command "\$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile '$(cygpath -w "$PWD/$bootstrapper")'"
fi
[[ -s "$bootstrapper" ]] || { echo "FATAL: no WebView2 bootstrapper at $bootstrapper" >&2; exit 1; }
# This file runs elevated on users' machines, so only Microsoft's signature will do.
signer=$(powershell.exe -NoProfile -Command "\$s = Get-AuthenticodeSignature '$(cygpath -w "$PWD/$bootstrapper")'; if (\$s.Status -eq 'Valid') { \$s.SignerCertificate.Subject }" | tr -d '\r')
[[ "$signer" == *"O=Microsoft Corporation"* ]] \
  || { echo "FATAL: $bootstrapper is not validly signed by Microsoft (signer: '$signer')" >&2; exit 1; }

# MSYS2 would otherwise rewrite the /D and /O switches as if they were POSIX paths.
MSYS2_ARG_CONV_EXCL='*' "$iscc" "/DDistDir=$(cygpath -w "$PWD/$staged")" "/O$(cygpath -w "$PWD/$out")" \
  "$(cygpath -w "$PWD/piet/packaging/piet_setup.iss")"
echo "[package] built $out/PIET-$version-setup.exe"
