/* slippi_online/so_pads.c
 * Pad exchange bookkeeping (see so_pads.h).
 */
#include "so_pads.h"
#include "so_util.h"
#include "string.h"

#define MSG_PAD 0x80
#define PAD_HEADER_SIZE 14	/* id, frame, player index, checksum frame, checksum */

/* ---- local pads ---------------------------------------------------------- */

void so_local_pads_reset(SoLocalPads *q)
{
	q->head = 0;
	q->count = 0;
}

void so_local_pads_push(SoLocalPads *q, const SoLocalPad *pad)
{
	q->ring[q->head % SO_LOCAL_PAD_RING] = *pad;
	q->head++;
	if (q->count < SO_LOCAL_PAD_RING)
		q->count++;
}

const SoLocalPad *so_local_pads_at(const SoLocalPads *q, u32 i)
{
	if (i >= q->count)
		return NULL;
	return &q->ring[(q->head - 1 - i) % SO_LOCAL_PAD_RING];
}

const SoLocalPad *so_local_pads_newest(const SoLocalPads *q)
{
	return so_local_pads_at(q, 0);
}

void so_local_pads_drop_acked(SoLocalPads *q, s32 min_acked_frame)
{
	const SoLocalPad *newest = so_local_pads_newest(q);
	if (!newest)
		return;
	if (min_acked_frame < newest->frame - SO_PADS_MAX_PER_PACKET)
		min_acked_frame = newest->frame - SO_PADS_MAX_PER_PACKET;
	while (q->count > 0 && so_local_pads_at(q, q->count - 1)->frame < min_acked_frame)
		q->count--;
}

int so_local_pads_encode(const SoLocalPads *q, u8 player_idx, u8 *out, int size)
{
	const SoLocalPad *newest = so_local_pads_newest(q);
	u32 i;
	if (!newest)
		return 0;
	if (size < PAD_HEADER_SIZE + (int)q->count * SO_PAD_SIZE)
		return -1;
	out[0] = MSG_PAD;
	so_put_u32(out + 1, (u32)newest->frame);
	out[5] = player_idx;
	so_put_u32(out + 6, (u32)newest->checksum_frame);
	so_put_u32(out + 10, newest->checksum);
	for (i = 0; i < q->count; i++)
		memcpy(out + PAD_HEADER_SIZE + i * SO_PAD_SIZE, so_local_pads_at(q, i)->pad, SO_PAD_SIZE);
	return PAD_HEADER_SIZE + q->count * SO_PAD_SIZE;
}

/* ---- remote pads --------------------------------------------------------- */

void so_remote_pads_reset(SoRemotePads *q)
{
	q->head_frame = 0;
	q->count = 0;
}

int so_remote_pads_add(SoRemotePads *q, s32 newest_frame, const u8 *pads, u32 pad_bytes)
{
	s64 to_copy = (s64)newest_frame - (s64)q->head_frame;
	s64 i;
	if (to_copy > SO_PADS_MAX_PER_PACKET || to_copy * SO_PAD_SIZE > (s64)pad_bytes)
		return -1;
	if (to_copy <= 0)
		return 0;	/* an old packet: nothing new */
	/* Oldest first, so the ring stays in frame order. */
	for (i = to_copy - 1; i >= 0; i--) {
		s32 frame = (s32)(newest_frame - i);
		memcpy(q->pads[(u32)frame % SO_REMOTE_PAD_RING], pads + i * SO_PAD_SIZE, SO_PAD_SIZE);
		if (q->count < SO_REMOTE_PAD_RING)
			q->count++;
	}
	q->head_frame = newest_frame;
	return (int)to_copy;
}

int so_remote_pads_get(const SoRemotePads *q, s32 frame, u8 *out)
{
	if (q->count == 0 || frame > q->head_frame || frame <= q->head_frame - (s32)q->count)
		return -1;
	memcpy(out, q->pads[(u32)frame % SO_REMOTE_PAD_RING], SO_PAD_SIZE);
	return 0;
}

/* ---- ack timers ---------------------------------------------------------- */

void so_ack_timers_reset(SoAckTimers *t)
{
	t->head = 0;
	t->count = 0;
}

void so_ack_timers_push(SoAckTimers *t, s32 frame, u64 sent_us)
{
	u32 slot = (t->head + t->count) % SO_ACK_TIMER_RING;
	if (t->count == SO_ACK_TIMER_RING) {
		/* Full: forget the oldest send. */
		t->head = (t->head + 1) % SO_ACK_TIMER_RING;
		t->count--;
	}
	t->frame[slot] = frame;
	t->sent_us[slot] = sent_us;
	t->count++;
}

s64 so_ack_timers_ack(SoAckTimers *t, s32 frame, u64 now_us)
{
	u64 sent;
	while (t->count > 0 && t->frame[t->head] < frame) {
		t->head = (t->head + 1) % SO_ACK_TIMER_RING;
		t->count--;
	}
	if (t->count == 0 || t->frame[t->head] != frame)
		return -1;
	sent = t->sent_us[t->head];
	t->head = (t->head + 1) % SO_ACK_TIMER_RING;
	t->count--;
	return (s64)(now_us - sent);
}

/* ---- time sync ----------------------------------------------------------- */

void so_time_sync_reset(SoTimeSync *s)
{
	s->count = 0;
	s->next = 0;
}

void so_time_sync_add(SoTimeSync *s, s32 offset_us)
{
	s->samples[s->next] = offset_us;
	s->next = (s->next + 1) % SO_TIME_SYNC_SAMPLES;
	if (s->count < SO_TIME_SYNC_SAMPLES)
		s->count++;
}

s32 so_time_sync_offset(const SoTimeSync *s, int *valid)
{
	s32 sorted[SO_TIME_SYNC_SAMPLES];
	u32 n = s->count, i, j, trim;
	s64 sum = 0;
	for (i = 0; i < n; i++) {
		/* Insertion sort: at most 30 values. */
		s32 v = s->samples[i];
		for (j = i; j > 0 && sorted[j - 1] > v; j--)
			sorted[j] = sorted[j - 1];
		sorted[j] = v;
	}
	trim = n / 3;
	if (n - 2 * trim == 0) {
		*valid = 0;
		return 0;
	}
	for (i = trim; i < n - trim; i++)
		sum += sorted[i];
	*valid = 1;
	return (s32)(sum / (s64)(n - 2 * trim));
}

s32 so_time_sync_sample(u64 arrived_us, u32 ping_us, s32 remote_frame, s32 local_frame, u64 local_sent_us)
{
	s64 remote_sent_us = (s64)arrived_us - (s64)(ping_us / 2);
	s64 frame_diff_us = (s64)SO_FRAME_US * ((s64)local_frame - (s64)remote_frame);
	return (s32)(remote_sent_us - (s64)local_sent_us + frame_diff_us);
}
