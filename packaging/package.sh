#!/usr/bin/env bash
# Builds the player release: release/driveclub-recomp-<version>-win64/ and a zip next to it.
# Needs the MSYS2 CLANG64 toolchain used for the build (no other downloads).
# usage: packaging/package.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/clang64/bin:$PATH"
VERSION="${1:-dev}"
NAME="driveclub-recomp-$VERSION-win64"
OUT="release/$NAME"

bash build.sh
mkdir -p out/launcher
windres -I launcher launcher/launcher.rc -O coff -o out/launcher/launcher.res.o
clang++ -O2 -std=c++20 -municode -mwindows -static -o out/launcher/DriveClub.exe   launcher/driveclub_launcher.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp out/launcher/launcher.res.o   -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi

rm -rf "$OUT"
mkdir -p "$OUT/runtime/patches"
cp out/launcher/DriveClub.exe "$OUT/"
cp out/DriveClubGame.exe "$OUT/runtime/"
strip --strip-all "$OUT/runtime/DriveClubGame.exe" || true
cp patches/DriveClub.xml "$OUT/runtime/patches/"
cp packaging/release/README.txt packaging/release/THIRD_PARTY_NOTICES.txt LICENSE "$OUT/"
# Every non-system DLL the game program loads, so nothing has to be installed.
ldd out/DriveClubGame.exe | awk '{print $3}' | grep -i '/msys64/' | sort -u | while read -r dll; do
  cp "$dll" "$OUT/runtime/"
done
# DLLs the game program loads by name at run time.
for extra in SDL3.dll; do
  [ -f "out/$extra" ] && cp "out/$extra" "$OUT/runtime/" || true
done
echo "files: $(find "$OUT" -type f | wc -l), size: $(du -sh "$OUT" | cut -f1)"
python - "$OUT" "release/$NAME.zip" <<'PYEOF'
import os, sys, zipfile
src, dest = sys.argv[1], sys.argv[2]
top = os.path.basename(src.rstrip("/"))
if os.path.exists(dest):
    os.remove(dest)
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for base, _, files in os.walk(src):
        for name in sorted(files):
            path = os.path.join(base, name)
            z.write(path, top + "/" + os.path.relpath(path, src).replace(os.sep, "/"))
PYEOF
echo "wrote release/$NAME.zip"
