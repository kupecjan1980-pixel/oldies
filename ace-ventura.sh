#!/bin/sh
# Ace Ventura (7th Level, 1996) through Wine. The game is a 16-bit Windows 3.1 program, which today's Wine
# can't run by itself; OTVDM (winevdm) runs it inside Wine, in a 640x480 Wine desktop of its own (the game
# sizes its window to the screen).
#
#   ace-ventura.sh [en | de | folder with ACE.EXE]      (en, de: ~/oldies/games/ace-ventura/en/ACE, .../de/ACE)
#
# Wine: a portable build unpacked in ~/.local/share/oldies/wine (github.com/Kron4ek/Wine-Builds, the
#   amd64-wow64 one), or the system's.
# OTVDM: development build 2712 (ci.appveyor.com/project/otya128/winevdm) unpacked in
#   ~/.local/share/oldies/otvdm-new, with ace-otvdm-patch.py run once on its dll/user.exe16: without that
#   the game crashes a few seconds in ("Interrupt 0D #GP").
# The game: the ACE folder of the CD, as it is (no installing).

data="${XDG_DATA_HOME:-$HOME/.local/share}/oldies"
case "${1:-en}" in
	en|de) game="$HOME/oldies/games/ace-ventura/${1:-en}/ACE" ;;
	*) game="$1" ;;
esac
[ -f "$game/ACE.EXE" ] || { echo "No ACE.EXE in $game: give the game's ACE folder as the argument."; exit 1; }

if [ -x "$data/wine/bin/wine" ]; then
	PATH="$data/wine/bin:$PATH"
fi
command -v wine >/dev/null || { echo "Wine not found (neither installed nor in $data/wine)."; exit 1; }

# (the newest build first: otvdm-new holds a development build, otvdm the 0.9.0 release)
otvdm=$(find "$data/otvdm-new" "$data/otvdm" -name otvdmw.exe 2>/dev/null | head -1)
[ -n "$otvdm" ] || { echo "OTVDM not found below $data/otvdm-new or $data/otvdm."; exit 1; }

export WINEPREFIX="$data/wineprefix"
export WINEDLLOVERRIDES="mscoree,mshtml="   # (no Mono / Gecko download questions)
export WINEDEBUG="${WINEDEBUG:-fixme-all}"
[ -d "$WINEPREFIX" ] || wine wineboot -i

cd "$game" || exit 1
# With gamescope installed the 640x480 desktop is blown up to the whole screen with FSR 1, which smooths the
# edges and sharpens (ACE_SHARPNESS: 0 the sharpest .. 20 the softest; ACE_FILTER=nearest for hard pixels).
# Without gamescope it stays a 640x480 window.
run="wine explorer /desktop=AceVentura,640x480 $(winepath -w "$otvdm") ACE.EXE"
if command -v gamescope >/dev/null; then
	exec gamescope -f -w 640 -h 480 -S fit -F "${ACE_FILTER:-fsr}" --sharpness "${ACE_SHARPNESS:-4}" -- $run
fi
exec $run
