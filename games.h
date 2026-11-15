/*
 * Oldies: what's known of each game.
 *
 *   marks, exe   the files a game's folder is recognised by, and the one it's started with
 *   options      the emulator core's settings for it
 *   keys         a controller's buttons as the keys the game's own controls have for them, with what each
 *                does (shown in the pause menu's Controls) in English, Czech and Slovak
 *   hints        what to do, in the three languages
 */

#ifndef GAMES_H__
#define GAMES_H__

#include <SDL.h>
#include "third_party/libretro.h"

enum { kMaxPadKeys = 16, kMaxHints = 12 };

struct PadKey {
	int button;            // SDL_CONTROLLER_BUTTON_*
	unsigned key;          // RETROK_*
	const char *what[3];
	const char *pad;       // the button's name on the screen
	const char *keyName;   // the key's
};

struct Hint {
	const char *title[3];
	const char *text[3];
};

struct Game {
	const char *id, *name, *year, *core, *exe;
	const char *marks[4];
	const char *options[10][2];
	unsigned diag[4];      // the keys for up-right, down-right, down-left, up-left, where the game has its own (0: none)
	const char *gameLang[3];   // the game's own languages, where its texts are a file to swap ("": none), and
	const char *gameLangFile[4]; //   their files, the last the one the game reads: copied over it before the start
	PadKey keys[kMaxPadKeys];
	Hint hints[kMaxHints];
};

static const Game kGames_[] = {
	{ "body-blows", "Body Blows", "Team17, 1993", "dosbox_pure", "BB.EXE",
	  { "BODYBLOW.GFX", 0 },
	  { { "dosbox_pure_cycles", "auto" }, { "dosbox_pure_aspect_correction", "false" }, { "dosbox_pure_menu_time", "0" },
	    { "dosbox_pure_auto_mapping", "false" }, { "dosbox_pure_on_screen_keyboard", "false" }, { "", "" } },
	  { RETROK_PAGEUP, RETROK_PAGEDOWN, RETROK_END, RETROK_HOME },
	  { "", "", "" }, { "", "", "", "" },
	  { // (the first player's keys as the game's own BB.CFG has them: the cursor keys, the four corner keys of
	    // the keypad for the diagonals, Enter the fire button)
	    { SDL_CONTROLLER_BUTTON_DPAD_UP, RETROK_UP, { "Jump", "Skok", "Skok" }, "D-pad ↑", "↑" },
	    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETROK_DOWN, { "Crouch", "Dřep", "Drep" }, "D-pad ↓", "↓" },
	    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETROK_LEFT, { "Left", "Doleva", "Doľava" }, "D-pad ←", "←" },
	    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETROK_RIGHT, { "Right", "Doprava", "Doprava" }, "D-pad →", "→" },
	    { SDL_CONTROLLER_BUTTON_A, RETROK_RETURN, { "Hit (with a direction: the moves); confirm in the menus", "Úder (se směrem: chvaty); potvrzení v nabídkách", "Úder (so smerom: chvaty); potvrdenie v ponukách" }, "A", "Enter" },
	    { SDL_CONTROLLER_BUTTON_BACK, RETROK_ESCAPE, { "Back / leave the fight", "Zpět / opustit zápas", "Späť / opustiť zápas" }, "View / Back", "Esc" },
	    { 0, 0, { "", "", "" }, "", "" } },
	  { { { "The moves", "Chvaty", "Chvaty" },
	      { "Every hit is the fire button with a direction: up, down, towards the other fighter, away from him and the four in between give different punches, kicks and throws. Away from the other fighter without fire blocks.",
	        "Každý úder je tlačítko úderu se směrem: nahoru, dolů, k soupeři, od soupeře a čtyři směry mezi nimi dávají různé údery, kopy a hody. Směr od soupeře bez tlačítka kryje.",
	        "Každý úder je tlačidlo úderu so smerom: hore, dole, k súperovi, od súpera a štyri smery medzi nimi dávajú rôzne údery, kopy a hody. Smer od súpera bez tlačidla kryje." } },
	    { { "The special move", "Zvláštní chvat", "Zvláštny chvat" },
	      { "Hold the fire button without a direction: the power bar fills up; let go when it's full and the fighter does his special move.",
	        "Drž tlačítko úderu bez směru: ukazatel síly se plní; když je plný, pusť ho a bojovník předvede svůj zvláštní chvat.",
	        "Drž tlačidlo úderu bez smeru: ukazovateľ sily sa plní; keď je plný, pusti ho a bojovník predvedie svoj zvláštny chvat." } },
	    { { "", "", "" }, { "", "", "" } } } },
	{ "metal-mutant", "Metal Mutant", "Silmarils, 1991", "dosbox_pure", "METAL.EXE",
	  { "MUTANT.IO", 0 },
	  { { "dosbox_pure_cycles", "auto" }, { "dosbox_pure_aspect_correction", "false" }, { "dosbox_pure_menu_time", "0" },
	    { "dosbox_pure_auto_mapping", "false" }, { "dosbox_pure_on_screen_keyboard", "false" }, { "", "" } },
	  { RETROK_KP9, RETROK_KP3, RETROK_KP1, RETROK_KP7 },
	  { "English", "Español", "Français" }, { "TEXTE.EN", "TEXTE.ES", "TEXTE.FR", "TEXTE.IO" },
	  { // (the manual: the numeric keypad for the directions, Shift the fire button, F1-F3 the three forms, Esc pause)
	    { SDL_CONTROLLER_BUTTON_DPAD_UP, RETROK_KP8, { "Up (jump)", "Nahoru (skok)", "Hore (skok)" }, "D-pad ↑", "↑ / Num 8" },
	    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETROK_KP2, { "Down", "Dolů", "Dole" }, "D-pad ↓", "↓ / Num 2" },
	    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETROK_KP4, { "Left", "Doleva", "Doľava" }, "D-pad ←", "← / Num 4" },
	    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETROK_KP6, { "Right", "Doprava", "Doprava" }, "D-pad →", "→ / Num 6" },
	    { SDL_CONTROLLER_BUTTON_A, RETROK_LSHIFT, { "Fire (with a direction: the weapons)", "Střelba (se směrem: zbraně)", "Streľba (so smerom: zbrane)" }, "A", "Shift" },
	    { SDL_CONTROLLER_BUTTON_X, RETROK_F1, { "Become the Cyborg", "Proměna v Kyborga", "Premena na Kyborga" }, "X", "F1" },
	    { SDL_CONTROLLER_BUTTON_Y, RETROK_F2, { "Become the Dino", "Proměna v Dina", "Premena na Dina" }, "Y", "F2" },
	    { SDL_CONTROLLER_BUTTON_B, RETROK_F3, { "Become the Tank", "Proměna v Tank", "Premena na Tank" }, "B", "F3" },
	    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RETROK_RETURN, { "Confirm", "Potvrdit", "Potvrdiť" }, "LB", "Enter" },
	    { SDL_CONTROLLER_BUTTON_BACK, RETROK_ESCAPE, { "The game's own pause", "Pauza ve hře", "Pauza v hre" }, "View / Back", "Esc" },
	    { 0, 0, { "", "", "" }, "", "" } },
	  { { { "The Cyborg", "Kyborg", "Kyborg" },
	      { "Fire with a direction: up jumps, towards the enemy swings the battle axe, up and forwards is the power strike, down and forwards the trident, up and back throws the rope, down and back saves the game at an ALOG module, down turns into the Dino. A weapon works only once its KYO cartridge has been found.",
	        "Střelba se směrem: nahoru skok, dopředu sekera, nahoru dopředu silový úder, dolů dopředu trojzubec, nahoru dozadu lano, dolů dozadu uložení hry u modulu ALOG, dolů proměna v Dina. Zbraň funguje, až když najdeš její kazetu KYO.",
	        "Streľba so smerom: hore skok, dopredu sekera, hore dopredu silový úder, dole dopredu trojzubec, hore dozadu lano, dole dozadu uloženie hry pri module ALOG, dole premena na Dina. Zbraň funguje, až keď nájdeš jej kazetu KYO." } },
	    { { "The Dino", "Dino", "Dino" },
	      { "Fire with a direction: up breathes flame, forwards bites, up and forwards is the hypnotic ray, down and forwards the shield, up and back the Cyberstyx, down and back the tail flame, down turns into the Tank.",
	        "Střelba se směrem: nahoru plamen, dopředu kousnutí, nahoru dopředu hypnotický paprsek, dolů dopředu štít, nahoru dozadu Cyberstyx, dolů dozadu plamen z ocasu, dolů proměna v Tank.",
	        "Streľba so smerom: hore plameň, dopredu uhryznutie, hore dopredu hypnotický lúč, dole dopredu štít, hore dozadu Cyberstyx, dole dozadu plameň z chvosta, dole premena na Tank." } },
	    { { "The Tank", "Tank", "Tank" },
	      { "Fire with a direction: up shoots upwards, forwards shoots ahead, the two upper diagonals shoot diagonally, down and forwards launches a torpedo, down and back turns the radar on, down turns into the Cyborg.",
	        "Střelba se směrem: nahoru střela vzhůru, dopředu střela vpřed, obě horní úhlopříčky střílejí šikmo, dolů dopředu torpédo, dolů dozadu radar, dolů proměna v Kyborga.",
	        "Streľba so smerom: hore strela nahor, dopredu strela vpred, obe horné uhlopriečky strieľajú šikmo, dole dopredu torpédo, dole dozadu radar, dole premena na Kyborga." } },
	    { { "Every screen", "Každá obrazovka", "Každá obrazovka" },
	      { "A screen has to be cleared of its enemies before the way on opens. The computer's panel under the picture tells which weapon the place needs. The game saves only at ALOG survival modules, as the Cyborg; the pause menu's own saving works anywhere.",
	        "Obrazovku je třeba vyčistit od nepřátel, než se otevře cesta dál. Panel počítače pod obrazem napovídá, jakou zbraň místo potřebuje. Hra sama ukládá jen u modulů přežití ALOG, v podobě Kyborga; ukládání v nabídce pauzy funguje kdekoli.",
	        "Obrazovku treba vyčistiť od nepriateľov, kým sa otvorí cesta ďalej. Panel počítača pod obrazom napovedá, akú zbraň miesto potrebuje. Hra sama ukladá len pri moduloch prežitia ALOG, v podobe Kyborga; ukladanie v ponuke pauzy funguje kdekoľvek." } },
	    { { "", "", "" }, { "", "", "" } } } },
};
static const int kGameCount = sizeof(kGames_) / sizeof(kGames_[0]);

#endif
