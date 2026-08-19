/*
 * play_mid_fm.c - play a Standard MIDI File (.mid) as FM synthesis on the
 * SoundBlaster driver's OPL2 chip, via /dev/sbfm.
 *
 * This is the FM-synth counterpart to play_mid.c.  play_mid.c turns an SMF
 * into a stream of raw MIDI bytes for /dev/sbmidi, and the driver paces
 * that stream itself in the kernel against lbolt.  /dev/sbfm works
 * differently: sbwrite() returns ENXIO for the FM minor (see sb.c), and the
 * only interface is ioctl() - FM_IOCTL_NOTE_ON/OFF, FM_IOCTL_SET_VOICE,
 * FM_IOCTL_RESET.  There is no kernel-side pacing for it at all, so this
 * program has to do its own real-time scheduling: parse and merge the
 * tracks exactly like play_mid.c does, run the tempo map to get each
 * event's absolute millisecond, and actually sleep() between events while
 * we drive the chip directly, the same way play_cmf.c and play_instr.c do
 * with high_res_sleep().
 *
 * The OPL2 only has MAX_FM_NOTES (9) hardware voices, so General MIDI's up
 * to 16-channel polyphony is funneled through a 9-voice pool shared across
 * all channels, stealing the oldest-triggered voice when all 9 are busy.
 * Channel 10 (index 9, GM percussion) does not get the OPL rhythm-mode
 * registers - that would need FM_IOCTL_SET_RHYTHM and a different keyon
 * path this driver's fm_key_on()/fm_key_off() don't take - so every
 * percussion note instead borrows one voice slot and plays a single
 * generic percussive patch, pitched by note number like any other voice.
 * That is a crude stand-in, not a drum kit; -d drops channel 10 entirely
 * if that sounds worse than nothing.
 *
 * Instrument patches are General MIDI program -> coarse "family" -> 16-byte
 * FM_IOCTL_SET_VOICE data, the same style of lossy many-to-one map
 * play_mid.c's -c mode uses for GM -> MT-32 timbres (gm2mt32[]).  These are
 * hand-built OPL2 2-operator patches, not a transcription of any
 * commercial AdLib/GENMIDI instrument bank.  The per-voice byte layout
 * (modulator/carrier char, KSL/TL, AR/DR, SL/RR, waveform, feedback/algo)
 * is exactly what get_instr.c documents for CMF instrument blocks and what
 * fm_set_voice() in sb.c writes to the chip; instrument #0's bytes below
 * are lifted directly from tst_fm_note.c's example.  -b lets you replace
 * the whole 128-program table with a raw file of 128 * 16 bytes in that
 * same layout, one program per row, if you build a better one by ear.
 *
 * Not modeled at all: pitch bend, sustain pedal, poly/channel pressure,
 * sysex.  Channel volume (CC 7) and expression (CC 11) scale the carrier's
 * output level alongside note velocity; CC 120/123 (all sound/notes off)
 * are honored per channel.
 *
 * Usage:
 *     play_mid_fm [-d] [-q] [-l] [-b bankfile] file.mid
 *
 *     -d          Drop channel 10 (percussion) instead of playing it
 *                 through the generic percussive patch.
 *     -q          Quiet: no progress output.
 *     -l          Dry run: parse and walk the file at full speed without
 *                 opening /dev/sbfm or sleeping.  Useful for checking the
 *                 parse off the target machine.
 *     -b bankfile Replace the built-in family-based patch table with a raw
 *                 128 * 16 byte file (one FM_IOCTL_SET_VOICE-format
 *                 instrument per GM program, in program order).
 *
 * Compile (on the Xenix box, once /usr/include/sys/sb.h is installed):
 *     cc -M3 -o play_mid_fm play_mid_fm.c -lm
 *
 * Interrupting: SIGINT breaks the event loop and falls through to the same
 * cleanup as normal exit - every voice gets an explicit NOTE_OFF.  Closing
 * /dev/sbfm also triggers the driver's own fm_close()/fm_reset(), which
 * sweeps all 9 voices again, but we do not rely on that alone since a
 * stuck note should go silent the moment ^C is noticed, not whenever the
 * process happens to get around to close().
 */

#include <sys/fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/times.h>
#include <sys/sb.h>

extern char *malloc();
extern long lseek();
extern double pow();

#define MAXTRK 64
#define MAXDELTA 0xffffffL	/* 24-bit-ish sanity clamp, ~4.6 hours */

/* ------------------------------------------------------------------ */
/* options							      */
/* ------------------------------------------------------------------ */

int dropdrums = 0;
int listonly = 0;
int quiet = 0;

/* ------------------------------------------------------------------ */
/* input file / track cursors - same shape as play_mid.c		      */
/* ------------------------------------------------------------------ */

unsigned char *filebuf;
long filelen;

struct track {
	unsigned char *p;
	unsigned char *end;
	long tick;
	int running;
	int done;
};

struct track trk[MAXTRK];
int ntrk;

/* ------------------------------------------------------------------ */
/* tick -> millisecond conversion - identical to play_mid.c		      */
/* ------------------------------------------------------------------ */

long tconv_num = 500000L;
long tconv_den = 480L;
int smpte = 0;

long cur_tick = 0L;
long abs_ms = 0L;
long us_acc = 0L;
long frac = 0L;

long last_played_ms = 0L;
int interrupted = 0;

/* ------------------------------------------------------------------ */
/* FM voice pool							      */
/* ------------------------------------------------------------------ */

#define DRUM_CHAN 9

struct fmvoice {
	int active;
	int chan;
	int note;
	long age;
};

struct fmvoice voices[MAX_FM_NOTES];
long voice_clock = 0L;

int chan_prog[16];		/* GM program number per channel	*/
int chan_vol[16];		/* last CC7 (volume) or CC11 (expression) seen, 0-127 */

int fm_fd = -1;

/* ------------------------------------------------------------------ */
/* the patch table							      */
/* ------------------------------------------------------------------ */

/*
 * Family indices.  Coarser than General MIDI's 128 programs on purpose -
 * see the file header.
 */
#define FAM_PIANO     0
#define FAM_CHROMPERC 1
#define FAM_ORGAN     2
#define FAM_GUITAR    3
#define FAM_BASS      4
#define FAM_STRINGS   5
#define FAM_ENSEMBLE  6
#define FAM_BRASS     7
#define FAM_REED      8
#define FAM_PIPE      9
#define FAM_SYNLEAD   10
#define FAM_SYNPAD    11
#define FAM_SYNFX     12
#define FAM_DRUM      13
#define NFAM          14

/*
 * Sixteen bytes per patch: [0] modulator char, [1] carrier char,
 * [2] modulator KSL/TL, [3] carrier KSL/TL, [4] modulator AR/DR,
 * [5] carrier AR/DR, [6] modulator SL/RR, [7] carrier SL/RR,
 * [8] modulator wave select, [9] carrier wave select,
 * [10] feedback/connection, [11-15] unused (fm_set_voice only reads 0-10).
 * This is exactly the layout fm_set_voice() in sb.c writes out register
 * by register, and what get_instr.c decodes from a CMF instrument block.
 */
unsigned char fam_patch[NFAM][16] = {
/* FAM_PIANO      */ { 0x11, 0x01, 0x8a, 0x40, 0xf1, 0xf1, 0x11, 0xb3,
			0x00, 0x00, 0x06, 0, 0, 0, 0, 0 },
/* FAM_CHROMPERC  */ { 0x0e, 0x02, 0x9c, 0x00, 0xf6, 0xf3, 0x25, 0x18,
			0x02, 0x00, 0x08, 0, 0, 0, 0, 0 },
/* FAM_ORGAN      */ { 0x63, 0x21, 0x1e, 0x0c, 0xf3, 0xf3, 0x33, 0x33,
			0x00, 0x00, 0x0e, 0, 0, 0, 0, 0 },
/* FAM_GUITAR     */ { 0x33, 0x11, 0x1a, 0x00, 0xf5, 0xf6, 0x14, 0x36,
			0x00, 0x00, 0x0a, 0, 0, 0, 0, 0 },
/* FAM_BASS       */ { 0x21, 0x21, 0x14, 0x00, 0xf6, 0xf6, 0x22, 0x25,
			0x00, 0x00, 0x0c, 0, 0, 0, 0, 0 },
/* FAM_STRINGS    */ { 0xa2, 0x61, 0x1c, 0x08, 0x74, 0x63, 0x23, 0x23,
			0x00, 0x00, 0x02, 0, 0, 0, 0, 0 },
/* FAM_ENSEMBLE   */ { 0xa1, 0x61, 0x2c, 0x18, 0x53, 0x52, 0x23, 0x24,
			0x01, 0x01, 0x00, 0, 0, 0, 0, 0 },
/* FAM_BRASS      */ { 0x31, 0x21, 0x1a, 0x00, 0xa6, 0xa6, 0x43, 0x43,
			0x00, 0x00, 0x0a, 0, 0, 0, 0, 0 },
/* FAM_REED       */ { 0x32, 0x21, 0x1e, 0x06, 0x8a, 0x87, 0x35, 0x35,
			0x01, 0x01, 0x0a, 0, 0, 0, 0, 0 },
/* FAM_PIPE       */ { 0x21, 0x21, 0x1a, 0x00, 0x92, 0x92, 0x33, 0x33,
			0x00, 0x00, 0x00, 0, 0, 0, 0, 0 },
/* FAM_SYNLEAD    */ { 0x21, 0x21, 0x00, 0x00, 0xf6, 0xf3, 0x25, 0x28,
			0x03, 0x03, 0x04, 0, 0, 0, 0, 0 },
/* FAM_SYNPAD     */ { 0x71, 0x72, 0x2c, 0x18, 0x54, 0x54, 0x22, 0x22,
			0x02, 0x02, 0x00, 0, 0, 0, 0, 0 },
/* FAM_SYNFX      */ { 0x22, 0x21, 0x1c, 0x00, 0xf3, 0xa4, 0x34, 0x35,
			0x02, 0x03, 0x06, 0, 0, 0, 0, 0 },
/* FAM_DRUM       */ { 0x0f, 0x0f, 0x00, 0x00, 0xf8, 0xf8, 0x2a, 0x2a,
			0x03, 0x03, 0x0e, 0, 0, 0, 0, 0 }
};

/*
 * GM program -> family, grouped the same way gm2mt32[] is in play_mid.c:
 * coarse, by family, tune by ear or override wholesale with -b.
 */
unsigned char gm2fam[128] = {
	/* 0-7    piano			 */ FAM_PIANO, FAM_PIANO, FAM_PIANO, FAM_PIANO,
					   FAM_PIANO, FAM_PIANO, FAM_PIANO, FAM_PIANO,
	/* 8-15   chromatic percussion	 */ FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC,
					   FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC,
	/* 16-23  organ			 */ FAM_ORGAN, FAM_ORGAN, FAM_ORGAN, FAM_ORGAN,
					   FAM_ORGAN, FAM_ORGAN, FAM_ORGAN, FAM_ORGAN,
	/* 24-31  guitar		 */ FAM_GUITAR, FAM_GUITAR, FAM_GUITAR, FAM_GUITAR,
					   FAM_GUITAR, FAM_GUITAR, FAM_GUITAR, FAM_GUITAR,
	/* 32-39  bass			 */ FAM_BASS, FAM_BASS, FAM_BASS, FAM_BASS,
					   FAM_BASS, FAM_BASS, FAM_BASS, FAM_BASS,
	/* 40-47  strings		 */ FAM_STRINGS, FAM_STRINGS, FAM_STRINGS, FAM_STRINGS,
					   FAM_STRINGS, FAM_STRINGS, FAM_STRINGS, FAM_STRINGS,
	/* 48-55  ensemble		 */ FAM_ENSEMBLE, FAM_ENSEMBLE, FAM_ENSEMBLE, FAM_ENSEMBLE,
					   FAM_ENSEMBLE, FAM_ENSEMBLE, FAM_ENSEMBLE, FAM_ENSEMBLE,
	/* 56-63  brass			 */ FAM_BRASS, FAM_BRASS, FAM_BRASS, FAM_BRASS,
					   FAM_BRASS, FAM_BRASS, FAM_BRASS, FAM_BRASS,
	/* 64-71  reed			 */ FAM_REED, FAM_REED, FAM_REED, FAM_REED,
					   FAM_REED, FAM_REED, FAM_REED, FAM_REED,
	/* 72-79  pipe			 */ FAM_PIPE, FAM_PIPE, FAM_PIPE, FAM_PIPE,
					   FAM_PIPE, FAM_PIPE, FAM_PIPE, FAM_PIPE,
	/* 80-87  synth lead		 */ FAM_SYNLEAD, FAM_SYNLEAD, FAM_SYNLEAD, FAM_SYNLEAD,
					   FAM_SYNLEAD, FAM_SYNLEAD, FAM_SYNLEAD, FAM_SYNLEAD,
	/* 88-95  synth pad		 */ FAM_SYNPAD, FAM_SYNPAD, FAM_SYNPAD, FAM_SYNPAD,
					   FAM_SYNPAD, FAM_SYNPAD, FAM_SYNPAD, FAM_SYNPAD,
	/* 96-103 synth effects		 */ FAM_SYNFX, FAM_SYNFX, FAM_SYNFX, FAM_SYNFX,
					   FAM_SYNFX, FAM_SYNFX, FAM_SYNFX, FAM_SYNFX,
	/* 104-111 ethnic		 */ FAM_GUITAR, FAM_GUITAR, FAM_GUITAR, FAM_GUITAR,
					   FAM_GUITAR, FAM_GUITAR, FAM_GUITAR, FAM_GUITAR,
	/* 112-119 percussive		 */ FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC,
					   FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC, FAM_CHROMPERC,
	/* 120-127 sound effects	 */ FAM_SYNFX, FAM_SYNFX, FAM_SYNFX, FAM_SYNFX,
					   FAM_SYNFX, FAM_SYNFX, FAM_SYNFX, FAM_SYNFX
};

unsigned char prog_patch[128][16];
unsigned char drum_patch[16];

/* ------------------------------------------------------------------ */

die(msg)
char *msg;
{
	fprintf(stderr, "play_mid_fm: %s\n", msg);
	exit(1);
}

void
onintr()
{
	interrupted = 1;
	signal(SIGINT, onintr);
}

/* ------------------------------------------------------------------ */
/* big-endian readers / VLQ - identical to play_mid.c			      */
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
/* time - identical to play_mid.c					      */
/* ------------------------------------------------------------------ */

settempo(us_per_qn)
long us_per_qn;
{
	if (smpte)
		return (0);
	tconv_num = us_per_qn;
	return (0);
}

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
/* file loading and chunk splitting - identical to play_mid.c		      */
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
			"play_mid_fm: warning: format 2 file, playing tracks merged\n");

	p += 8 + len;

	ntrk = 0;
	while (p + 8 <= fend) {
		len = be32(p + 4);

		if (strncmp((char *) p, "MTrk", 4) != 0) {
			if (len < 0L || p + 8 + len > fend)
				break;
			p += 8 + len;
			continue;
		}

		if (ntrk >= MAXTRK) {
			fprintf(stderr,
				"play_mid_fm: warning: more than %d tracks, ignoring the rest\n",
				MAXTRK);
			break;
		}

		trk[ntrk].p = p + 8;
		trk[ntrk].end = p + 8 + len;
		if (trk[ntrk].end > fend || len < 0L)
			trk[ntrk].end = fend;
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
/* FM voice pool and instrument handling				      */
/* ------------------------------------------------------------------ */

build_default_bank()
{
	int p, i, fam;

	for (p = 0; p < 128; p++) {
		fam = gm2fam[p];
		for (i = 0; i < 16; i++)
			prog_patch[p][i] = fam_patch[fam][i];
	}
	for (i = 0; i < 16; i++)
		drum_patch[i] = fam_patch[FAM_DRUM][i];
}

loadbank(name)
char *name;
{
	int fd;
	long got, n;
	unsigned char buf[128 * 16];

	fd = open(name, O_RDONLY);
	if (fd == -1) {
		perror(name);
		exit(1);
	}

	got = 0L;
	while (got < 128L * 16L) {
		n = read(fd, (char *) buf + got, (int) (128L * 16L - got));
		if (n <= 0L)
			break;
		got += n;
	}
	close(fd);

	if (got != 128L * 16L) {
		fprintf(stderr,
			"play_mid_fm: warning: %s is %ld bytes, expected %ld; bank not loaded\n",
			name, got, 128L * 16L);
		return (0);
	}

	memcpy((char *) prog_patch, (char *) buf, 128 * 16);
	return (0);
}

/*
 * MIDI note -> OPL2 F-number/block.  Picks the smallest block (finest
 * frequency resolution) for which the F-number still fits in 10 bits, at
 * the chip's usual 49716Hz per-operator sample rate.
 */
freq_block(note, fnump, blockp)
int note;
int *fnump, *blockp;
{
	double freq;
	int b;
	long f;

	freq = 440.0 * pow(2.0, (note - 69) / 12.0);
	f = 1024L;

	for (b = 0; b <= 7; b++) {
		f = (long) (freq * (double) (1L << (20 - b)) / 49716.0 + 0.5);
		if (f < 1024L)
			break;
	}
	if (b > 7)
		b = 7;
	if (f > 1023L)
		f = 1023L;
	if (f < 0L)
		f = 0L;

	*blockp = b;
	*fnump = (int) f;
}

/*
 * Build the 16 SET_VOICE bytes for a note: the channel's program (or the
 * generic drum patch on channel 10), with the carrier's output level
 * attenuated by velocity and channel volume/expression together.
 */
build_patch(dst, ch, vel)
unsigned char *dst;
int ch, vel;
{
	unsigned char *src;
	int i, level, atten, car_tl;

	src = (ch == DRUM_CHAN) ? drum_patch : prog_patch[chan_prog[ch] & 0x7f];
	for (i = 0; i < 16; i++)
		dst[i] = src[i];

	level = (vel * chan_vol[ch]) / 127;
	if (level > 127)
		level = 127;
	atten = (127 - level) * 0x20 / 127;	/* 0..32 extra attenuation */

	car_tl = (dst[3] & 0x3f) + atten;
	if (car_tl > 0x3f)
		car_tl = 0x3f;
	dst[3] = (dst[3] & 0xc0) | car_tl;
}

load_voice(v, patch)
int v;
unsigned char *patch;
{
	sb_fm_character vc;
	int i;

	if (fm_fd < 0)
		return;

	vc.voice_num = v;
	for (i = 0; i < 16; i++)
		vc.data[i] = patch[i];

	ioctl(fm_fd, FM_IOCTL_SET_VOICE, (int) &vc);
}

/*
 * Pick a voice for a new note: any free one, or the least-recently-
 * triggered one across all 9, in which case its old note is cut off.
 */
int
alloc_voice()
{
	int i, best;
	long bestage;

	for (i = 0; i < MAX_FM_NOTES; i++)
		if (!voices[i].active)
			return (i);

	best = 0;
	bestage = voices[0].age;
	for (i = 1; i < MAX_FM_NOTES; i++)
		if (voices[i].age < bestage) {
			best = i;
			bestage = voices[i].age;
		}

	if (fm_fd >= 0)
		ioctl(fm_fd, FM_IOCTL_NOTE_OFF, best);
	voices[best].active = 0;
	return (best);
}

note_on(ch, note, vel)
int ch, note, vel;
{
	int v, fnum, block;
	unsigned char patch[16];
	sb_fm_note keynote;

	if (vel == 0) {
		note_off(ch, note);
		return;
	}

	v = alloc_voice();

	build_patch(patch, ch, vel);
	load_voice(v, patch);

	freq_block(note, &fnum, &block);

	note_num(keynote) = v;
	fnum_low(keynote) = fnum & 0xff;
	keyon_blk_fnum(keynote) = 0;
	keyon_blk_fnum(keynote) |= 1 << 5;
	keyon_blk_fnum(keynote) |= (block & 7) << 2;
	keyon_blk_fnum(keynote) |= (fnum >> 8) & 3;

	if (fm_fd >= 0)
		ioctl(fm_fd, FM_IOCTL_NOTE_ON, keynote);

	voices[v].active = 1;
	voices[v].chan = ch;
	voices[v].note = note;
	voices[v].age = ++voice_clock;
}

note_off(ch, note)
int ch, note;
{
	int i;

	for (i = 0; i < MAX_FM_NOTES; i++) {
		if (voices[i].active && voices[i].chan == ch
		    && voices[i].note == note) {
			if (fm_fd >= 0)
				ioctl(fm_fd, FM_IOCTL_NOTE_OFF, i);
			voices[i].active = 0;
			return;
		}
	}
}

chan_notes_off(ch)
int ch;
{
	int i;

	for (i = 0; i < MAX_FM_NOTES; i++) {
		if (voices[i].active && voices[i].chan == ch) {
			if (fm_fd >= 0)
				ioctl(fm_fd, FM_IOCTL_NOTE_OFF, i);
			voices[i].active = 0;
		}
	}
}

all_notes_off()
{
	int i;

	for (i = 0; i < MAX_FM_NOTES; i++) {
		if (fm_fd >= 0)
			ioctl(fm_fd, FM_IOCTL_NOTE_OFF, i);
		voices[i].active = 0;
	}
}

/* ------------------------------------------------------------------ */
/* one event, dispatched straight to the FM voice pool (or discarded) */
/* ------------------------------------------------------------------ */

doevent_fm(t)
struct track *t;
{
	int status, type, ch, n;
	long len;
	unsigned char d0, d1;

	if (t->p >= t->end) {
		t->done = 1;
		return (0);
	}

	status = *t->p;

	if (status < 0x80) {
		if (t->running == 0) {
			t->done = 1;
			return (0);
		}
		status = t->running;
	} else {
		t->p++;
		if (status < 0xf0)
			t->running = status;
		else
			t->running = 0;
	}

	if (status == 0xff) {
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
		/* sysex: nothing an OPL2 can act on */
		len = readvar(&t->p, t->end);
		if (len < 0L || t->p + len > t->end) {
			t->done = 1;
			return (0);
		}
		t->p += len;
		return (0);
	}

	ch = status & 0x0f;

	if ((status & 0xf0) == 0xc0) {
		if (t->p >= t->end) {
			t->done = 1;
			return (0);
		}
		chan_prog[ch] = *t->p++ & 0x7f;
		return (0);
	}

	if ((status & 0xf0) == 0xd0) {
		if (t->p >= t->end) {
			t->done = 1;
			return (0);
		}
		t->p++;		/* channel pressure: not modeled */
		return (0);
	}

	if ((status & 0xf0) == 0xf0) {
		if (status == 0xf2)
			n = 2;
		else if (status == 0xf1 || status == 0xf3)
			n = 1;
		else
			n = 0;
		if (t->p + n > t->end) {
			t->done = 1;
			return (0);
		}
		t->p += n;
		return (0);
	}

	/* everything left needs exactly two data bytes: 0x80,0x90,0xa0,0xb0,0xe0 */
	if (t->p + 2 > t->end) {
		t->done = 1;
		return (0);
	}
	d0 = *t->p++;
	d1 = *t->p++;

	if (dropdrums && ch == DRUM_CHAN)
		return (0);

	switch (status & 0xf0) {
	case 0x80:
		note_off(ch, d0 & 0x7f);
		break;
	case 0x90:
		note_on(ch, d0 & 0x7f, d1 & 0x7f);
		break;
	case 0xb0:
		if (d0 == 7 || d0 == 11)	/* channel volume / expression */
			chan_vol[ch] = d1 & 0x7f;
		else if (d0 == 120 || d0 == 123)	/* all sound/notes off */
			chan_notes_off(ch);
		break;
	/* 0xa0 poly pressure, 0xe0 pitch bend: not modeled */
	}

	return (0);
}

/* ------------------------------------------------------------------ */
/* real-time playback loop						      */
/* ------------------------------------------------------------------ */

#include <sys/poll.h>

high_res_sleep(ms)
long ms;
{
	struct pollfd p;

	if (ms <= 0L)
		return;
	poll(&p, 0, (int) ms);
}

play()
{
	int i, best;
	long besttick, delta;
	long lastreport = -1L;
	struct tms tbuf;
	long start_ticks = 0L, target_ticks, now_ticks;

	for (i = 0; i < ntrk; i++)
		nextdelta(&trk[i]);

	if (!listonly)
		start_ticks = times(&tbuf);

	for (;;) {
		if (interrupted)
			break;

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

		/*
		 * Pace against real elapsed time (times(), HZ resolution),
		 * not against the sum of previous nominal deltas: any
		 * per-event overhead (ioctl round-trips, poll()'s own tick
		 * quantization, the periodic progress printf) would
		 * otherwise never get subtracted back, so it accumulates
		 * across thousands of events into a large, one-directional
		 * slowdown. Sleeping only the deficit between score time and
		 * actual elapsed time - and skipping the sleep entirely when
		 * already behind - bounds the drift to about one tick (20ms)
		 * instead of letting it compound.
		 */
		if (!listonly) {
			target_ticks = (abs_ms * HZ) / 1000L;
			now_ticks = times(&tbuf) - start_ticks;
			if (target_ticks > now_ticks) {
				delta = ((target_ticks - now_ticks) * 1000L) / HZ;
				if (delta > MAXDELTA)
					delta = MAXDELTA;
				if (delta > 0L)
					high_res_sleep(delta);
			}
			last_played_ms = abs_ms;
		}

		if (!quiet && !listonly && abs_ms / 5000L != lastreport) {
			lastreport = abs_ms / 5000L;
			printf("\r%ld:%02ld ", abs_ms / 60000L,
			       (abs_ms / 1000L) % 60L);
			fflush(stdout);
		}

		doevent_fm(&trk[best]);

		nextdelta(&trk[best]);
	}

	return (0);
}

/* ------------------------------------------------------------------ */

usage()
{
	fprintf(stderr,
		"usage: play_mid_fm [-d] [-q] [-l] [-b bankfile] file.mid\n");
	exit(1);
}

main(argc, argv)
int argc;
char **argv;
{
	int i;
	char *fname = 0;
	char *bankname = 0;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-' || argv[i][1] == '\0') {
			if (fname)
				usage();
			fname = argv[i];
			continue;
		}
		switch (argv[i][1]) {
		case 'd':
			dropdrums = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'l':
			listonly = 1;
			break;
		case 'b':
			if (++i >= argc)
				usage();
			bankname = argv[i];
			break;
		default:
			usage();
		}
	}

	if (fname == 0)
		usage();

	build_default_bank();
	if (bankname)
		loadbank(bankname);

	readfile(fname);
	parsechunks();

	for (i = 0; i < 16; i++) {
		chan_prog[i] = 0;
		chan_vol[i] = 127;
	}
	for (i = 0; i < MAX_FM_NOTES; i++)
		voices[i].active = 0;

	if (!listonly) {
		fm_fd = open("/dev/sbfm", O_WRONLY);
		if (fm_fd == -1) {
			perror("open /dev/sbfm");
			exit(1);
		}
		ioctl(fm_fd, FM_IOCTL_RESET);
		signal(SIGINT, onintr);
	}

	play();

	interrupted = 0;
	all_notes_off();

	if (fm_fd >= 0)
		close(fm_fd);

	if (!quiet)
		printf("\r%ld:%02ld  done\n", abs_ms / 60000L,
		       (abs_ms / 1000L) % 60L);

	exit(0);
}
