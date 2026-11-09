/* SlippWii diagnostics (see glue.s): per-frame CPU time during online
 * games, and memory copy speed, reported over EXI 0xDA kind 4 (logged by
 * the kernel as "PERF ..."; kinds 1-3 are text_report.s's).
 *
 * Every 600 online in-game frames (and when a game ends):
 *   PERF n=<frames> cpu=<avg>/<max> draw=<avg>/<max> | busy=<avg>/<max>
 *        period=<min>-<max> over=<25%>,<33%>,<50%>,<75%> bus=<Hz>
 * cpu and draw are Melee's own HSD_Perf numbers: the time from the start of
 * the frame's logic to its end, and to the end of building the render, in
 * 1/10000ths of a frame by the OS's idea of the timebase (OSSecondsToTicks,
 * from the bus clock at 0x800000F8). busy (the same span to this call) and
 * period (between two frames) are raw timebase ticks, so the PC can check
 * the OS's scale against the real one. over counts frames whose logic took
 * more than 25/33/50/75% of a frame.
 *
 * When a game ends: PERF hist <frames by logic time: <1.5, <2, <2.5, <3,
 * <3.5, <4, <5, <6 ms, more>.
 *
 * Once, after 3 s on the online CSS, with interrupts off (bench):
 *   PERF copy <ticks per 1 MB: in place, to MEM2 plain, dcbz, dcbz+prefetch,
 *        back to MEM1> ok=<all copies exact> | hid2= bat3= mem2=
 *
 * Position independent (tools/pic_code.py): no globals or constant data.
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef struct {
	float k;		/* 0x00 10000.0 */
	u32 pad;
	u32 frames, cpu_sum, cpu_max, draw_sum, draw_max;
	u32 busy_sum, busy_max, period_min, period_max;
	u32 over[4];
	u64 last_tb;
	u32 was_in_game, css_frames, benched;
	u32 hist[9];
	u32 heap_size, heap_free_min, heap_big_min;
} Probe;

#define FN_EXITransferBuffer ((void (*)(void *, int, int))0x800055F0)

u64 timebase(void);
u32 bat(int n);
u32 irq_off(void);
void irq_restore(u32 msr);
void set_dbat3(u32 upper, u32 lower);
u32 hid2(void);
u32 hid0(void);
void lc_on(void);
void lc_off(void);
u32 lc_queue(void);
void lc_load(u32 lc, u32 phys, u32 blocks);
void lc_store(u32 lc, u32 phys, u32 blocks);
void set_hid0(u32 v);
void copy_lines(u32 dst, u32 src, u32 lines, u32 mode);

static u32 rd32(u32 a) { return *(volatile u32 *)a; }
static u8 rd8(u32 a) { return *(volatile u8 *)a; }
static float rdf(u32 a) { return *(volatile float *)a; }

/* ---- the report ---------------------------------------------------------- */

typedef struct {
	char *p, *end;
} Out;

static void put_c(Out *o, char c)
{
	if (o->p < o->end)
		*o->p++ = c;
}

/* Text without string constants (they would be data): up to 4 characters
 * packed in an immediate, C4('c','p','u','=') */
#define C4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static void put4(Out *o, u32 w)
{
	int i;
	for (i = 24; i >= 0; i -= 8)
		if ((w >> i) & 0xFF)
			put_c(o, (char)((w >> i) & 0xFF));
}

static void put_u(Out *o, u32 v)
{
	char d[10];
	int n = 0;
	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		put_c(o, d[--n]);
}

static void put_x(Out *o, u32 v)
{
	int i;
	for (i = 28; i >= 0; i -= 4)
		put_c(o, (char)(((v >> i) & 15) < 10 ? '0' + ((v >> i) & 15) : 'a' + ((v >> i) & 15) - 10));
}

/* 0xDA kind 3: two text fields, 48 and 64 bytes */
static void send(const char *a, const char *b)
{
	u8 raw[0x80 + 32];
	u8 *buf = (u8 *)(((u32)raw + 31) & ~31u);
	int i;
	for (i = 0; i < 0x80; i++)
		buf[i] = 0;
	buf[0] = 0xDA;
	buf[1] = 4;
	for (i = 0; i < 47 && a[i]; i++)
		buf[0x10 + i] = (u8)a[i];
	for (i = 0; i < 63 && b[i]; i++)
		buf[0x40 + i] = (u8)b[i];
	FN_EXITransferBuffer(buf, 0x80, 1);
}

/* Melee's biggest OS heap (OSAlloc's HeapArray at 0x804D7360, NumHeaps at
 * 0x804D7364; {size, free, allocated}, cells {prev, next, size}): its size,
 * its free bytes, and its largest free cell. */
static void heap_info(u32 *size, u32 *free, u32 *largest, u32 *cell_out)
{
	u32 arr = rd32(0x804D7360), n = rd32(0x804D7364), i;
	*size = *free = *largest = *cell_out = 0;
	if (arr < 0x80000000u || arr >= 0x81800000u || n > 16)
		return;
	for (i = 0; i < n; i++) {
		u32 sz = rd32(arr + i * 12), cell = rd32(arr + i * 12 + 4), f = 0, big = 0, at = 0, count = 0;
		if (sz & 0x80000000u || sz <= *size)
			continue;
		while (cell >= 0x80000000u && cell < 0x81800000u && count++ < 4096) {
			u32 c = rd32(cell + 8);
			f += c;
			if (c > big) {
				big = c;
				at = cell;
			}
			cell = rd32(cell + 4);
		}
		*size = sz;
		*free = f;
		*largest = big;
		*cell_out = at;
	}
}

/* ---- the frame statistics --------------------------------------------------- */

static void report(Probe *s)
{
	char a[48], b[64];
	Out o;
	u32 i;
	if (!s->frames)
		return;
	{
		u32 size, free, largest, cell;
		heap_info(&size, &free, &largest, &cell);
		s->heap_size = size;
		if (!s->heap_free_min || free < s->heap_free_min)
			s->heap_free_min = free;
		if (!s->heap_big_min || largest < s->heap_big_min)
			s->heap_big_min = largest;
	}
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('n', '=', 0, 0));
	put_u(&o, s->frames);
	put4(&o, C4(' ', 'c', 'p', 'u')); put4(&o, C4('=', 0, 0, 0));
	put_u(&o, s->cpu_sum / s->frames);
	put_c(&o, '/');
	put_u(&o, s->cpu_max);
	put4(&o, C4(' ', 'd', 'r', 'a')); put4(&o, C4('w', '=', 0, 0));
	put_u(&o, s->draw_sum / s->frames);
	put_c(&o, '/');
	put_u(&o, s->draw_max);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('b', 'u', 's', 'y')); put4(&o, C4('=', 0, 0, 0));
	put_u(&o, s->busy_sum / s->frames);
	put_c(&o, '/');
	put_u(&o, s->busy_max);
	put4(&o, C4(' ', 'p', 'e', 'r')); put4(&o, C4('i', 'o', 'd', '='));
	put_u(&o, s->period_min);
	put_c(&o, '-');
	put_u(&o, s->period_max);
	put4(&o, C4(' ', 'o', 'v', 'e')); put4(&o, C4('r', '=', 0, 0));
	for (i = 0; i < 4; i++) {
		if (i)
			put_c(&o, ',');
		put_u(&o, s->over[i]);
	}

	*o.p = 0;
	send(a, b);
	s->frames = s->cpu_sum = s->cpu_max = s->draw_sum = s->draw_max = 0;
	s->busy_sum = s->busy_max = s->period_max = 0;
	s->period_min = ~0u;
	for (i = 0; i < 4; i++)
		s->over[i] = 0;
}

/* ---- the copy test --------------------------------------------------------------- */

/* The copy tests: 1 MB of game code (static) copied to SlippWii's MEM2
 * scratch (0x12370000, kernel SO_PPC_SCRATCH_SIZE; mapped here at
 * 0x90000000 with DBAT3, which the game leaves unused) and back onto itself
 * (the same bytes), four ways, with interrupts off. Each result is checked. */
#define SCRATCH 0x92370000u
#define SOURCE 0x80010000u
#define LINES (0x100000 / 32)

static u32 timed_copy(u32 dst, u32 src, u32 mode)
{
	u64 t0 = timebase();
	copy_lines(dst, src, LINES, mode);
	return (u32)(timebase() - t0);
}

static int same(u32 a, u32 b)
{
	u32 i;
	for (i = 0; i < 0x100000; i += 4)
		if (rd32(a + i) != rd32(b + i))
			return 0;
	return 1;
}

/* Through the locked cache with its DMA engine: four 4 KB buffers in turn,
 * each a load then a store, the queue kept full (a buffer is reused once its
 * last store is done: at most 6 operations queued). Physical addresses. */
static u32 lc_copy(u32 dst, u32 src, u32 bytes)
{
	u32 i, chunks = bytes / 4096;
	u64 t0 = timebase();
	for (i = 0; i < chunks; i++) {
		u32 lc = 0xE0000000u + (i & 3) * 4096;
		while (lc_queue() > 6)
			;
		lc_load(lc, src + i * 4096, 128);
		lc_store(lc, dst + i * 4096, 128);
	}
	while (lc_queue())
		;
	return (u32)(timebase() - t0);
}

static void bench(void)
{
	char a[48], b[64];
	Out o;
	u32 msr, t[9], ok = 1, lc_ok = 0, i, h0 = hid0(), hsize, hfree, hbig, hcell;
	/* a Wii (the bus at 243 MHz; Dolphin's GameCube has no MEM2) with DBAT3
	 * free: map MEM2 (64 MB) at 0x90000000 */
	int mem2 = rd32(0x800000F8) == 243000000u && bat(3) == 0;

	heap_info(&hsize, &hfree, &hbig, &hcell);
	t[5] = t[6] = t[7] = t[8] = 0;
	msr = irq_off();
	t[0] = timed_copy(SOURCE, SOURCE, 0);	/* in place, as the first test */
	if (hbig >= 0x100000 + 64) {	/* into the largest free heap cell, past its header */
		u32 dst = (hcell + 64) & ~31u;
		t[5] = timed_copy(dst, SOURCE, 1);
		ok &= same(dst, SOURCE);
	}
	if (mem2) {
		set_dbat3(0x900007FFu, 0x10000002u);
		t[1] = timed_copy(SCRATCH, SOURCE, 0);	/* plain */
		ok &= same(SCRATCH, SOURCE);
		t[2] = timed_copy(SCRATCH, SOURCE, 1);	/* dcbz the destination */
		ok &= same(SCRATCH, SOURCE);
		t[3] = timed_copy(SCRATCH, SOURCE, 3);	/* dcbz + prefetch */
		ok &= same(SCRATCH, SOURCE);
		t[4] = timed_copy(SOURCE, SCRATCH, 3);	/* back: the load direction */
		ok &= same(SCRATCH, SOURCE);
		if (h0 & 1) {	/* NOOPTI makes dcbt a no-op: the same with prefetching on */
			set_hid0(h0 & ~1u);
			t[6] = timed_copy(SOURCE, SCRATCH, 3);
			set_hid0(h0);
			ok &= same(SCRATCH, SOURCE);
		}
		if (!(hid2() & 0x10000000u)) {	/* the locked cache is the game's to use otherwise */
			set_dbat3(0xE0000003u, 0xE0000002u);	/* map it, 128 KB at 0xE0000000 */
			lc_on();
			t[7] = lc_copy(0x12370000u, SOURCE & 0x0FFFFFFFu, 0x100000);	/* MEM1 -> MEM2 */
			t[8] = lc_copy(SOURCE & 0x0FFFFFFFu, 0x12370000u, 0x100000);	/* MEM2 -> MEM1 */
			lc_off();
			set_dbat3(0x900007FFu, 0x10000002u);
			lc_ok = same(SCRATCH, SOURCE);
		}
	} else {
		t[1] = t[2] = t[3] = t[4] = 0;
	}
	irq_restore(msr);

	o.p = a;
	o.end = a + 47;
	put4(&o, C4('c', 'o', 'p', 'y'));
	for (i = 0; i < 5; i++) {
		put_c(&o, i ? ',' : ' ');
		put_u(&o, t[i]);
	}
	put4(&o, C4(' ', 'o', 'k', '='));
	put_u(&o, ok);
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('m', '1', 'm', '1'));
	put_c(&o, '=');
	put_u(&o, t[5]);
	put4(&o, C4(' ', 'p', 'f', '='));
	put_u(&o, t[6]);
	put4(&o, C4(' ', 'h', '0', '='));
	put_x(&o, h0);
	put4(&o, C4(' ', 'h', 'f', '='));
	put_u(&o, hbig >> 10);
	put4(&o, C4(' ', 'h', 'i', 'd'));
	put4(&o, C4('2', '=', 0, 0));
	put_x(&o, hid2());
	(void)mem2;
	*o.p = 0;
	send(a, b);
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('l', 'c', 'd', 'm'));
	put4(&o, C4('a', ' ', 'm', '1'));
	put4(&o, C4('m', '2', '=', 0));
	put_u(&o, t[7]);
	put4(&o, C4(' ', 'm', '2', 'm'));
	put4(&o, C4('1', '=', 0, 0));
	put_u(&o, t[8]);
	put4(&o, C4(' ', 'o', 'k', '='));
	put_u(&o, lc_ok);
	*o.p = 0;
	b[0] = 0;
	send(a, b);
}

/* The game's logic times, binned (ms): <1.5, <2, <2.5, <3, <3.5, <4, <5, <6, more */
static void report_hist(Probe *s)
{
	char a[48], b[64];
	Out o;
	u32 i;
	o.p = a;
	o.end = a + 47;
	put4(&o, C4('h', 'i', 's', 't'));
	for (i = 0; i < 9; i++) {
		put_c(&o, i ? ',' : ' ');
		put_u(&o, s->hist[i]);
		s->hist[i] = 0;
	}
	*o.p = 0;
	o.p = b;
	o.end = b + 63;
	put4(&o, C4('h', 'e', 'a', 'p'));
	put_c(&o, '=');
	put_u(&o, s->heap_size >> 10);
	put4(&o, C4(' ', 'f', 'r', 'e'));
	put4(&o, C4('e', '=', 0, 0));
	put_u(&o, s->heap_free_min >> 10);
	put4(&o, C4(' ', 'b', 'i', 'g'));
	put_c(&o, '=');
	put_u(&o, s->heap_big_min >> 10);
	put4(&o, C4(' ', 'K', 'B', 0));
	s->heap_free_min = s->heap_big_min = 0;
	*o.p = 0;
	send(a, b);
}

/* ---- each frame ------------------------------------------------------------------ */

void perf_probe_frame(Probe *s)
{
	u8 major = rd8(0x80479D30), minor = rd8(0x80479D33);
	int in_game = major == 8 && minor == 2;
	u64 now = timebase();

	if (major == 8 && minor == 0) {
		if (!s->benched && ++s->css_frames == 180) {
			s->benched = 1;
			bench();
		}
	} else {
		s->css_frames = 0;
	}

	if (!in_game) {
		if (s->was_in_game) {
			report(s);	/* the game's last frames */
			report_hist(s);
		}
		s->was_in_game = 0;
		s->last_tb = 0;
		return;
	}
	if (!s->was_in_game) {
		s->was_in_game = 1;
		s->frames = s->cpu_sum = s->cpu_max = s->draw_sum = s->draw_max = 0;
		s->busy_sum = s->busy_max = s->period_max = 0;
		s->period_min = ~0u;
		s->over[0] = s->over[1] = s->over[2] = s->over[3] = 0;
	}
	{
		u32 cpu = (u32)(int)(rdf(0x80407218) * s->k);	/* HSD_PerfCurrentStat.cpu_time */
		u32 draw = (u32)(int)(rdf(0x8040721C) * s->k);	/* .draw_time */
		u64 start = ((u64)rd32(0x804D76F8) << 32) | rd32(0x804D76FC);	/* HSD_PerfSetStartTime's */
		u32 busy = (u32)(now - start);
		s->frames++;
		s->cpu_sum += cpu;
		s->draw_sum += draw;
		s->busy_sum += busy;
		if (cpu > s->cpu_max)
			s->cpu_max = cpu;
		if (draw > s->draw_max)
			s->draw_max = draw;
		if (busy > s->busy_max)
			s->busy_max = busy;
		{
			/* 1 ms = 599 (1/10000ths of a 16.68 ms frame) */
			u32 bin = cpu < 899 ? 0 : cpu < 1199 ? 1 : cpu < 1499 ? 2 : cpu < 1798 ? 3 : cpu < 2098 ? 4 :
				  cpu < 2398 ? 5 : cpu < 2997 ? 6 : cpu < 3596 ? 7 : 8;
			s->hist[bin]++;
		}
		if (cpu > 2500)
			s->over[0]++;
		if (cpu > 3333)
			s->over[1]++;
		if (cpu > 5000)
			s->over[2]++;
		if (cpu > 7500)
			s->over[3]++;
		if (s->last_tb) {
			u32 period = (u32)(now - s->last_tb);
			if (period < s->period_min)
				s->period_min = period;
			if (period > s->period_max)
				s->period_max = period;
		}
		s->last_tb = now;
	}
	if (s->frames == 600)
		report(s);
}
