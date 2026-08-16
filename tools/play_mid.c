/*
 * midifile.c - play a Standard MIDI File (.mid) through the
 * SoundBlaster driver's /dev/sbmidi raw MIDI port.
 *
 * The driver takes a stream of 4-byte packets:
 *     byte 0    : one raw MIDI byte
 *     bytes 1-3 : 24-bit little-endian pause, in milliseconds, to take
 *                 *before* sending that byte
 * and does the pacing itself, in the kernel, against lbolt.  So all this
 * program does is turn an SMF into that packet stream: parse the chunks,
 * merge the tracks into one time-ordered event list, run the tempo map to
 * convert ticks to milliseconds, and emit.  It never sleeps.
 *
 * Usage:
 *     midifile [-c] [-d] [-m mapfile] [-l] [-o outfile] [-q] file.mid
 *
 *     -c          MT-32 mode.  Sends a System Area sysex that reassigns
 *                 parts 1-8 to MIDI channels 1-8 (an MT-32 powers up with
 *                 them on channels 2-9, so channel 1 is silent by default),
 *                 and translates General MIDI program numbers to MT-32
 *                 timbres.  Use this for GM files; leave it off for files
 *                 authored for the MT-32.
 *     -d          Drop channel 10.  GM drum note numbers do not match the
 *                 MT-32 rhythm key map, so GM percussion comes out as
 *                 nonsense; this mutes it.
 *     -m mapfile  Load the GM->MT-32 program map from a file (128 decimal
 *                 numbers, whitespace separated, '#' to end of line is a
 *                 comment).  Overrides the built-in table.
 *     -l          List events instead of playing: dump "time status data"
 *                 lines to stdout.  Useful for checking the parse on a
 *                 machine where you cannot hear the result.
 *     -o outfile  Write the raw packet stream to a file instead of
 *                 /dev/sbmidi.  `cp` that file to /dev/sbmidi later to
 *                 play it, or feed it to midiplay.
 *     -q          Quiet: no progress output.
 *
 * Compile:
 *     cc -M3 -o midifile midifile.c
 *
 * Interrupting: SIGINT breaks out of the blocking write(), and we send
 * all-notes-off across all 16 channels before exiting.  The driver's own
 * midi_close() only sweeps channel 1, so without this a stuck note on any
 * other channel rings forever.
 */

#include <sys/types.h>
#include <sys/fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

extern char *malloc();
extern long lseek();

#define MAXTRK 64		/* plenty; format 1 files rarely exceed 32 */
#define OUTPKTS 256		/* packets buffered before a write() */
#define MAXDELTA 0xffffffL	/* 24-bit pause field, ~4.6 hours */

/* ------------------------------------------------------------------ */
/* options							      */
/* ------------------------------------------------------------------ */

int mt32mode = 0;
int dropdrums = 0;
int listonly = 0;
int quiet = 0;
char *outname = 0;

/*
 * GM program -> MT-32 timbre.
 *
 * This is a coarse, family-level map, not a faithful copy of the tables
 * in DOSBox or ScummVM: GM instrument N is mapped to whichever MT-32
 * preset sits in roughly the right family.  Expect to tune it by ear.
 * Override the whole thing with -m rather than editing this array.
 */
unsigned char gm2mt32[128] = {
	/* 0-7    piano			 */  0,  1,  2,  7,  3,  4,  5,  6,
	/* 8-15   chromatic percussion	 */ 47, 47, 48, 48, 49, 49, 50, 50,
	/* 16-23  organ			 */  8,  9, 10, 11, 12, 13, 14, 15,
	/* 24-31  guitar		 */ 16, 16, 17, 17, 18, 18, 19, 19,
	/* 32-39  bass			 */ 20, 21, 21, 22, 22, 23, 23, 23,
	/* 40-47  strings		 */ 24, 24, 25, 25, 26, 26, 27, 28,
	/* 48-55  ensemble		 */ 29, 29, 30, 30, 31, 31, 32, 32,
	/* 56-63  brass			 */ 33, 33, 34, 34, 35, 35, 36, 36,
	/* 64-71  reed			 */ 37, 37, 38, 38, 39, 39, 40, 40,
	/* 72-79  pipe			 */ 41, 41, 42, 42, 43, 43, 44, 44,
	/* 80-87  synth lead		 */ 45, 45, 46, 46, 51, 51, 52, 52,
	/* 88-95  synth pad		 */ 53, 53, 54, 54, 55, 55, 56, 56,
	/* 96-103 synth effects		 */ 57, 57, 58, 58, 59, 59, 60, 60,
	/* 104-111 ethnic		 */ 17, 17, 18, 18, 19, 19, 27, 27,
	/* 112-119 percussive		 */ 47, 48, 49, 50, 61, 61, 62, 62,
	/* 120-127 sound effects	 */ 63, 63, 63, 63, 63, 63, 63, 63
};

/* ------------------------------------------------------------------ */
/* input file							      */
/* ------------------------------------------------------------------ */

unsigned char *filebuf;
long filelen;

/* ------------------------------------------------------------------ */
/* track cursors						      */
/* ------------------------------------------------------------------ */

struct track {
	unsigned char *p;	/* next unread byte			*/
	unsigned char *end;	/* one past the last byte of the chunk	*/
	long tick;		/* absolute tick of the pending event	*/
	int running;		/* running status byte, 0 if none	*/
	int done;
};

struct track trk[MAXTRK];
int ntrk;

/* ------------------------------------------------------------------ */
/* tick -> millisecond conversion				      */
/*								      */
/* us per tick = tconv_num / tconv_den.  For a PPQN division that is    */
/* tempo/division and changes with every FF 51 meta event; for an SMPTE */
/* division it is fixed and tempo events are ignored.		      */
/*								      */
/* Time is accumulated in absolute terms (cur_tick -> abs_ms) with the  */
/* sub-millisecond remainder carried in us_acc and the sub-microsecond  */
/* remainder in frac, so there is no cumulative rounding drift over a   */
/* long file.  Everything stays in 32-bit range: abs_ms overflows after */
/* 24 days, and the per-chunk products are bounded by the chunk size    */
/* computed in advance_to().					      */
/* ------------------------------------------------------------------ */

long tconv_num = 500000L;	/* default tempo: 120 bpm */
long tconv_den = 480L;
int smpte = 0;

long cur_tick = 0L;
long abs_ms = 0L;
long us_acc = 0L;
long frac = 0L;
long time_bias = 0L;		/* ms added to every event (MT-32 setup) */

/* ------------------------------------------------------------------ */
/* packet output						      */
/* ------------------------------------------------------------------ */

unsigned char outbuf[OUTPKTS * 4];
int outused = 0;
long last_ms = 0L;		/* time of the last byte we emitted */
int outfd = -1;
long bytes_out = 0L;
int interrupted = 0;

/* ------------------------------------------------------------------ */

die(msg)
char *msg;
{
	fprintf(stderr, "midifile: %s\n", msg);
	exit(1);
}

void
onintr()
{
	interrupted = 1;
	signal(SIGINT, onintr);
}

/* ------------------------------------------------------------------ */
/* big-endian readers						      */
/* ------------------------------------------------------------------ */

long
be32(p)
unsigned char *p;
{
	return (((long) p[0] << 24) | ((long) p[1] << 16)
		| ((long) p[2] << 8) | (long) p[3]);
}

int
be16(p)
unsigned char *p;
{
	return ((p[0] << 8) | p[1]);
}

/*
 * Read a variable-length quantity.  Advances *pp.  Returns -1 and leaves
 * *pp at the end on a malformed (runaway) VLQ.
 */
long
readvar(pp, end)
unsigned char **pp;
unsigned char *end;
{
	long v = 0L;
	int n = 0;
	int c;

	while (*pp < end) {
		c = *(*pp)++;
		v = (v << 7) | (c & 0x7f);
		if ((c & 0x80) == 0)
			return (v);
		if (++n >= 4)
			break;
	}
	*pp = end;
	return (-1L);
}

/* ------------------------------------------------------------------ */
/* emitting							      */
/* ------------------------------------------------------------------ */

flushout()
{
	int n;

	if (outused == 0)
		return (0);

	n = outused * 4;
	if (write(outfd, (char *) outbuf, n) != n) {
		if (interrupted)
			return (-1);
		perror("write");
		return (-1);
	}
	bytes_out += outused;
	outused = 0;
	return (0);
}

/*
 * Emit one MIDI byte scheduled at absolute time at_ms.  The packet's
 * delta is the pause since the previous byte, so the bytes of a single
 * message after the first all carry 0.
 */
emit(byte, at_ms)
int byte;
long at_ms;
{
	long delta;

	delta = at_ms - last_ms;
	if (delta < 0L)
		delta = 0L;
	if (delta > MAXDELTA)
		delta = MAXDELTA;	/* a >4.6h gap; corrupt file */
	last_ms += delta;

	outbuf[outused * 4 + 0] = byte & 0xff;
	outbuf[outused * 4 + 1] = delta & 0xff;
	outbuf[outused * 4 + 2] = (delta >> 8) & 0xff;
	outbuf[outused * 4 + 3] = (delta >> 16) & 0xff;
	outused++;

	if (outused >= OUTPKTS)
		return (flushout());
	return (0);
}

emitmsg(buf, n, at_ms)
unsigned char *buf;
int n;
long at_ms;
{
	int i;

	if (listonly) {
		printf("%8ld ", at_ms);
		for (i = 0; i < n; i++)
			printf("%02x ", buf[i]);
		printf("\n");
		return (0);
	}

	for (i = 0; i < n; i++)
		if (emit(buf[i], at_ms) < 0)
			return (-1);
	return (0);
}

/* ------------------------------------------------------------------ */
/* time								      */
/* ------------------------------------------------------------------ */

settempo(us_per_qn)
long us_per_qn;
{
	if (smpte)
		return (0);		/* SMPTE files ignore tempo */
	tconv_num = us_per_qn;
	return (0);
}

/*
 * Walk cur_tick forward to target, accumulating abs_ms.  Chunked so the
 * intermediate products cannot overflow a 32-bit long even for a very
 * slow tempo combined with a coarse division.
 */
advance_to(target)
long target;
{
	long n, chunk, whole, rem;

	whole = tconv_num / tconv_den;
	rem = tconv_num % tconv_den;

	chunk = 2000000000L / (whole + 1L);
	if (chunk > 4096L)
		chunk = 4096L;
	if (chunk < 1L)
		chunk = 1L;

	while (cur_tick < target) {
		n = target - cur_tick;
		if (n > chunk)
			n = chunk;

		us_acc += n * whole;
		frac += n * rem;
		us_acc += frac / tconv_den;
		frac %= tconv_den;
		abs_ms += us_acc / 1000L;
		us_acc %= 1000L;

		cur_tick += n;
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* MT-32 setup							      */
/* ------------------------------------------------------------------ */

/*
 * Reassign the MT-32's eight melodic parts to MIDI channels 1-8 and the
 * rhythm part to channel 10, so a General MIDI file addresses them the
 * way it expects.  System Area address 10 00 0D, nine bytes, one per
 * part, 0-based channel numbers.
 */
mt32_setup()
{
	unsigned char sx[32];
	int n, i, sum;
	static unsigned char chans[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 9 };

	n = 0;
	sx[n++] = 0xf0;
	sx[n++] = 0x41;		/* Roland		*/
	sx[n++] = 0x10;		/* device id		*/
	sx[n++] = 0x16;		/* model: MT-32		*/
	sx[n++] = 0x12;		/* command: DT1 (send)	*/
	sx[n++] = 0x10;		/* address		*/
	sx[n++] = 0x00;
	sx[n++] = 0x0d;
	for (i = 0; i < 9; i++)
		sx[n++] = chans[i];

	/* checksum over address + data */
	sum = 0;
	for (i = 5; i < n; i++)
		sum += sx[i];
	sx[n++] = (0x80 - (sum & 0x7f)) & 0x7f;
	sx[n++] = 0xf7;

	if (emitmsg(sx, n, 0L) < 0)
		return (-1);

	/* the MT-32 needs a moment to digest a System Area write before
	 * it will respond to notes */
	time_bias = 100L;
	return (0);
}

/* ------------------------------------------------------------------ */
/* cleanup							      */
/* ------------------------------------------------------------------ */

allnotesoff()
{
	unsigned char m[3];
	int ch;
	long endtime;

	/* not last_ms: in -l mode nothing goes through emit(), so last_ms
	 * never advances and the sweep would be logged at time 0 */
	endtime = abs_ms + time_bias;

	for (ch = 0; ch < 16; ch++) {
		m[0] = 0xb0 | ch;
		m[1] = 0x7b;		/* all notes off	*/
		m[2] = 0x00;
		if (emitmsg(m, 3, endtime) < 0)
			return (-1);
		m[1] = 0x78;		/* all sound off	*/
		if (emitmsg(m, 3, endtime) < 0)
			return (-1);
		m[1] = 0x79;		/* reset controllers	*/
		if (emitmsg(m, 3, endtime) < 0)
			return (-1);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* file loading and chunk splitting				      */
/* ------------------------------------------------------------------ */

readfile(name)
char *name;
{
	int fd;
	long got, n;

	fd = open(name, O_RDONLY);
	if (fd == -1) {
		perror(name);
		exit(1);
	}
	filelen = lseek(fd, 0L, 2);
	if (filelen <= 0L)
		die("empty or unseekable input file");
	lseek(fd, 0L, 0);

	filebuf = (unsigned char *) malloc((unsigned) filelen);
	if (filebuf == 0)
		die("out of memory reading file (try a smaller .mid)");

	got = 0L;
	while (got < filelen) {
		n = read(fd, (char *) filebuf + got, (int) (filelen - got));
		if (n <= 0L)
			die("short read on input file");
		got += n;
	}
	close(fd);
	return (0);
}

/*
 * Split the file into MThd + MTrk chunks.  Chunk lengths are used to walk
 * from one chunk to the next, but a track's parse is also bounded by the
 * end of the file, so a bad length truncates that track instead of
 * running off into whatever follows.
 */
int
parsechunks()
{
	unsigned char *p, *fend;
	long len;
	int format, ntrks_hdr, div;

	p = filebuf;
	fend = filebuf + filelen;

	if (filelen < 14L || strncmp((char *) p, "MThd", 4) != 0)
		die("not a Standard MIDI File (no MThd)");

	len = be32(p + 4);
	if (len < 6L)
		die("bad MThd length");

	format = be16(p + 8);
	ntrks_hdr = be16(p + 10);
	div = be16(p + 12);

	if (div & 0x8000) {
		/* SMPTE: high byte is negative frames/sec, low byte is
		 * ticks per frame.  Rate is fixed; tempo events do not
		 * apply. */
		int fps = 256 - ((div >> 8) & 0xff);
		int sub = div & 0xff;

		if (fps <= 0 || sub <= 0)
			die("bad SMPTE division in MThd");
		smpte = 1;
		tconv_num = 1000000L;
		tconv_den = (long) fps * (long) sub;
	} else {
		if (div == 0)
			die("bad division in MThd");
		tconv_den = div;
		tconv_num = 500000L;
	}

	if (format == 2)
		fprintf(stderr,
			"midifile: warning: format 2 file, playing tracks merged\n");

	p += 8 + len;		/* skip the header chunk */

	ntrk = 0;
	while (p + 8 <= fend) {
		len = be32(p + 4);

		if (strncmp((char *) p, "MTrk", 4) != 0) {
			/* unknown chunk type: the spec says skip it */
			if (len < 0L || p + 8 + len > fend)
				break;
			p += 8 + len;
			continue;
		}

		if (ntrk >= MAXTRK) {
			fprintf(stderr,
				"midifile: warning: more than %d tracks, ignoring the rest\n",
				MAXTRK);
			break;
		}

		trk[ntrk].p = p + 8;
		trk[ntrk].end = p + 8 + len;
		if (trk[ntrk].end > fend || len < 0L)
			trk[ntrk].end = fend;	/* bad length; clamp */
		trk[ntrk].running = 0;
		trk[ntrk].done = 0;
		trk[ntrk].tick = 0L;
		ntrk++;

		if (len < 0L || p + 8 + len > fend)
			break;
		p += 8 + len;
	}

	if (ntrk == 0)
		die("no MTrk chunks found");

	if (!quiet)
		printf("format %d, %d track%s (%d in header), %ld %s\n",
		       format, ntrk, ntrk == 1 ? "" : "s", ntrks_hdr,
		       tconv_den, smpte ? "ticks/sec" : "ticks/quarter");

	return (format);
}

/*
 * Read the delta time of a track's next event.  Marks the track done at
 * end of chunk or on a malformed delta.
 */
nextdelta(t)
struct track *t;
{
	long d;

	if (t->p >= t->end) {
		t->done = 1;
		return (0);
	}
	d = readvar(&t->p, t->end);
	if (d < 0L) {
		t->done = 1;
		return (0);
	}
	t->tick += d;
	return (0);
}

/* ------------------------------------------------------------------ */
/* the event loop						      */
/* ------------------------------------------------------------------ */

/*
 * Parse and emit one event from track t, which is scheduled at time
 * at_ms.  Returns -1 on write error.
 */
int
doevent(t, at_ms)
struct track *t;
long at_ms;
{
	int status, type, ch, n;
	long len;
	unsigned char msg[3];
	unsigned char *dp;

	if (t->p >= t->end) {
		t->done = 1;
		return (0);
	}

	status = *t->p;

	if (status < 0x80) {
		/* running status: reuse the previous status byte and take
		 * this byte as the first data byte */
		if (t->running == 0) {
			t->done = 1;	/* data with no status; desynced */
			return (0);
		}
		status = t->running;
	} else {
		t->p++;
		if (status < 0xf0)
			t->running = status;
		else
			t->running = 0;	/* sysex/meta clear running status */
	}

	if (status == 0xff) {
		/* meta event: consumed here, never sent to the wire */
		if (t->p >= t->end) {
			t->done = 1;
			return (0);
		}
		type = *t->p++;
		len = readvar(&t->p, t->end);
		if (len < 0L || t->p + len > t->end) {
			t->done = 1;
			return (0);
		}
		if (type == 0x51 && len == 3L)
			settempo(((long) t->p[0] << 16)
				 | ((long) t->p[1] << 8) | (long) t->p[2]);
		else if (type == 0x2f)
			t->done = 1;
		t->p += len;
		return (0);
	}

	if (status == 0xf0 || status == 0xf7) {
		/* sysex.  F0 carries its own leading F0; F7 is an escape
		 * or continuation and its bytes go out verbatim. */
		len = readvar(&t->p, t->end);
		if (len < 0L || t->p + len > t->end) {
			t->done = 1;
			return (0);
		}
		dp = t->p;
		t->p += len;

		if (status == 0xf0) {
			msg[0] = 0xf0;
			if (emitmsg(msg, 1, at_ms) < 0)
				return (-1);
		}
		if (emitmsg(dp, (int) len, at_ms) < 0)
			return (-1);
		return (0);
	}

	/* channel voice / system common */
	ch = status & 0x0f;

	switch (status & 0xf0) {
	case 0xc0:		/* program change	*/
	case 0xd0:		/* channel pressure	*/
		n = 1;
		break;
	case 0xf0:		/* system common	*/
		if (status == 0xf2)
			n = 2;
		else if (status == 0xf1 || status == 0xf3)
			n = 1;
		else
			n = 0;
		break;
	default:
		n = 2;
		break;
	}

	if (t->p + n > t->end) {
		t->done = 1;
		return (0);
	}

	msg[0] = status;
	if (n > 0)
		msg[1] = *t->p++;
	if (n > 1)
		msg[2] = *t->p++;

	/* GM drums do not survive translation to the MT-32 rhythm map */
	if (dropdrums && status < 0xf0 && ch == 9)
		return (0);

	if (mt32mode && (status & 0xf0) == 0xc0 && ch != 9)
		msg[1] = gm2mt32[msg[1] & 0x7f];

	return (emitmsg(msg, n + 1, at_ms));
}

play()
{
	int i, best;
	long besttick, at_ms;
	long lastreport = -1L;

	/* prime every track with its first delta */
	for (i = 0; i < ntrk; i++)
		nextdelta(&trk[i]);

	for (;;) {
		if (interrupted)
			break;

		/* merge: the earliest pending event across all tracks,
		 * ties broken by track order so a file's own ordering of
		 * simultaneous events is preserved */
		best = -1;
		besttick = 0L;
		for (i = 0; i < ntrk; i++) {
			if (trk[i].done)
				continue;
			if (best < 0 || trk[i].tick < besttick) {
				best = i;
				besttick = trk[i].tick;
			}
		}
		if (best < 0)
			break;

		advance_to(besttick);
		at_ms = abs_ms + time_bias;

		if (!quiet && !listonly && abs_ms / 5000L != lastreport) {
			lastreport = abs_ms / 5000L;
			printf("\r%ld:%02ld ", abs_ms / 60000L,
			       (abs_ms / 1000L) % 60L);
			fflush(stdout);
		}

		if (doevent(&trk[best], at_ms) < 0)
			return (-1);

		nextdelta(&trk[best]);
	}

	return (0);
}

/* ------------------------------------------------------------------ */
/* the GM -> MT-32 map file					      */
/* ------------------------------------------------------------------ */

loadmap(name)
char *name;
{
	FILE *f;
	int c, v, n;

	f = fopen(name, "r");
	if (f == NULL) {
		perror(name);
		exit(1);
	}

	n = 0;
	while (n < 128) {
		c = getc(f);
		if (c == EOF)
			break;
		if (c == '#') {
			while (c != EOF && c != '\n')
				c = getc(f);
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		ungetc(c, f);
		if (fscanf(f, "%d", &v) != 1)
			break;
		gm2mt32[n++] = v & 0x7f;
	}
	fclose(f);

	if (n != 128)
		fprintf(stderr,
			"midifile: warning: %s had %d entries, expected 128\n",
			name, n);
	return (0);
}

/* ------------------------------------------------------------------ */

usage()
{
	fprintf(stderr,
		"usage: midifile [-c] [-d] [-m map] [-l] [-o out] [-q] file.mid\n");
	exit(1);
}

main(argc, argv)
int argc;
char **argv;
{
	int i;
	char *fname = 0;
	char *mapname = 0;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-' || argv[i][1] == '\0') {
			if (fname)
				usage();
			fname = argv[i];
			continue;
		}
		switch (argv[i][1]) {
		case 'c':
			mt32mode = 1;
			break;
		case 'd':
			dropdrums = 1;
			break;
		case 'l':
			listonly = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'm':
			if (++i >= argc)
				usage();
			mapname = argv[i];
			break;
		case 'o':
			if (++i >= argc)
				usage();
			outname = argv[i];
			break;
		default:
			usage();
		}
	}

	if (fname == 0)
		usage();
	if (mapname)
		loadmap(mapname);

	readfile(fname);
	parsechunks();

	if (!listonly) {
		if (outname) {
			outfd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (outfd == -1) {
				perror(outname);
				exit(1);
			}
		} else {
			outfd = open("/dev/sbmidi", O_WRONLY);
			if (outfd == -1) {
				perror("open /dev/sbmidi");
				exit(1);
			}
		}
		signal(SIGINT, onintr);
	}

	if (mt32mode)
		mt32_setup();

	play();

	/* always sweep the notes off, including after ^C */
	interrupted = 0;
	allnotesoff();
	flushout();

	if (!listonly)
		close(outfd);

	if (!quiet && !listonly)
		printf("\r%ld:%02ld  %ld MIDI bytes\n",
		       abs_ms / 60000L, (abs_ms / 1000L) % 60L, bytes_out);
	if (!quiet)
		printf("done\n");
	exit(0);
}
