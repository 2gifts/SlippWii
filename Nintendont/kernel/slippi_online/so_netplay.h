/* slippi_online/so_netplay.h
 * Peer-to-peer netplay: Slippi Dolphin's SlippiNetplayClient (Ishiiruka
 * v3.6.4) in C, so the Wii speaks exactly what a PC running Slippi expects.
 *
 * Wire format (SFML packets: big-endian, a u8 message id first):
 *   0x80 pad          s32 frame, u8 player, s32 checksum frame, u32 checksum,
 *                     then 8-byte pads newest first   unsequenced, channel 1
 *   0x81 pad ack      s32 frame, u8 player            unsequenced, channel 2
 *   0x82 selections   u8 char, u8 color, u8 char selected, u8 player,
 *                     u16 stage, u8 stage selected, u32 rng offset, u8 team,
 *                     u8 alt stage mode                reliable, channel 0
 *   0x83 connection selected                           reliable, channel 0
 *   0x84 chat         s32 message id, u8 player        reliable, channel 0
 *   0x85 game prep step, 0x86 synced game state        reliable, channel 0
 *
 * Every peer connects to every other peer at the same time (that is what
 * hole-punches the NATs), so a peer can end up with two connections to one
 * opponent; the lower player index keeps one and closes the rest, as Dolphin
 * does.
 *
 * Not thread-safe: the client calls everything under its lock. Sends are
 * flushed at once, so a pad goes out as soon as the game hands it over.
 */
#ifndef SO_NETPLAY_H
#define SO_NETPLAY_H

#include "global.h"
#include "so_mm.h"
#include "so_pads.h"

#define SO_REMOTE_MAX 3

typedef enum {
	SO_NP_IDLE,
	SO_NP_CONNECTING,
	SO_NP_CONNECTED,
	SO_NP_FAILED,	/* could not reach every opponent within 8 s */
	SO_NP_DISCONNECTED,
} SoNpStatus;

/* Dolphin's SlippiDisconnectReason, carried in the ENet disconnect data. */
enum {
	SO_DISCONNECT_UNSPECIFIED = 0,
	SO_DISCONNECT_POOR_PERFORMANCE = 1,
};

typedef struct {
	u8 player_idx;
	u8 character_id;
	u8 character_color;
	u8 team_id;
	u8 is_character_selected;
	u16 stage_id;
	u8 is_stage_selected;
	u8 alt_stage_mode;
	u32 rng_offset;
} SoSelections;

typedef struct {
	u8 step_idx;
	u8 char_selection;
	u8 char_color_selection;
	u8 stage_selections[2];
} SoGamePrepStep;

typedef struct {
	char match_id[96];
	u32 game_index;
	u32 tiebreak_index;
	u32 seconds_remaining;
	u8 stocks_remaining[4];
	u16 current_health[4];
} SoSyncedState;

typedef struct {
	u32 pad_packets_sent, pad_packets_received, pad_packets_stale, pad_packets_bad;
	u32 acks_sent, acks_received;
	u32 resends;	/* pad packets sent without a new pad */
	u32 test_dropped;
} SoNpStats;

int so_np_init(void);

/* Connects to the opponents of `match` from local UDP port `port`
 * (config: packet logging and the test drop rate). */
int so_np_start(const SoMatchResult *match, u16 port, const SoConfig *config);
/* Services ENet (never blocks). */
void so_np_poll(void);
/* How long the network thread may sleep before so_np_poll() has timed work
 * (a held ack), capped at `idle_us`. */
u32 so_np_wait_hint_us(u32 idle_us);
/* Disconnects (without waiting) and frees the socket. */
void so_np_stop(void);

SoNpStatus so_np_status(void);
u8 so_np_remote_count(void);
u8 so_np_local_player_idx(void);
int so_np_is_decider(void);
/* Remote index (0..count-1) <-> player index (0..3). */
u8 so_np_remote_player_idx(int remote);
int so_np_player_active(u8 player_idx);
u32 so_np_disconnect_reason(void);

/* ---- game ---------------------------------------------------------------- */

/* Resets per-game state before a game starts (StartSlippiGame). */
void so_np_start_game(void);

/* Queues our pad (NULL: resend what is not yet acked) and sends every
 * un-acked pad to each opponent (SendSlippiPad). */
void so_np_send_pad(const SoLocalPad *pad);

/* Lockstep access to an opponent's pads. */
int so_np_remote_pad(int remote, s32 frame, u8 *out);
s32 so_np_remote_head_frame(int remote);
void so_np_remote_checksum(int remote, s32 *frame, u32 *checksum);

/* How far we run ahead of the slowest opponent (CalcTimeOffsetUs). */
s32 so_np_time_offset_us(void);
u32 so_np_ping_us(int remote);
/* Average ping over the samples since the last call, in microseconds. */
u32 so_np_take_avg_ping_us(void);

/* ---- menus --------------------------------------------------------------- */

void so_np_set_selections(const SoSelections *s);
const SoSelections *so_np_local_selections(void);
const SoSelections *so_np_remote_selections(int remote);

void so_np_send_chat(int message_id);
/* Returns the last chat message received (0: none) and clears it. */
int so_np_take_chat(u8 *player_idx);

void so_np_send_connection_selected(void);
int so_np_connection_selected(void);

void so_np_send_game_prep_step(const SoGamePrepStep *s);
int so_np_game_prep_results(u8 step_idx, SoGamePrepStep *out);
void so_np_send_synced_state(const SoSyncedState *s);
const SoSyncedState *so_np_remote_synced_state(int remote);

/* Disconnects every opponent and tells them why. */
void so_np_force_disconnect(u32 reason);

void so_np_stats(SoNpStats *out);

#endif
