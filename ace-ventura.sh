#!/bin/sh
# Ace Ventura (7th Level, 1996) through Wine. The game is a 16-bit Windows 3.1 program, which today's Wine
# can't run by itself; OTVDM (winevdm) runs it inside Wine. The game must not see a big screen: on a 4K one it
# sizes its window to the screen and crashes, so it gets a 640x480 Wine desktop of its own.
#
#   ace-ventura.sh [folder with ACE.EXE]
#
# Wine: the system's, or a portable build unpacked in ~/.local/share/oldies/wine.
# OTVDM: unpacked in ~/.local/share/oldies/otvdm (the folder holding otvdm.exe, anywhere below it).

data="${XDG_DATA_HOME:-$HOME/.local/share}/oldies"
game="${1:-$HOME/oldies/games/ace-ventura/cd/ACE}"
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
# With gamescope installed the 640x480 desktop is blown up to the whole screen; without it, it stays a
# 640x480 window.
run="wine explorer /desktop=AceVentura,640x480 $(winepath -w "$otvdm") ACE.EXE"
if command -v gamescope >/dev/null; then
	exec gamescope -f -w 640 -h 480 -S fit -F nearest -- $run
fi
exec $run
