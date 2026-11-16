/*
 * Oldies viewer: subtitles for a game that only speaks. The game's sound (sent by tgspy.so, preloaded into
 * Wine) is compared with the beginnings of the game's own speech clips; a clip heard starting is followed,
 * and its lines (from a subtitle pack made beforehand by tools/tgsubs.py) are given out as it goes.
 */

#ifndef SUBS_H__
#define SUBS_H__

#include <string>

enum { kSubsLangs = 3 }; // the pack's text columns: English, German, Czech

// gameDir: the folder with the game's data files; pack: the subtitle pack; sock: the datagram socket to listen
// on (tgspy.so's TGSPY_SOCK). False when the pack or the socket can't be had.
bool Subs_start(const char *gameDir, const char *pack, const char *sock);
void Subs_stop();
// the line(s) being spoken now in the language (0..kSubsLangs-1), "" when none
std::string Subs_current(int lang);
// for testing: a log written by tgspy.so (TGSPY_LOG) instead of the socket, as fast as it goes; prints what's
// heard when
void Subs_feedFile(const char *path);

#endif
