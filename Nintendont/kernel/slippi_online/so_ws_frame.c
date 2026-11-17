/* slippi_online/so_ws_frame.c
 * WebSocket frames (see so_ws.h): no I/O here (so_ws.c), unit-tested.
 */
#include "so_ws.h"
#include "string.h"

int so_ws_build_frame(u8 *out, u32 size, u8 opcode, const u8 *payload, u32 len, const u8 mask[4])
{
	u32 n = 0, i;
	if (len > 0xFFFF || size < len + 8)
		return -1;
	out[n++] = (u8)(0x80 | opcode);	/* FIN: never fragmented */
	if (len < 126) {
		out[n++] = (u8)(0x80 | len);
	} else {
		out[n++] = 0x80 | 126;
		out[n++] = (u8)(len >> 8);
		out[n++] = (u8)len;
	}
	memcpy(out + n, mask, 4);
	n += 4;
	for (i = 0; i < len; i++)
		out[n + i] = payload[i] ^ mask[i & 3];
	return (int)(n + len);
}

int so_ws_parse_frame(u8 *data, u32 have, u32 limit, u8 *opcode, int *fin, u8 **payload, u32 *payload_len)
{
	u32 head = 2, len, i;
	int masked;
	if (have < 2)
		return 0;
	masked = data[1] & 0x80;
	len = data[1] & 0x7F;
	if (len == 126) {
		if (have < 4)
			return 0;
		len = ((u32)data[2] << 8) | data[3];
		head = 4;
	} else if (len == 127) {
		if (have < 10)
			return 0;
		if (data[2] | data[3] | data[4] | data[5] | data[6] | data[7])
			return -1;
		len = ((u32)data[8] << 8) | data[9];
		head = 10;
	}
	if (masked)
		head += 4;
	if (len > limit || head + len > limit + 14)
		return -1;
	if (have < head + len)
		return 0;
	if (masked)
		for (i = 0; i < len; i++)
			data[head + i] ^= data[head - 4 + (i & 3)];
	*opcode = data[0] & 0x0F;
	*fin = (data[0] & 0x80) != 0;
	*payload = data + head;
	*payload_len = len;
	return (int)(head + len);
}
