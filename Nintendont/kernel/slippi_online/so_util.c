/* slippi_online/so_util.c
 * Bounded formatting and parsing helpers (see so_util.h).
 */
#include "so_util.h"
#include "string.h"

typedef struct {
	char *out;
	int size;
	int len;
} Sink;

static void put(Sink *s, char c)
{
	if (s->len + 1 < s->size)
		s->out[s->len] = c;
	s->len++;
}

static void put_padded(Sink *s, const char *text, int text_len, int width, int left, char pad)
{
	int fill = width > text_len ? width - text_len : 0;
	int i;
	if (!left)
		for (i = 0; i < fill; i++)
			put(s, pad);
	for (i = 0; i < text_len; i++)
		put(s, text[i]);
	if (left)
		for (i = 0; i < fill; i++)
			put(s, ' ');
}

static int format_number(char *buf, unsigned long long value, int base, int upper, int negative)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0, len = 0;
	do {
		tmp[n++] = digits[value % base];
		value /= base;
	} while (value);
	if (negative)
		buf[len++] = '-';
	while (n)
		buf[len++] = tmp[--n];
	return len;
}

int so_vsnprintf(char *out, int size, const char *fmt, va_list ap)
{
	Sink s = { out, size, 0 };
	for (; *fmt; fmt++) {
		int left = 0, width = 0, precision = -1, longs = 0;
		char pad = ' ', num[24];
		if (*fmt != '%') {
			put(&s, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				pad = '0';
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			precision = 0;
			if (*fmt == '*') {
				precision = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					precision = precision * 10 + (*fmt++ - '0');
			}
		}
		while (*fmt == 'l') {
			longs++;
			fmt++;
		}
		switch (*fmt) {
		case 'd':
		case 'i': {
			long long v = longs >= 2 ? va_arg(ap, long long) : longs ? va_arg(ap, long) : va_arg(ap, int);
			unsigned long long mag = v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
			put_padded(&s, num, format_number(num, mag, 10, 0, v < 0), width, left, pad);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			unsigned long long v = longs >= 2 ? va_arg(ap, unsigned long long)
			                     : longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
			put_padded(&s, num, format_number(num, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', 0), width, left, pad);
			break;
		}
		case 'p':
			num[0] = '0';
			num[1] = 'x';
			put_padded(&s, num, 2 + format_number(num + 2, (unsigned long long)(size_t)va_arg(ap, void *), 16, 0, 0),
				width, left, ' ');
			break;
		case 'c':
			num[0] = (char)va_arg(ap, int);
			put_padded(&s, num, 1, width, left, ' ');
			break;
		case 's': {
			const char *text = va_arg(ap, const char *);
			int n = 0;
			if (!text)
				text = "(null)";
			while (text[n] && (precision < 0 || n < precision))
				n++;
			put_padded(&s, text, n, width, left, ' ');
			break;
		}
		case '%':
			put(&s, '%');
			break;
		case '\0':
			fmt--;
			break;
		default:
			put(&s, '%');
			put(&s, *fmt);
			break;
		}
	}
	if (size > 0)
		out[s.len < size ? s.len : size - 1] = '\0';
	return s.len < size ? s.len : (size > 0 ? size - 1 : 0);
}

int so_snprintf(char *out, int size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = so_vsnprintf(out, size, fmt, ap);
	va_end(ap);
	return n;
}

long so_parse_long(const char *text, const char **end)
{
	/* Accumulates unsigned and saturates, so long digit strings cannot
	 * overflow. */
	const unsigned long limit = (~0UL >> 1) / 10;
	unsigned long value = 0;
	int negative = 0;
	while (*text == ' ' || *text == '\t')
		text++;
	if (*text == '-' || *text == '+')
		negative = *text++ == '-';
	while (*text >= '0' && *text <= '9') {
		value = value > limit ? ~0UL >> 1 : value * 10 + (unsigned long)(*text - '0');
		text++;
	}
	if (end)
		*end = text;
	if (value > (~0UL >> 1))
		value = ~0UL >> 1;
	return negative ? -(long)value : (long)value;
}

static int parse_ipv4_prefix(const char *text, u32 *addr, const char **end)
{
	u32 result = 0;
	int part;
	for (part = 0; part < 4; part++) {
		const char *after;
		long v;
		if (*text < '0' || *text > '9')
			return -1;
		v = so_parse_long(text, &after);
		if (v > 255 || after - text > 3)
			return -1;
		result = (result << 8) | (u32)v;
		text = after;
		if (part < 3) {
			if (*text != '.')
				return -1;
			text++;
		}
	}
	*addr = result;
	*end = text;
	return 0;
}

int so_parse_ipv4(const char *text, u32 *addr)
{
	const char *end;
	u32 result;
	if (parse_ipv4_prefix(text, &result, &end) < 0 || *end != '\0')
		return -1;
	*addr = result;
	return 0;
}

int so_parse_endpoint(const char *text, u32 *addr, u16 *port)
{
	const char *end;
	u32 result;
	long p;
	if (parse_ipv4_prefix(text, &result, &end) < 0 || *end != ':')
		return -1;
	text = end + 1;
	if (*text < '0' || *text > '9')
		return -1;
	p = so_parse_long(text, &end);
	if (*end != '\0' || p < 1 || p > 65535)
		return -1;
	*addr = result;
	*port = (u16)p;
	return 0;
}

void so_format_ipv4(u32 addr, char *out, int size)
{
	so_snprintf(out, size, "%u.%u.%u.%u", addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
}
