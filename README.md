# Oldies: Body Blows and Metal Mutant on Linux (desktops and handhelds)

A shell around a DOS emulator that gives two old DOS games what a handheld or a living-room PC needs.
The games run in [DOSBox Pure](https://codeberg.org/schelling/dosbox-pure); Oldies puts this around them:

- **Controllers** (any that SDL knows; plugged in or out while playing) mapped to each game's own keys,
  diagonals included, and the keyboard.
- **A pause menu** (Start / F6) in **English, Czech or Slovak**: save and load anywhere (99 slots), the
  controls with what each button does in the game, hints, the picture's upscaling, the folder with the
  game's files.
- **Upscaling to the screen's resolution**, switched while playing (RB / F7): the original 320×200, hard
  pixels, smooth, sharp, **AMD FSR 1** (on any maker's card) and three sizes of a neural net (Anime4K), to
  Full HD or to the screen's own resolution (up to 4K). Always shown 4:3, as on the monitors of the time.
- **Frames in between** (RT / F8): the games draw 10 to 35 pictures a second; with this on, the screen gets
  a new picture at every refresh (60 to 165 a second), made from the game's last two.
- **The game's language** for Metal Mutant (English, Spanish, French) where your copy has the three text
  files (`TEXTE.EN`, `.ES`, `.FR`).

**The games are not included.** You need your own copies; the program asks for each game's folder the first
time it's started.

| Game | Folder it wants |
|---|---|
| Body Blows (Team17, 1993), DOS | the one with `BB.EXE` and `BODYBLOW.GFX` |
| Metal Mutant (Silmarils, 1991), DOS | the one with `METAL.EXE` and `MUTANT.IO` |

## Install

Download `oldies-linux-X.Y.Z.tar.gz` from the
[newest release](https://github.com/kupecjan1980-pixel/oldies/releases/latest), unpack it anywhere, and in
the unpacked folder run

    ./install.sh      # fetches the DOS emulator core (1.5 MB, from the libretro project) and adds a menu entry
    ./oldies          # or: ./oldies body-blows, ./oldies metal-mutant

It needs SDL2 and OpenGL ES 3 (Mesa or Nvidia's driver), which desktops, Bazzite, CachyOS and SteamOS have;
glibc 2.38 or newer. On a handheld add `oldies` to Steam as a non-Steam game.

A core already on the computer is found too: `dosbox_pure_libretro.so` in `cores/` beside the program, in
`~/.local/share/oldies/cores`, or where RetroArch keeps its cores.

## Controls

The shell's own, the same in every game:

| | Controller | Keyboard |
|---|---|---|
| Pause menu | Menu / Start | F6 |
| **Picture: next upscaling** | **RB** | **F7** (Shift+F7 back) |
| **Frames in between on / off** | **RT** | **F8** |
| Quick save / load | L3 / R3 | F5 / F9 |
| Full screen | | F11 |
| Save the picture as PNG | | F12 |

**Body Blows** (the game's first-player keys): D-pad / left stick / arrows move, jump and crouch; A / Enter
hits (with a direction: the moves) and confirms in the menus; View / Esc goes back.

**Metal Mutant**: D-pad / left stick / arrows (the game's keypad 8 2 4 6, and 7 9 1 3 for the diagonals);
A / Shift fires (with a direction: the weapons); X, Y, B / F1, F2, F3 become the Cyborg, the Dino, the Tank;
LB / Enter confirms; View / Esc is the game's own pause. At the first screen pick the graphics with F1 (CGA)
or F2 (VGA) on a keyboard, or Y for VGA on a controller.

The pause menu's Controls shows the same for the game being played.

## What it can't do

- **Hints don't follow where you are.** An emulator doesn't tell the shell what's happening in the game, so
  the hints are pages to leaf through, not picked by progress.
- **No Czech or Slovak game texts.** Metal Mutant's text files are packed in the game's own format; Body
  Blows has next to no text. The menus around the games are in the three languages.
- **FSR 2–4, XeSS, the NPU**: built for 3D games (they need depth and motion from the game's engine); a 2D
  picture from an emulator has neither. FSR 1 and the neural nets work on finished pictures and are in.
- **Ace Ventura** (7th Level, 1996) is a 16-bit Windows 3.1 game, not a DOS one. `ace-ventura.sh` in the
  source tries it through Wine and OTVDM; there it loads and then crashes, so it's not part of the release.

## Building

    make            # needs g++, SDL2's development files, OpenGL ES 3 headers (Mesa)
    make dist       # the folder and .tar.gz to hand out

Settings are kept in `~/.config/oldies/config.ini`, saved states in `~/.local/share/oldies/`.

Adding a game is an entry in `games.h`: the files its folder is recognised by, the program to start, the
controller's buttons as the game's keys, the hints.

For testing without a screen: `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy OLDIES_WINDOW=1920x1080
OLDIES_SCRIPT=file ./oldies metal-mutant FOLDER --window`, the file holding lines `milliseconds key NAME`,
`… shot FILE.png`, `… quit`.

## Whose work this is

- **The games, their pictures, sounds and texts** belong to their makers (Team17; Silmarils). Not included
  and not to be added to this repository.
- **DOSBox Pure**: © Bernhard Schelling and the DOSBox Team, GPL-2. Not in this repository; fetched by
  `install.sh` as built by the libretro project.
- The shell (`shell.cpp`, `games.h`, `gfx`, `ui`) may be used freely. `libretro.h`: MIT.
- Anime4K upscaling shaders: © bloc97, MIT; FSR 1: © Advanced Micro Devices, MIT, as ported to mpv by agyild
  (`shaders/`). stb_truetype, stb_image_write: Sean Barrett, public domain (`third_party/`).
- Noto Sans: SIL Open Font License (`assets/`).
