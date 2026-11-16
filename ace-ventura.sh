#!/bin/sh
# Ace Ventura (7th Level, 1996) through Wine. The game is a 16-bit Windows 3.1 program, which today's Wine
# can't run by itself; OTVDM (winevdm) runs it inside Wine.
#
#   ace-ventura.sh [en | de | folder with ACE.EXE]      (en, de: ~/oldies/games/ace-ventura/en/ACE, .../de/ACE)
#
# How it's shown, the best that's there:
#   1. oldies-view (beside this script; `make`) and Xwayland: the game runs in a small hidden X server of its
#      own and the viewer shows its picture with the 256-colour dithering taken out, upscaled by FSR 1 or a
#      neural net to the screen. F7 next upscaling, F8 dithering removal on / off, F11 full screen; a
#      controller's left stick is the pointer, A / B the mouse buttons.
#      With a subtitle pack for the game's version in ~/.local/share/oldies/ace-subs (made by
#      tools/tgsubs.py from the game's own speech), subtitles: F9 none / English / German / Czech.
#      F10 marks the things that can be used, with their names.
#   2. gamescope: full screen with FSR 1 (ACE_SHARPNESS 0 the sharpest .. 20; ACE_FILTER=nearest hard pixels).
#   3. neither: a 640x480 window.
#   ACE_SHOW=view | gamescope | window picks one.
#
# Wine: a portable build unpacked in ~/.local/share/oldies/wine (github.com/Kron4ek/Wine-Builds, the
#   amd64-wow64 one), or the system's.
# OTVDM: development build 2712 (ci.appveyor.com/project/otya128/winevdm) unpacked in
#   ~/.local/share/oldies/otvdm-new, with ace-otvdm-patch.py run once on its dll/user.exe16: without that
#   the game crashes a few seconds in ("Interrupt 0D #GP").
# The game: the ACE folder of the CD, as it is (no installing).

here=$(cd "$(dirname "$0")" && pwd)
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
# The later version of the game (the German one, with ACESAVEn.SV7 slots) keeps the running game in
# ACEGAME.SAV and quits at the start when one is left over that it can't delete: taken out of its way.
if ls | grep -qi '^ACESAVE[0-9]*\.SV7$'; then
	for f in [Aa][Cc][Ee][Gg][Aa][Mm][Ee].[Ss][Aa][Vv]; do
		[ -f "$f" ] && mv -f "$f" "$f.old"
	done
fi

otvdm_win=$(winepath -w "$otvdm")
show="${ACE_SHOW:-}"
if [ -z "$show" ]; then
	if [ -x "$here/oldies-view" ] && command -v Xwayland >/dev/null && [ -n "$WAYLAND_DISPLAY" ]; then show=view
	elif command -v gamescope >/dev/null; then show=gamescope
	else show=window; fi
fi

case "$show" in
view)
	n=77
	while [ -e "/tmp/.X11-unix/X$n" ]; do n=$((n + 1)); done
	# (Hyprland: the X server's own window goes to a workspace out of sight; it must keep its size)
	if command -v hyprctl >/dev/null; then
		hyprctl eval 'hl.window_rule({match={class="org.freedesktop.Xwayland"}, workspace="special:oldies silent", float=true, no_initial_focus=true, render_unfocused=true})' >/dev/null 2>&1
	fi
	Xwayland ":$n" -geometry 640x480 -noreset >/dev/null 2>&1 &
	xpid=$!
	i=0
	while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	# subtitles: the pack made for these very data files (tools/tgsubs.py; its first line names INTRO.BIN's size).
	# tgspy.so, inside Wine, tells the viewer the game's sound and which data it reads: by them the viewer
	# knows the line being spoken. (The viewer first: it must be listening when the game starts reading.)
	subs=""
	spy="${XDG_RUNTIME_DIR:-/tmp}/oldies-spy-$n"
	size=$(stat -c %s INTRO.BIN 2>/dev/null || stat -c %s intro.bin 2>/dev/null)
	if [ -f "$here/tgspy.so" ]; then
		for pack in "$data"/ace-subs/*.tsv; do
			[ -f "$pack" ] && [ "$(head -1 "$pack")" = "#game INTRO.BIN $size" ] && subs="$pack"
		done
	fi
	"$here/oldies-view" ":$n" "Ace Ventura" ${subs:+--subs "$subs" --game "$game" --spy "$spy"} &
	vpid=$!
	( sleep 15; env -u WAYLAND_DISPLAY DISPLAY=":$n" wineserver -w; kill $vpid 2>/dev/null ) &   # (the game was quit: so is the viewer)
	sleep 0.5
	# (inside a 640x480 Wine desktop: without one the game's window stays black in a server with no window manager)
	if [ -n "$subs" ]; then
		env -u WAYLAND_DISPLAY DISPLAY=":$n" TGSPY_DIR="$game" TGSPY_SOCK="$spy" LD_PRELOAD="$here/tgspy.so${LD_PRELOAD:+:$LD_PRELOAD}" \
			wine explorer "/desktop=AceVentura,640x480" "$otvdm_win" ACE.EXE &
	else
		env -u WAYLAND_DISPLAY DISPLAY=":$n" wine explorer "/desktop=AceVentura,640x480" "$otvdm_win" ACE.EXE &
	fi
	wait $vpid
	wineserver -k
	kill $xpid 2>/dev/null
	;;
gamescope)
	exec gamescope -f -w 640 -h 480 -S fit -F "${ACE_FILTER:-fsr}" --sharpness "${ACE_SHARPNESS:-4}" -- \
		wine explorer "/desktop=AceVentura,640x480" "$otvdm_win" ACE.EXE
	;;
*)
	exec wine explorer "/desktop=AceVentura,640x480" "$otvdm_win" ACE.EXE
	;;
esac
