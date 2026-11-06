/* Melee's Text_InitializeSubtext (see glue.s) takes a format and its arguments, and
 * Slippi's FG_CreateSubtext calls it with the format but not the arguments: they are
 * whatever the registers hold, and its next call, to Text_UpdateSubtextContents, gives
 * the real ones. With "%s" in the format that reads a string at any address. Dolphin
 * gets away with it; a console has nothing mapped there and crashes. The one case
 * players meet: a game ending on the timer with a ledge grab limit exceeded, whose
 * "%s Exceeded Ledge Grab Limit of %d" comes with r5 = 1.
 *
 * So before the call: a "%s" whose argument can't be a string gets an empty one.
 *
 * Built position independent (tools/pic_code.py): no globals, no constant data.
 */
typedef unsigned int u32;

#define OSReport ((void (*)(const char *, ...))0x803456A8)

#define ARG_REGISTERS 6	/* r5..r10; more would be on the stack: none of Slippi's has */

static int in_memory(u32 p)
{
	return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0xC0000000u && p < 0xC1800000u);
}

static int is_conversion(char c)
{
	if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
		return 0;
	return c != 'h' && c != 'l' && c != 'L' && c != 'q' && c != 'j' && c != 'z' && c != 't';
}

static int is_float(char c)
{
	return c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G' || c == 'a' || c == 'A';
}

/* args: the saved r5..r10. A float argument travels in the float registers. */
void text_args_check(const char *format, u32 *args, const char *empty, const char *report)
{
	const char *p = format;
	int n = 0;
	char c;
	if (!in_memory((u32)format))
		return;
	while (*p && n < ARG_REGISTERS) {
		if (*p++ != '%')
			continue;
		if (*p == '%') {
			p++;
			continue;
		}
		while (*p && !is_conversion(*p)) {
			if (*p == '*')
				n++;
			p++;
		}
		c = *p;
		if (!c || n >= ARG_REGISTERS)
			break;
		p++;
		if (is_float(c))
			continue;
		if (c == 's' && !in_memory(args[n])) {
			if (args[n])	/* null: what Melee's own menus do, and fine */
				OSReport(report, format);
			args[n] = (u32)empty;
		}
		n++;
	}
}
