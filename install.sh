#!/usr/bin/env bash
# Oldies: fetches the DOS emulator core if it isn't there yet, and adds Oldies to the applications menu from
# where this folder is (move the folder and run this again). ./install.sh --remove takes the entry away.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor/scalable/apps"
if [ "${1:-}" = "--remove" ]; then
    rm -f "$APPS/oldies.desktop" "$ICONS/oldies.svg"
    echo "Oldies' menu entry removed"
    exit 0
fi
CORE="$HERE/cores/dosbox_pure_libretro.so"
if [ ! -f "$CORE" ]; then
    # DOSBox Pure (GPL-2, https://codeberg.org/schelling/dosbox-pure), as built by the libretro project
    URL="https://buildbot.libretro.com/nightly/linux/x86_64/latest/dosbox_pure_libretro.so.zip"
    echo "Fetching the DOS emulator core from $URL"
    mkdir -p "$HERE/cores"
    TMP="$(mktemp -d)"
    curl -fsSL -o "$TMP/core.zip" "$URL"
    if command -v unzip >/dev/null; then unzip -q -o "$TMP/core.zip" -d "$TMP"
    elif command -v bsdtar >/dev/null; then bsdtar -xf "$TMP/core.zip" -C "$TMP"
    else python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$TMP/core.zip" "$TMP"; fi
    mv "$TMP/dosbox_pure_libretro.so" "$CORE"
    rm -rf "$TMP"
fi
mkdir -p "$APPS" "$ICONS"
cp "$HERE/assets/icon.svg" "$ICONS/oldies.svg"
cat > "$APPS/oldies.desktop" <<D
[Desktop Entry]
Type=Application
Name=Oldies
Comment=Body Blows and Metal Mutant on Linux (your own game files are needed)
Exec="$HERE/oldies"
Path=$HERE
Icon=oldies
Categories=Game;
StartupWMClass=oldies
D
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
echo "Oldies is in the applications menu; the emulator core is $CORE"
