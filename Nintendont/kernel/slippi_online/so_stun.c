/* slippi_online/so_stun.c
 * STUN's messages (see so_stun.h): no I/O here (so_stun_io.c), unit-tested.
 */
#include "so_stun.h"
#include "string.h"

#define MAGIC_COOKIE 0x2112A442u
#define BINDING_REQUEST 0x0001
#define BINDING_SUCCESS 0x0101
#define ATTR_MAPPED_ADDRESS 0x0001
#define ATTR_XOR_MAPPED_ADDRESS 0x0020

static u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

void so_stun_build_request(u8 out[SO_STUN_REQUEST], const u8 id[12])
{
	out[0] = BINDING_REQUEST >> 8;
	out[1] = BINDING_REQUEST & 0xFF;
	out[2] = out[3] = 0;	/* no attributes */
	out[4] = (u8)(MAGIC_COOKIE >> 24);
	out[5] = (u8)(MAGIC_COOKIE >> 16);
	out[6] = (u8)(MAGIC_COOKIE >> 8);
	out[7] = (u8)MAGIC_COOKIE;
	memcpy(out + 8, id, 12);
}

static int decode(const u8 *value, u32 len, int xored, u32 *addr, u16 *port)
{
	u32 a;
	u16 p;
	if (len < 8 || value[1] != 0x01)	/* IPv4 */
		return -1;
	p = (u16)(value[2] << 8 | value[3]);
	a = be32(value + 4);
	if (xored) {
		p ^= (u16)(MAGIC_COOKIE >> 16);
		a ^= MAGIC_COOKIE;
	}
	*addr = a;
	*port = p;
	return 0;
}

int so_stun_parse_response(const u8 id[12], const u8 *data, u32 len, u32 *addr, u16 *port)
{
	u32 body, at = 20, fallback_addr = 0;
	u16 fallback_port = 0;
	int fallback = 0;
	if (len < 20 || (data[0] << 8 | data[1]) != BINDING_SUCCESS || be32(data + 4) != MAGIC_COOKIE ||
	    memcmp(data + 8, id, 12) != 0)
		return -1;
	body = (u32)(data[2] << 8 | data[3]);
	if (20 + body < len)
		len = 20 + body;
	while (at + 4 <= len) {
		u32 type = (u32)(data[at] << 8 | data[at + 1]), size = (u32)(data[at + 2] << 8 | data[at + 3]);
		const u8 *value = data + at + 4;
		if (at + 4 + size > len)
			break;
		if (type == ATTR_XOR_MAPPED_ADDRESS) {
			if (decode(value, size, 1, addr, port) == 0)
				return 0;
		} else if (type == ATTR_MAPPED_ADDRESS && !fallback) {
			fallback = decode(value, size, 0, &fallback_addr, &fallback_port) == 0;
		}
		at += 4 + size + ((4 - (size & 3)) & 3);	/* attributes are padded to four bytes */
	}
	if (!fallback)
		return -1;
	*addr = fallback_addr;
	*port = fallback_port;
	return 0;
}

const char *so_stun_nat_type(const SoStun *stun)
{
	if (!stun->have[0] || !stun->have[1])
		return "unknown";
	return stun->addr[0] == stun->addr[1] && stun->port[0] == stun->port[1] ? "cone" : "symmetric";
}
