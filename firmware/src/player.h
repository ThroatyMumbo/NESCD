// player.h - the audio CD player: a ROM the user starts on the cart drives the
// transport through the mailbox page, and the host streams a Red Book disc to
// the DAC. Byte roles in sprite tile $FF, $1FF0..$1FFF:
//
//   $1FF0..3  ROM   "NCDP", rewritten every frame; the host arms on it
//   $1FF4..5  host  current track length, min / sec
//   $1FF8     ROM   request (seq << 4) | cmd, seq 1..7 rolling; 0 = none
//   $1FF9     ROM   PLAY's track, 0 = the current one
//   $1FFA     host  the request echoed once acted on, | 0x80 refused
//   $1FFB     host  state, $1FFC track, $1FFD track count, $1FFE/F min / sec
//
// The host acts on a change of $1FF8, never on a level, so the ROM neither
// dwells at 0 nor sees a stale answer to a repeated command.
#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#define PLAYER_MAGIC      "NCDP"
#define PLAYER_MAGIC_PPU  0x1FF0u
#define PLAYER_LEN_PPU    0x1FF4u
#define PLAYER_REQ_PPU    0x1FF8u
#define PLAYER_STAT_PPU   0x1FFBu
#define PLAYER_ANS_FAIL   0x80u

enum { PLC_PLAY = 1, PLC_PAUSE, PLC_STOP, PLC_NEXT, PLC_PREV, PLC_EJECT, PLC_LOAD };

typedef enum {
    PL_NO_DISC, PL_TRAY_OPEN, PL_LOADING, PL_STOPPED, PL_PLAYING, PL_PAUSED,
    PL_NOT_AUDIO
} player_state_t;

// `fresh` is a ROM that just booted: its first poll waits for the tile upload.
void player_arm(bool fresh);
void player_disarm(void);
bool player_armed(void);
bool player_magic_ok(void);              // one read of $1FF0..3

// Media edges from the idle loop's tray poll.
void player_disc_in(void);               // cdda_open() succeeded
void player_disc_other(void);            // a disc is in, not an audio one
void player_disc_out(void);
void player_media(int st, bool shut_empty);   // every TEST UNIT READY result

bool player_poll(void);                  // true when it printed
bool player_reap(int rc);                // a finished cdda job's result
void player_console(const char *arg);    // the C command
void player_report(void);

#endif
