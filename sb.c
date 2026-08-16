#define DPR(X)	/* printf X */
#define RICK	1

/* XXX make dsp_reset use sleep */
/*
 * Copyrighted as an unpublished work.
 * (c) Copyright 1991 Brian Smith
 * All rights reserved.
 *
 * Read the LICENSE file for details on distribution and use.
 *
 *
 * Substantial changes by Pace Willisson, January 1992
 * Merge my assembler handler stuff into Pace's version, March 1992
 *	- Lance Norskog
 *
 * Updated for SCO Xenix by Lorenzo Gatti, August 2026 (programming by Claude)
 *
 */

/* Notes:
 *
 * 1. never crash the kernel, no matter what sequence of system calls
 *    the user program does.  Therefore, assume that practically every
 *    routine is reentrant (although it's still ok to assume that a
 *    hi level routine will not be interrupted by another hi level
 *    routine unless it explicitly calls sleep).
 *
 * 2. never hang a process so it can't be killed.  We insure this by
 *    never sleeping at a negative priority
 */

/* Data formats:
 *
 * Midi:
 *
 * The input and output midi data streams are the same:  The data is in
 * 4 byte packets.  The least significant byte is the midi code, and the
 * most significant three bytes is the delta in milliseconds between the
 * previous code and this code.  This scheme allows
 * you to make a simple recorder like this:
 *
 *     $ dd if=/dev/sbmidi of=mfile bs=4
 *         ...play on keyboard...
 *     ^C
 *     $ cp mfile /dev/sbmidi
 *         ...music is played back...
 *
 * I used 'dd' instead of cp so I would not lose the last bufferful of data
 * when I interrupted it.  A fancy recorder would arrange for a more 
 * graceful way to signal the end of recording.
 *
 * Note that this scheme records exactly the same information as if the
 * 3 byte time was a delta from the start of the file, instead of from
 * the previous byte.  This allows you to cut a section out of the middle
 * of a file and play it by simply writing it out again.
 * Also, you can concatenate two files
 * for output (although you probably want a utility that would adjust the
 * delta in the first packet of the second file to space the files out
 * correctly).  
 *
 * A read on the midi device blocks until there is some data, then returns
 * however much is available, even if it is less than the user asked for.
 * This is similar to tty character devices.   You may also set the NDELAY
 * flag either in open or with fcntl to cause read to be entirely 
 * non-blocking.
 *
 * It whould be really nice if it were possible to use something like
 * select(2) to get input from either the midi keyboard or the tty.
 * Or to use midi input in an X-window client program.  I don't
 * know how to pull that off short of converting this to a streams driver.
 */

/*
 * Xenix 386 port notes (see install.md for full detail):
 *
 * - This kernel's #include <sys/foo.h> resolves to the *userspace*
 *   /usr/include/sys/foo.h, since /usr/sys/h has no "sys" subdirectory
 *   (headers sit flat: /usr/sys/h/foo.h). We use unprefixed <foo.h>
 *   with -I/usr/sys/h instead, matching the driver-writer's guide's own
 *   example ("../h/param.h" etc). <sysmacros.h> is added explicitly:
 *   it supplies the minor()/major()/btoc()/ctob() macros that the
 *   original driver relied on via <sys/sysmacros.h>.
 * - There is no <dma.h> on the include path (the real one lives in
 *   /usr/sys/io/dma.h, which is not searched), so the handful of 8237
 *   constants this driver needs are defined below. The DMA *functions*
 *   themselves - dma_alloc/dma_relse/dma_param/dma_enable/dma_resid - do
 *   exist in this kernel and are used exactly as the original driver did.
 * - cmn_err() also exists here, but printf() is what the driver-writer's
 *   guide uses in its own examples, so the printf() calls are kept.
 */
#include <types.h>
#include <param.h>
#include <file.h>
#include <dir.h>
#include <buf.h>
#include <signal.h>
/*
 * <user.h> (M_I386 build) references struct tabent/struct descriptor,
 * defined in <page.h>/<seg.h> respectively. Real kernel source normally
 * pulls these in via other umbrella headers before user.h; we need them
 * explicitly here since this driver includes user.h directly.
 */
#include <page.h>
#include <seg.h>
#include <user.h>
#include <sysmacros.h>
#include <errno.h>
#include <conf.h>
#include "sb.h"

int wakeup ();

/*
 * From /usr/sys/io/dma.h, which is not on the -I search path. The kernel
 * DMA functions themselves (dma_alloc/dma_relse/dma_param/dma_enable/
 * dma_resid) are present in this kernel's libraries and are used below.
 */
#ifndef DMA_Rdmode
#define DMA_Rdmode 0x44		/* device -> memory */
#endif
#ifndef DMA_Wrmode
#define DMA_Wrmode 0x48		/* memory -> device */
#endif
#ifndef DMA_BLOCK
#define DMA_BLOCK 0
#endif
#ifndef DMA_NBLOCK
#define DMA_NBLOCK 1
#endif

/*
 * Xenix 386 hardware support routine.
 *
 * tenmicrosec() does not exist in this kernel (unlike the DMA routines,
 * which do). sb_tenmicrosec() is the classic AT-era I/O-delay trick:
 * reading an unused port (0x80, the POST-code diagnostic port) takes
 * about 1 microsecond on ISA bus timing, so ten reads approximate 10us.
 */

static void
sb_tenmicrosec()
{
	register int i;

	for (i = 0; i < 10; i++)
		inb(0x80);
}

/*
 * Xenix 386: this kernel has no iomove() function at all. The original
 * driver used it as a combined copyin()/copyout() that also implicitly
 * advanced the current syscall's user-space buffer pointer/count
 * (u.u_base / u.u_count in the classic scheme). This kernel's struct
 * user has no u_base field, but does have the equivalent u_baseu /
 * u_count (u_baseu is a plain char* under M_I386). Real copyin()/
 * copyout() exist here but need an explicit user address and do not
 * touch u.u_baseu/u.u_count themselves, so this wrapper does both
 * explicitly, matching what iomove() would have done.
 */
static void
sb_iomove(kaddr, count, rw)
caddr_t kaddr;
unsigned count;
int rw;
{
	if (rw == B_WRITE)
		copyin(u.u_baseu, kaddr, count);
	else
		copyout(kaddr, u.u_baseu, count);

	u.u_baseu += count;
	u.u_count -= count;
}


/* #define static */
int sb_owns_dma;

#ifdef	FASTINTR
/* Assembler interrupt handler common storage */
long	sb_next = 0;
char	sb_dmadir = 0;
char	sb_dmalow = 0;
char	sb_dmahigh = 0;
char	sb_dmapage = 0;
char	sb_dmalenh = 0;
char	sb_dmalenl = 0;
short	sb_dspstat = 0;
short	sb_dspcmd = 0;
short	sb_dspdata = 0;
char	sb_dspdir = 0;
char	sb_dsplenh = 0;
char	sb_dsplenl = 0;

unsigned long sb_poke_save;
extern  sb_asmintr();
static  void sb_prep_dma();
#endif

int sb_didprep = 0;
int sb_didstart = 0;

static int dsp_reset ();
static int dsp_speed ();
static int dsp_voice ();
static void sb_start_dma ();
static int fm_reset ();
static int midi_write_flush ();

/* GLOBALS */
static struct sb_stat_type sb_status;          /* Soundblaster Status */
extern time_t lbolt;


#ifdef LDRV
#include "ldrv.h"

int sbopen (), sbclose (), sbread (), sbwrite (), sbioctl (), sbintr ();

initfunc (op, ldp)
struct ldrvarg *ldp;
{
	struct cdevsw *cp;
	int x, y;

	sb_dma_chan = 1;	/* XXX */
	sb_interrupt = 5;	/* XXX */

	switch (op) {
	case LDRV_LOAD_OP:
		printf ("Sound Blaster installed\n");
		cp = &cdevsw[ldp->majornum];
		cp->d_open = sbopen;
		cp->d_close = sbclose;
		cp->d_read = sbread;
		cp->d_write = sbwrite;
		cp->d_ioctl = sbioctl;
		/* second arg is priority */
		ldrv_set_intr (sb_interrupt, 5, sbintr);
		sbinit ();
		return (0);
	case LDRV_UNLOAD_OP:
		if (!sbidle ()) {
			printf ("Sound Blaster not idle: can't unload\n");
			return (-1);
		}
		printf ("Sound Blaster unloading\n");
		return (0);
	default:
		printf ("unknown op %d\n", op);
		return (0);
	}
}

int
sbidle ()
{
	if (sb_status.dsp_open_for_reading
	    || sb_status.dsp_open_for_writing
	    || sb_status.fm_open
	    || sb_status.cms_open
	    || sb_status.midi_open_for_reading
	    || sb_status.midi_open_for_writing)
		return (0);

	dsp_reset ();
	return (1);
}
#endif /* LDRV */

/* The board supports these sampling rates:
 *
 * Input:  8-bit only   4khz to 12khz
 * Output: 8-bit        4khz to 23khz
 *         4-bit ADPCM  4khz to 12khz
 *         2-bit ADPCM  4khz to 13khz
 *         2.6 ADPCM    4khz to 11khz
 */

/* The board uses DMA for the DAC and ADC.  To deal with the poor real-time
 * preformance of unix at the user process level, we implement a double
 * buffering scheme in the kernel.  There are two buffers of DSP_BUF_SIZE
 * bytes.  For writes, one gets filled by the user program while the 
 * other is drained by the board.  The variable dsp_hi is always 0 or 1,
 * telling which buffer is current for the user process.  dsp_low tells
 * the current buffer for the interrupt routine.  Since the user may
 * fill up the buffer using a series of write calls, the variable dsp_hi_used
 * tells how many bytes have been filled in so far.  When a user write
 * fills the buffer, the flag dsp_full[dsp_hi] is set, and then dsp_hi
 * is toggled.  dsp_full[x] is set to 0 by the interrupt routine when
 * it is done draining the buffer.  Normally, the user program will be
 * writing faster than the board can drain, so when switching buffers,
 * it blocks until dsp_full[x] is 0.  If the user gets behind, the 
 * interrupt routine will notice that dsp_full[x] is 0 when it wants
 * to swich buffers, so it will shut down.  This will cause a gap in 
 * the output.  When the user eventually writes some more, dsp_write()
 * will restart the interrupt level processing.
 *
 * There is an ioctl to flush out a partial buffer, when that is desired.
 * The buffer is not automatically flushed during when the device is closed,
 * since that is likely to cause a confusing delay of a few seconds when
 * the user tries to kill the program.
 *
 * Reading works pretty much the same way.  This time, dsp_full is set by
 * the interrupt routine and reset by the user level.  dsp_hi_used tells
 * how many bytes of the current buffer have been passed to the user.
 * As a special feature, the interrupt routine can set a flag if it 
 * overruns the buffers, and this will ultimately cause EIO to be 
 * returned to one of the user's read calls.  This lets the user know 
 * that some samples were missed.
 */

/* The IBM-PC dma hardware cannot do transfers that cross a 64k boundary.
 * Therefore, we allocate 3 buffers, all in a space of less than 64k.  
 * Therefore, we know that at most one of the buffers crosses a boundary,
 * and that the other two are safe.
 */
/* XXX move to space.c */
/*
 * Must be less than 64k/3. This array is 3*DSP_BUF_SIZE of *initialized*
 * data as far as this linker is concerned (the kernel x.out format has no
 * bss), so at the default 16384 it adds 48K to the kernel's data segment -
 * for comparison, adding the whole TCP/IP stack only added about 12K.
 * Override with -DDSP_BUF_SIZE=nnnn to build a smaller-footprint kernel.
 */
#ifndef DSP_BUF_SIZE
#define DSP_BUF_SIZE 16384
#endif
unsigned char dsp_buf_space[DSP_BUF_SIZE * 3];
static unsigned char *dsp_buf[2];

/* physical addresses for dma_buf[] */
static paddr_t dsp_buf_phys[2];

/* for each buffer, set by the writer, reset by reader */
static int dsp_full[2];

 /* current buffer for hi and low levels */
static int dsp_hi;
static int dsp_low;

/* number of bytes already written/read in buffer */
static int dsp_used[2];

/* set when interrupt routine should do wakeup (dsp_buf) */
static int dsp_wanted;

/* these are used to implement the input overrun error */
static int dsp_waiting_for_first_read;
static int dsp_error;

static int sb_interrupt_pending;

#define DSP_LOOP_LIMIT_MILLISECONDS 5

/*
 * Upper bound on the spin in dsp_command(), expressed as an iteration
 * count. The original driver measured this at init time against lbolt,
 * but that cannot work here: sbinit() runs during the kernel's device
 * init sweep, before the timer interrupt is live, so lbolt never advances
 * and the measurement always came out as delta == 0 ("warning: error
 * setting dsp_loop_limit"). It also burned a full second of boot on a
 * million ISA port reads whose result was then discarded.
 *
 * Calibration is not actually needed. Every iteration of the spin below
 * contains an inb() on the ISA bus, which costs roughly a microsecond on
 * any AT-class machine regardless of CPU speed, so the iteration count
 * for a given timeout is essentially CPU-independent. 20000 iterations is
 * therefore on the order of 20ms - comfortably longer than
 * DSP_LOOP_LIMIT_MILLISECONDS, and this is only ever a timeout: the DSP
 * is normally ready on the first pass.
 */
#define DSP_LOOP_LIMIT (4000 * DSP_LOOP_LIMIT_MILLISECONDS)

static int dsp_loop_limit = DSP_LOOP_LIMIT;

/*
 * reset DSP chip, and return TRUE if successful
 */
static int 
dsp_reset()
{
	int i;
	register unsigned char rc;
	int s;

	s = spl5 ();
	if (sb_owns_dma)
		dma_resid (SB_DMA_CHAN); /* disable dma */
	sb_interrupt_pending = 0;
	dsp_full[0] = 0;
	dsp_full[1] = 0;
	dsp_hi = 0;
	dsp_low = 0;
	dsp_used[0] = 0;
	dsp_used[1] = 0;
	dsp_wanted = 0;
#ifdef	FASTINTR
	sb_next = 0;
#endif
	wakeup (dsp_buf);
	splx (s);

	/* reset dsp */
	outb(DSP_RESET, 0x01);
	sb_tenmicrosec();
	outb(DSP_RESET, 0x00);
	for (i=0; i<200; i++) {
		rc = inb(DSP_RDAVAIL);
		if (rc & 128) {
			rc = inb(DSP_RDDATA);
			if (rc == 0xAA)
				break;
		}
	}

	if (i>=200) {
		printf("SoundBlaster(tm) DSP failed initialization\n");
		return (FALSE);
	}

	dsp_voice (FALSE);

	/* reset sampling speed */
	dsp_speed();

	return(TRUE);
}

/* this routine must not sleep */
static int
dsp_command (val)
int val;
{
	int i;

	for (i = 0; i < dsp_loop_limit; i++) {
		if ((inb (DSP_STATUS) & 0x80) == 0) {
			outb(DSP_COMMAND, val);
			return (TRUE);
		}
	}
printf("dsp_command: fail\n");
	return (FALSE);
}

/*
 * program the DSP's time constant: the sampling/output rate
 */
static int 
dsp_speed()
{
	char time_constant;

	time_constant = (char)(256 - (1000000/sb_status.dsp_speed));

	if (dsp_command (DSPCMD_TIME) == FALSE /* SET_TIME_CONSTANT */
	    || dsp_command (time_constant) == FALSE) {
		return (FALSE);
	}
	
	return(TRUE);
}


static int no_sound_blaster;

#define crosses_64k_boundary(a,b) ((((paddr_t)(a) & 0xffff) + (b)) >= 0x10000)

/*
 * called at OS startup time to initialize the SoundBlaster
 */
int
sbinit()
{
	unsigned char *p;
	paddr_t kp;
	
	/* Was checking logical addresses, not physical addresses! */
	p = dsp_buf_space;
	kp = ktop(p);
	if (crosses_64k_boundary (kp, DSP_BUF_SIZE)) {
		p += DSP_BUF_SIZE;
		kp += DSP_BUF_SIZE;
	}
	dsp_buf[0] = p;
	dsp_buf_phys[0] = kp;
	p += DSP_BUF_SIZE;
	kp += DSP_BUF_SIZE;
	if (crosses_64k_boundary (kp, DSP_BUF_SIZE)) {
		p += DSP_BUF_SIZE;
		kp += DSP_BUF_SIZE;
	}
	dsp_buf[1] = p;
	dsp_buf_phys[1] = kp;
	
	if (crosses_64k_boundary (dsp_buf_phys[0], DSP_BUF_SIZE)
	    || crosses_64k_boundary (dsp_buf_phys[1], DSP_BUF_SIZE)) {
		printf ("soundblaster configuration error: bad buffers\n");
		no_sound_blaster = 1;
		return (0);
	}

	sb_status.dsp_speed = 11000;
	sb_status.dsp_compression = ADCPM_8;

	if (dsp_reset() != TRUE)
		no_sound_blaster = 1;

	/*
	 * Add our line to the "device address vector dma comment" table
	 * the kernel prints at boot. Without this call the driver is
	 * invisible there even when it is correctly configured.
	 *
	 * printcfg(name, base, size, vector, dma, fmt, ...) prints the
	 * address range as base..base+size and the vector in octal, so
	 * SB_VECTOR is the master(F) vector number, not the raw IRQ.
	 * A negative vector or dma prints as "-".
	 */
	printcfg ("sb", SB_IO_PORT, 0x0f, SB_VECTOR, SB_DMA_CHAN,
		  no_sound_blaster
			? "not found"
			: "type=SoundBlaster unit=0");

	return (0);
}


/*
 * turn the dsp voice on if param is true
 */
static int
dsp_voice(on)
int on;
{
	return (dsp_command (on ? DSPCMD_SPKON : DSPCMD_SPKOFF));
}


/*
 * grabs the DSP chip for a process.
 * sets u.u_error to EBUSY if already opened by other device
 */
static void 
dsp_open(flag)
int flag;
{
	/* Claim the DMA channel so we arbitrate properly against the other
	 * DMA users on this machine (notably the floppy driver). Note that
	 * if dma_single is set (DMAEXCL), dma_alloc() collapses all the
	 * 8-bit channels onto one lock, so holding channel 1 locks out the
	 * floppy for as long as the dsp is open - and conversely, an open
	 * while the floppy is busy fails here with EBUSY.
	 */
	if (dma_alloc (SB_DMA_CHAN, DMA_NBLOCK) == 0) {
		u.u_error = EBUSY;
		return;
	}

	/* hardware does not support simultaneous dma and midi input */
	if (sb_status.midi_open_for_reading) {
		u.u_error = EBUSY;
		goto err;
	}

	/* hardware does not support simultaneous dma input and output */
	if ((flag & FREAD) != 0
	    && (flag & FWRITE) != 0) {
		u.u_error = EINVAL;
		goto err;
	}

	if (sb_status.dsp_open_for_reading == 0
	    && sb_status.dsp_open_for_writing == 0
	    && sb_status.midi_open_for_reading == 0
	    && sb_status.midi_open_for_writing == 0) {
		if (dsp_reset () == FALSE) {
printf("dsp_open fail #1");
			u.u_error = EIO;
			goto err;
		}
	}

	if (sb_status.dsp_open_for_writing == 0
	    && (flag & FWRITE) != 0) {
		if (sb_status.dsp_open_for_reading) {
			u.u_error = EBUSY;
			goto err;
		}
		sb_status.dsp_open_for_writing = 1;

		if (dsp_voice (TRUE) == FALSE) {
printf("dsp_open fail #2");
			u.u_error = EIO;
			goto err;
		}
	}

	if (sb_status.dsp_open_for_reading == 0
	    && (flag & FREAD) != 0) {
		if (sb_status.dsp_open_for_writing) {
			u.u_error = EBUSY;
			goto err;
		}
		sb_status.dsp_open_for_reading = 1;

		dsp_waiting_for_first_read = 1;
#		if RICK
			/* Do not start DMA till first read */
#		else
			sb_start_dma (B_READ);
#		endif
	}

	sb_pokeintr();

	sb_owns_dma = 1;
	return;
   err:
	sb_status.dsp_open_for_reading = 0;
	sb_status.dsp_open_for_writing = 0;
	dma_relse (SB_DMA_CHAN);
}

sb_pokeintr() {
/* Poke hard interrupt handler into right address */
#ifdef	FASTINTR
	{
	unsigned char * trap, *handler;
	unsigned long * ltrap;
	extern ivctM0();	/* start of interrupt jump table */

	trap = (unsigned char *) ivctM0;		
	trap = &trap[sb_interrupt * 12];	/* find our interrupt */
	handler = (unsigned char *) sb_asmintr;
	trap += 5;				/* move after prep */
	ltrap = (unsigned long *) trap;
	sb_poke_save = *ltrap;
	*ltrap = handler - (trap + 4);		/* it's a relative jump */
	}
#endif
}

static void
midi_open (flag)
int flag;
{
	/* hardware does not support DMA and midi input at the same time,
	 * but it is possible to do midi input and output at the same time,
	 * and it is marginal to do midi output with DMA (but that causes
	 * the dma to have gaps when the midi bytes are being sent)
	 */

	if ((flag & FREAD) != 0
	    && (sb_status.dsp_open_for_reading
		|| sb_status.dsp_open_for_writing)) {
		u.u_error = EBUSY;
		return;
	}

	if (sb_status.midi_open_for_reading == 0
	    && sb_status.midi_open_for_writing == 0
	    && sb_status.dsp_open_for_reading == 0
	    && sb_status.dsp_open_for_writing == 0) {
		if (dsp_reset () == FALSE) {
			u.u_error = EIO;
			return;
		}
	}

	if ((flag & FREAD) != 0
	    && sb_status.midi_open_for_reading == 0) {
		midi_in_in = 0;
		midi_in_out = 0;
		midi_in_waiting_for_first_io = 1;
		
		/* ack old interrupt, if any */
		inb (DSP_RDAVAIL);
		/* turn on midi input interrupts */
		if (dsp_command (DSPCMD_MIDIIN) == FALSE) {
			u.u_error = EIO;
			return;
		}
		sb_interrupt_pending = 1;
		sb_status.midi_open_for_reading = 1;
	}

	if ((flag & FWRITE) != 0
	    && sb_status.midi_open_for_writing == 0) {
		midi_out_waiting_for_first_io = 1;
		sb_status.midi_open_for_writing = 1;
	}
}

static void 
fm_open(flag)
int flag;
{
	if (fm_reset() == FALSE) {
		u.u_error = EIO;
		return;
	}
		
	sb_status.fm_open = 1;
}

/*
 * multiplexes opens to dsp_open(), fm_open(), and cms_open()
 * depending upon which minor dev was used
 */
int
sbopen(dev, flag)
dev_t dev;
int flag;
{
	if (no_sound_blaster) {
		u.u_error = ENXIO;
		return (0);
	}

	switch (minor (dev)) {
        case SB_FM_NUM:
		fm_open (flag);
		break;
        case SB_DSP_NUM:
		dsp_open (flag);
		break;
	case SB_MIDI_NUM:
		midi_open (flag);
		break;
        default:
		u.u_error = ENXIO;
		break;
	}
	
	sb_pokeintr();

	return(0);
}


/*
 * Release and reset the dsp chip
 */
static void 
dsp_close()
{
	int s;

	/* resetting the dsp can't hurt midi output, and we know there
	 * can't be any midi input going on, since that is not allowed
	 * when the dsp is opened
	 */
	/* Sleep until output done */
	if (sb_status.dsp_open_for_writing) {
		s = spl5 ();
		/* if a partial write remaining */
		dsp_full[dsp_hi] = (dsp_used[dsp_hi] > 0);
		if (!sb_interrupt_pending && dsp_full[dsp_hi]) {
			/* ASSERT(dsp_low == dsp_hi); */
			sb_start_dma (B_WRITE);
		}
#ifdef	FASTINTR
		else if (! sb_next && dsp_full[dsp_hi])
			sb_prep_dma (B_WRITE, dsp_hi);
#endif
		if (dsp_full[dsp_hi]) {
			dsp_wanted = 1;
			/* PCATCH to avoid device-busy bug */
			sleep (dsp_buf, PZERO+1 | PCATCH); 
			/* if it didn't finish, forget it. */
			/* dsp_reset & re-init should take care of things. */
		}
		splx (s);
	}

	dsp_reset();
	sb_status.dsp_open_for_reading  = 0;
	sb_status.dsp_open_for_writing  = 0;

	if (sb_owns_dma)
		dma_relse (SB_DMA_CHAN);
	sb_owns_dma = 0;


}

sb_unpokeintr() {
#ifdef	FASTINTR
	unsigned char * trap, *handler;
	unsigned long * ltrap;
	extern ivctM0();	/* start of interrupt jump table */

	/* Restore normal interrupt handler */
	trap = (unsigned char *) ivctM0;		
	trap = &trap[sb_interrupt * 12];	/* find our interrupt */
	handler = (unsigned char *) sb_asmintr;
	trap += 5;
	ltrap = (unsigned long *) trap;
	*ltrap = sb_poke_save;
#endif
}

#define midi_putc(c) (midi_out_buf[midi_out_used++] = (c), \
		      (midi_out_used >= MIDI_OUT_CHUNK) \
		      ? midi_write_flush () : TRUE)

static void
midi_close ()
{
	int note;

	/* my keyboard does not respond properly to all-notes-off, so
	 * here we turn off each note explicitly
	 */
	if (sb_status.midi_open_for_writing) {
		for (note = 0; note < 128; note++) {
			/* this is "note-on, velocity 0" */
			if (midi_putc (0x90) == FALSE
			    || midi_putc (note) == FALSE
			    || midi_putc (0) == FALSE)
				break;
		}
		midi_write_flush ();
	}

	/* turn off midi input interrupts, if necessary */
	if (sb_status.midi_open_for_reading)
		dsp_command (DSPCMD_MIDIIN);

	sb_status.midi_open_for_reading = 0;
	sb_status.midi_open_for_writing = 0;

	if (sb_status.dsp_open_for_reading == 0
	    && sb_status.dsp_open_for_writing == 0)
		dsp_reset ();
}

/*
 * Release and reset the fm chip
 */
static void
fm_close()
{
	sb_status.fm_open = 0;
	fm_reset();
}


/*
 * Multiplexes between the closes for dsp, fm, and cms chips
 */
int
sbclose(dev)
dev_t dev;
{
	switch (minor (dev)) {
	case SB_FM_NUM:
		fm_close();
		break;
	case SB_DSP_NUM:
		dsp_close();
		break;
	case SB_MIDI_NUM:
		midi_close ();
		break;
	default:
		u.u_error = ENXIO;
		break;
	}
    
	sb_unpokeintr();
	return(0);
}

static void
dsp_write ()
{
	int count;
	int s;

	/* this could only be triggered if another process is also
	 * writing to the board.  Note that this can happen even if
	 * we were to enforce an exclusive open in dsp_open since
	 * a process can fork and both parent and child can write
	 * on the same descriptor.
	 */
	s = spl5 ();
	while (dsp_full[dsp_hi]) {
		dsp_wanted = 1;
		sleep (dsp_buf, PZERO+1);
	}
	splx (s);

	DPR((" write cnt %d %d used %d,%d\n",
		u.u_count, dsp_hi, dsp_used[0], dsp_used[1]));

	/* XXX change to use iomove */
	while (u.u_count > 0) {
		count = u.u_count;
		if (count > DSP_BUF_SIZE - dsp_used[dsp_hi])
			count = DSP_BUF_SIZE - dsp_used[dsp_hi];
		/* ignore address errors */
		sb_iomove (dsp_buf[dsp_hi] + dsp_used[dsp_hi], count, B_WRITE);
		dsp_used[dsp_hi] += count;
		if (dsp_used[dsp_hi] == DSP_BUF_SIZE) {
			s = spl5 ();
			dsp_full[dsp_hi] = 1;
			if (sb_interrupt_pending == 0)
				sb_start_dma (B_WRITE);
#ifdef	FASTINTR
			/* if asm hasn't already started one */
			else if (! sb_next)
				sb_prep_dma  (B_WRITE, dsp_hi);
#endif
			dsp_hi ^= 1;
			dsp_used[dsp_hi] = 0;

			/* wait until the interrupt routine drains
			 * this buffer before we start using it
			 */
			while (dsp_full[dsp_hi]) {
				dsp_wanted = 1;
				sleep (dsp_buf, PZERO+1);
			}
			splx (s);
		}
	}
}

/* for the flush ioctl.  Returns TRUE/FALSE, like the other dsp_*
 * helpers: the DSP_IOCTL_FLUSH case in sbioctl() sets EIO on FALSE.
 */
static int
dsp_flush ()
{
	int s;

	DPR(("flush %d used %d,%d\n", dsp_hi, dsp_used[0], dsp_used[1]));

	/* flush out any partial buffer */
	if (dsp_used[dsp_hi] != 0) {
		dsp_full[dsp_hi] = 1;
		if (sb_interrupt_pending == 0)
			sb_start_dma (B_WRITE);
#ifdef	FASTINTR
		else if (! sb_next)
			sb_prep_dma  (B_WRITE, dsp_hi);
#endif
		/* From Rick @ Digiboard */
		dsp_hi ^= 1;
		dsp_used[dsp_hi] = 0;
	}

 	DPR(("flush %d full %d,%d\n", dsp_hi, dsp_full[0], dsp_full[1]));
	s = spl5 ();
	while ((dsp_full[0] || dsp_full[1])
	       && sb_interrupt_pending) {
		dsp_wanted = 1;
		sleep (dsp_buf, PZERO+1);
	}
	splx (s);

	return (TRUE);
}

/*
 * Converting between the packet stream's millisecond delta times and the
 * only clock available in here, lbolt, which counts clock ticks.
 *
 * The original driver spelled this "lbolt * 10", i.e. it assumed HZ == 100.
 * That is wrong on XENIX: a clock tick is 1/50th of a second on most 286
 * and 386 machines (Device Driver Writer's Guide, ch. 9 - "One clock tick
 * equals 1/50th of a second"), so HZ == 50.  The effect was that this
 * clock advanced at half real speed, every scheduled gap between MIDI
 * bytes was waited out twice as long as the file asked for, and all MIDI
 * playback ran at exactly half tempo.
 *
 * Derive the scale from HZ (<param.h>) rather than assuming a value.
 * Note that the timing resolution of MIDI output is one tick - 20ms at
 * HZ == 50 - which is fine for music but does quantize very fast runs.
 */
#define MS_PER_TICK	(1000 / HZ)
#define MIDI_NOW()	((lbolt * MS_PER_TICK) & 0xffffff)
#define MS_TO_TICKS(ms)	(((ms) + MS_PER_TICK - 1) / MS_PER_TICK)

/*
 * Here is the midi output timing strategy.  We read the midi
 * data stream up until the next pause of more than about 20 milliseconds
 * (or until we get 60 bytes).
 *
 * Then we dump these bytes out as quickly as possible.  This loop
 * keeps any user processes from running, but it is at spl0, so 
 * timer interrupt still work.  It takes about 20 milliseconds to
 * dump out 60 bytes.
 *
 * If we did a whole 60 bytes, then we better pause a little to let
 * other processes run.  It is most likely that if we are trying to
 * send data that fast, then it probably isn't really music - either
 * the file is trash, or perhaps we are downloading samples.  In
 * either case, it won't matter if we introduce a gap.
 */
static int
midi_write_flush ()
{
	int i;

	/* dsp_command does not sleep, so no other process
	 * will try to do a flush at the same time
	 */
	for (i = 0; i < midi_out_used; i++) {
		if (dsp_command (DSPCMD_MIDIOUT) == FALSE
		    || dsp_command (midi_out_buf[i]) == FALSE)
			return (FALSE);
	}

	if (midi_out_used >= MIDI_OUT_CHUNK) {
		midi_out_used = 0;
		timeout (wakeup, midi_write_flush, 3);
		sleep (midi_write_flush, PZERO+1);
	} else {
		midi_out_used = 0;
	}

	return (TRUE);
}

static void
midi_write ()
{
	int code;
	int t;
	int scheduled_time;
	int current_time;
	int delta;
	int c;
	
	midi_out_used = 0;
	while (u.u_count >= 4) {
		/* I don't think there is a need to convert this to
		 * iomove, since midi output is fairly slow 
		 */
		if ((code = cpass ()) < 0)
			break;

		if ((c = cpass ()) < 0)
			break;
		t = c;
		if ((c = cpass ()) < 0)
			break;
		t |= (c << 8);
		if ((c = cpass ()) < 0)
			break;
		t |= (c << 16);

		/* all of this hair is to arrange for bytes to
		 * be sent out as soon after their scheduled time
		 * as possible.  If, for some reason, output is
		 * delayed for a while, then when it picks up again,
		 * all of the overdue bytes are sent as quickly as
		 * possible, and then the normal timing resumes
		 */
		if (midi_out_waiting_for_first_io) {
			midi_out_waiting_for_first_io = 0;
			current_time = MIDI_NOW ();
			midi_out_last_time = current_time;
		} else {
			scheduled_time = midi_out_last_time + t;
		recheck:
			current_time = MIDI_NOW ();

			delta = (scheduled_time - current_time) & 0xffffff;
			if (delta > MS_PER_TICK && (delta & 0x800000) == 0) {
				if (midi_out_used) {
					if (midi_write_flush () == FALSE)
						goto bad;
				} else {
					timeout (wakeup, midi_write,
						 MS_TO_TICKS (delta));
					sleep (midi_write, PZERO+1);
				}
				goto recheck;
			}
		}
		midi_out_last_time += t;

		if (midi_putc (code) == FALSE)
			goto bad;
	}
	if (midi_out_used) {
		if (midi_write_flush () == FALSE)
			goto bad;
	}
	return;

 bad:
	u.u_error = EIO;
	return;
}

int
sbintr ()
{
	unsigned char code;

	if (sb_interrupt_pending) {
		sb_interrupt_pending = 0;

		if (sb_status.midi_open_for_reading) {
#ifndef	FASTINTR
			inb(DSP_RDAVAIL); /* acknowledge interrupt */
#endif
			sb_interrupt_pending = 1;

			/* The manual says that MIDI input buffers ??? */
			do {
				code = inb (DSP_RDDATA);
/*
				printf("midi input: 0x%x\n", code);
*/
				/* ignore real-time messages */
				if ((code & 0xf8) == 0xf8) 
					continue;
				midi_in_buf[midi_in_in++] = (MIDI_NOW () << 8)
					| code;

			} while (inb(DSP_RDAVAIL) & 0x80);
			if (midi_in_wanted) {
				midi_in_wanted = 0;
				wakeup (midi_in_buf);
			}
		} else if (sb_status.dsp_open_for_writing) {
#ifdef	FASTINTR
			/* assembler handler already started another transfer */
			if (sb_next) {
				sb_interrupt_pending = 1;
				sb_didprep++;
			}
#endif
#ifndef	FASTINTR
			/* acknowledge interrupt */
			inb(DSP_RDAVAIL);
#endif
			dsp_full[dsp_low] = 0;
			dsp_low ^= 1;
#ifdef	FASTINTR
			if (dsp_full[dsp_low])
				if (sb_next)
					sb_prep_dma (B_WRITE, dsp_low);
				else
					sb_start_dma (B_WRITE);
			sb_next = 0;
#endif
#ifndef	FASTINTR
			if (dsp_full[dsp_low])
				sb_start_dma (B_WRITE);
#endif
			dsp_used[dsp_low] = 0;
			if (dsp_wanted) {
				dsp_wanted = 0;
				wakeup (dsp_buf);
			}
		} else if (sb_status.dsp_open_for_reading) {
			/* ack interrupt */
#ifndef	FASTINTR
			inb(DSP_RDAVAIL); /* acknowledge interrupt */
#endif
			dsp_full[dsp_low] = 1;
			dsp_low ^= 1;
			if (dsp_full[dsp_low])
				dsp_error = EIO; /* overrun */
			dsp_full[dsp_low] = 0;
			sb_start_dma (B_READ);
			if (dsp_wanted) {
				dsp_wanted = 0;
				wakeup (dsp_buf);
			}
		}
	} else {
		/* Not expecting anything: still acknowledge, or the board
		 * keeps its interrupt line asserted.
		 */
		inb (DSP_RDAVAIL);
	}
	return (0);
}

static void
sb_start_dma (flag)
int flag;
{
	int bsize = (flag == B_READ) ? DSP_BUF_SIZE : dsp_used[dsp_low];

	/* dma_param() writes the count to the 8237 verbatim, so the
	 * caller passes (length - 1), same as the SoundBlaster itself.
	 */
	dma_param(SB_DMA_CHAN,
		  (flag == B_READ) ? DMA_Rdmode : DMA_Wrmode,
		  dsp_buf_phys[dsp_low],
		  bsize - 1);

	dma_enable(SB_DMA_CHAN);

	/* prep SoundBlaster for 8-bit DMA and length */
	dsp_command ((flag == B_READ) ? DSPCMD_READ : DSPCMD_WRITE);
	dsp_command (bsize - 1);
	dsp_command ((bsize - 1) >> 8);

	DPR(("dma %d\n", bsize));

	sb_interrupt_pending = 1;
	sb_didstart++;
}

#ifdef	FASTINTR
/* set up variables for assembler handler to start next dma */
static void
sb_prep_dma (flag, which)
int flag, which;
{
	int bsize = (flag == B_READ) ? DSP_BUF_SIZE : dsp_used[which];

	sb_dmadir  = ((flag == B_READ) ? DMA_Rdmode : DMA_Wrmode);
	sb_dmadir |= SB_DMA_CHAN;
	sb_dmalow  = dsp_buf_phys[which];
	sb_dmahigh = dsp_buf_phys[which] >> 8;
	sb_dmapage = dsp_buf_phys[which] >> 16;
	sb_dmalenl = (bsize - 1);
	sb_dmalenh = (bsize - 1) >> 8;
	
	/* prep SoundBlaster for 8-bit DMA and length */
	sb_dspdata = DSP_RDAVAIL;
	sb_dspstat = DSP_STATUS;
	sb_dspcmd  = DSP_COMMAND;

	sb_dspdir  = ((flag == B_READ) ? DSPCMD_READ : DSPCMD_WRITE);
	sb_dsplenl = (bsize - 1);
	sb_dsplenh = (bsize - 1) >> 8;
	
	/* There is another transfer to start, set up now. */
	sb_next = 1;	
}
#endif

/*
 * multiplexes writes to dsp, cm/s and fm chips
 */
int
sbwrite(dev)
int dev;
{
	switch (minor (dev)) {
        case SB_CMS_NUM:
		printf("sbwrite(): error, cms device accessed\n");
		u.u_error = ENXIO;
		break;
        case SB_FM_NUM:
		u.u_error = ENXIO;
		break;
        case SB_DSP_NUM:
		dsp_write();
		break;
	case SB_MIDI_NUM:
		midi_write ();
		break;
        default:
		u.u_error = ENXIO;
	}
	return(0);
}


/*
 * Starts the DMA read from the Soundblaster
 */
static int
dsp_read()
{
	int s;
	int count;

	if (dsp_waiting_for_first_read) {
		/* reset the flags associated with trapping overruns */
		s = spl5 ();
		dsp_waiting_for_first_read = 0;
		dsp_error = 0;
		dsp_full[0] = 0;
		dsp_full[1] = 0;
		splx (s);
#		if RICK
			/* Start the first read */
			sb_start_dma (B_READ);
#		endif
	}

	while (u.u_count > 0) {
		if (dsp_error) {
			u.u_error = dsp_error;
			return;
		}

		s = spl5 ();
		while (dsp_full[dsp_hi] == 0) {
			dsp_wanted = 1;
			sleep (dsp_buf, PZERO+1);
		}
		splx (s);

		count = u.u_count;
		if (count > DSP_BUF_SIZE - dsp_used[dsp_hi])
			count = DSP_BUF_SIZE - dsp_used[dsp_hi];
		
		sb_iomove (dsp_buf[dsp_hi] + dsp_used[dsp_hi], count, B_READ);
		if (u.u_error)
			return;
		dsp_used[dsp_hi] += count;
		if (dsp_used[dsp_hi] >= DSP_BUF_SIZE) {
			dsp_full[dsp_hi] = 0;
			dsp_hi ^= 1;
			dsp_used[dsp_hi] = 0;
		}
	}
}

static int
midi_read ()
{
	int val;
	int s;
	int code, t;
	int lbolt_low_bits;

	if (u.u_count < 4) {
		u.u_error = EINVAL;
		return;
	}

	if (midi_in_in == midi_in_out) {
		if (u.u_fmode & FNDELAY)
			return;

		s = spl5 ();
		while (midi_in_in == midi_in_out) {
			midi_in_wanted = 1;
			sleep (midi_in_buf, PZERO+1);
		}
		splx (s);
	}

	while (u.u_count >= 4 && midi_in_out != midi_in_in) {
		code = midi_in_buf[midi_in_out] & 0xff;
		lbolt_low_bits = (midi_in_buf[midi_in_out] >> 8) & 0xffffff;
		midi_in_out++;

		if (midi_in_waiting_for_first_io) {
			midi_in_waiting_for_first_io = 0;
			t = 0;
		} else {
			t = lbolt_low_bits - midi_in_last_time;
		}
		midi_in_last_time = lbolt_low_bits;

		val = code | (t << 8);

		/* I don't think midi input comes in fast enough to justify
		 * buffering this stuff and using iomove here
		 */
		if (passc (val) < 0
		    || passc (val >> 8) < 0
		    || passc (val >> 16) < 0
		    || passc (val >> 24) < 0)
			break;
	}
}

/*
 * multiplexes read/writes to different functions
 */
int
sbread(dev)
int dev;
{
	switch (minor (dev)) {
	case SB_CMS_NUM:
		u.u_error = ENXIO;
		break;
	case SB_FM_NUM:
		u.u_error = ENXIO;
		break;
	case SB_DSP_NUM:
		dsp_read();
		break;
	case SB_MIDI_NUM:
		midi_read ();
		break;
	default:
		u.u_error = ENXIO;
	}
	return(0);
}

/*
 * minor control function for the dsp
 */
static void
dsp_ioctl(cmd, arg1, arg2)
int cmd;
caddr_t arg1, arg2;
{
	switch(cmd) {
        case DSP_IOCTL_RESET:
		if (dsp_reset() == FALSE)
			u.u_error = EIO;
		break;
        case DSP_IOCTL_SPEED:
		sb_status.dsp_speed = (int)arg1;
		if (dsp_speed() == FALSE)
			u.u_error = EIO;
		break;
        case DSP_IOCTL_VOICE:
		if (dsp_voice((int)arg1) == FALSE)
			u.u_error = EIO;
		break;
	case DSP_IOCTL_FLUSH:
		if (dsp_flush () == FALSE)
			u.u_error = EIO;
		break;
        default:
		u.u_error = ENXIO;
		break;
	}
}


static void
thirtymicrosec ()
{
	sb_tenmicrosec();
	sb_tenmicrosec();
	sb_tenmicrosec();
}

/*
 * turns a note/key off
 */
static void
fm_key_off(voice_num)
int voice_num;
{
	unsigned char reg_num;
    
	/* error checking to avoid munching kernel */
	if ((voice_num < 0) || (voice_num >= MAX_FM_NOTES)) {
		u.u_error = EINVAL;
		return;
	}
    
	/* turn voice off */
	reg_num = (unsigned char)0xB0 + (unsigned char)voice_num;
#ifdef DEBUG
	printf("turning off voice for voice %d\n", voice_num);
	printf("reg_num is %x\n", reg_num);
#endif
	outb(FM_SELECT, reg_num);
	sb_tenmicrosec();
	outb(FM_REG, 0);
	thirtymicrosec ();
}

/*
 * turns a key on, with the frequency and octave so indicated in the
 * low 2 bytes of the data integer
 */
static void
fm_key_on(usr_note)
int usr_note;
{
	register unsigned char reg_num;

#if 0
	printf("turning on voice for voice %d\n", note_num(usr_note));
	printf("fnum_low (dec): %d\n", fnum_low(usr_note));
	printf("fnum_low (hex): %x\n", fnum_low(usr_note));
	printf("keyon_blk_fnum (dec): %d\n", keyon_blk_fnum(usr_note));
	printf("keyon_blk_fnum (hex): %x\n", keyon_blk_fnum(usr_note));
#endif

	/* put out first byte */
	reg_num = (unsigned char)0xA0 + note_num(usr_note);
#ifdef DEBUG
	printf("reg_num is %x\n", reg_num);
#endif
	outb(FM_SELECT, reg_num);
	sb_tenmicrosec();
	outb(FM_REG, fnum_low(usr_note));
	thirtymicrosec ();

	/* put out second byte */
	reg_num = (unsigned char)0xB0 + note_num(usr_note);
#ifdef DEBUG
	printf("reg_num is %x\n", reg_num);
#endif
	outb(FM_SELECT, reg_num);
	sb_tenmicrosec();
	outb(FM_REG, keyon_blk_fnum(usr_note));
	thirtymicrosec ();
}

/* at this point, it just turns all notes to off */
static int
fm_reset()
{
    int i;

    /* must be initialized? */
    outb(FM_SELECT, 1);
    sb_tenmicrosec();
    outb(FM_REG, 0);
    thirtymicrosec ();

    /* dispense for time being */
    for (i=0; i<MAX_FM_NOTES; i++)
	    fm_key_off(i);

    return (TRUE);
}


/*
 * set characteristics on a voice
 */
static void
fm_set_voice(usr_character)
sb_fm_character *usr_character;
{
	register unsigned char op_cell_num;
	int cell_offset;
	sb_fm_character voice_data;
	int i;

	/* copy in characteristics */
	if (copyin(usr_character, &voice_data, sizeof(sb_fm_character)) == -1)
	{
#ifdef DEBUG
		printf("fm_set_voice(): bad address\n");
#endif
		u.u_error = EFAULT;
		return;
	}

	/* echo voice characteristics */
#ifdef DEBUG
	printf("setting voice number %d\n", voice_data.voice_num);
	printf("setting voice number(hex) %x\n", voice_data.voice_num);
	printf("data: ");
	for (i=0; i<16; i++)
		printf("%x ", (unsigned int)voice_data.data[i]);
	printf("\n");
#endif

	/* check on voice_num range */
	if ((voice_data.voice_num >= MAX_FM_NOTES)
	    || (voice_data.voice_num < 0)) {
		printf("fm_set_voice(): voice number out of range\n");
		u.u_error = EFAULT;
		return;
	}
	cell_offset = voice_data.voice_num%3
		+ ((voice_data.voice_num / 3) << 3);

	/* set sound characteristic */
	op_cell_num = 0x20 + (char)cell_offset;
#ifdef DEBUG
	printf("op_cell for 20-35 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[0]);
	thirtymicrosec ();

	op_cell_num += 3;
#ifdef DEBUG
	printf("op_cell for 20-35 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[1]);
	thirtymicrosec ();

	/* set level/output */
	op_cell_num = 0x40 + (char)cell_offset;
#ifdef DEBUG
	printf("op_cell for 40-55 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[2]);
	thirtymicrosec ();

	op_cell_num += 3;
#ifdef DEBUG
	printf("op_cell for 40-55 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[3]);
	thirtymicrosec ();

	/* set Attack/Decay */
	op_cell_num = 0x60 + (char)cell_offset;
#ifdef DEBUG
	printf("op_cell for 60-75 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[4]);
	thirtymicrosec ();

	op_cell_num += 3;
#ifdef DEBUG
	printf("op_cell for 60-75 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[5]);
	thirtymicrosec ();

	/* set Sustain/Release */
	op_cell_num = 0x80 + (char)cell_offset;
#ifdef DEBUG
	printf("op_cell for 80-95 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[6]);
	thirtymicrosec ();

	op_cell_num += 3;
#ifdef DEBUG
	printf("op_cell for 80-95 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[7]);
	thirtymicrosec ();

	/* set Wave Select */
	op_cell_num = 0xE0 + (char)cell_offset;
#ifdef DEBUG
	printf("op_cell for E0-F5 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[8]);
	thirtymicrosec ();

	op_cell_num += 3;
#ifdef DEBUG
	printf("op_cell for E0-F5 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[9]);
	thirtymicrosec ();

	/* set Feedback/Selectivity */
	op_cell_num = (unsigned char)0xC0
		+ (unsigned char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for C0-C8 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[10]);
	thirtymicrosec ();
}


/*
 * set characteristics on an opcell
 */
static void
fm_set_opcell(usr_character)
sb_fm_character *usr_character;
{
	register unsigned char op_cell_num;
	int cell_offset;
	sb_fm_character voice_data;
	int i;

	/* copy in characteristics */
	if (copyin(usr_character, &voice_data, sizeof(sb_fm_character)) == -1)
	{
#ifdef DEBUG
		printf("bad address\n");
#endif
		u.u_error = EFAULT;
		return;
	}

	/* echo voice characteristics */
#ifdef DEBUG
	printf("setting opcell number %d\n", voice_data.voice_num);
	printf("setting opcell number(hex) %x\n", voice_data.voice_num);
	printf("data: ");
	for (i=0; i<8; i++)
		printf("%x ", (unsigned int)voice_data.data[i]);
	printf("\n");
#endif

	/* check on opcell range */
	if ((voice_data.voice_num >= 2*MAX_FM_NOTES)
	    || (voice_data.voice_num < 0)) {
		printf("opcell number out of range\n");
		u.u_error = EFAULT;
		return;
	}
	
	/* set sound characteristic */
	op_cell_num = 0x20 + (char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for 20-35 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[0]);
	thirtymicrosec ();

	/* set level/output */
	op_cell_num = 0x40 + (char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for 40-55 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[1]);
	thirtymicrosec ();

	/* set Attack/Decay */
	op_cell_num = 0x60 + (char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for 60-75 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[2]);
	thirtymicrosec ();

	/* set Sustain/Release */
	op_cell_num = 0x80 + (char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for 80-95 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[3]);
	thirtymicrosec ();

	/* set Wave Select */
	op_cell_num = 0xE0 + (char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for E0-F5 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[4]);
	thirtymicrosec ();

	/* set Feedback/Selectivity */
	op_cell_num = (unsigned char)0xC0
		+ (unsigned char)voice_data.voice_num;
#ifdef DEBUG
	printf("op_cell for C0-C8 = %x\n", op_cell_num);
#endif
	outb(FM_SELECT, op_cell_num);
	sb_tenmicrosec();
	outb(FM_REG, voice_data.data[5]);
	thirtymicrosec ();
}

/*
 * set the register which contains the keyon/off for rhythm, and depth flags
 */
static void
fm_set_rhythm(new_rhythm)
int new_rhythm;
{
	/* herf it in */
	outb(FM_SELECT, 0xBD);
	sb_tenmicrosec();
	outb(FM_REG, lobyte(new_rhythm));
	thirtymicrosec ();
}


/*
 * The only control for the FM chips.  The rest belongs in user code.
 */
static void
fm_ioctl(cmd, arg1, arg2)
int cmd;
caddr_t arg1, arg2;
{
	switch(cmd) {
        case FM_IOCTL_RESET:
		fm_reset();
		break;
        case FM_IOCTL_NOTE_ON:
		fm_key_on((int)arg1);
		break;
        case FM_IOCTL_NOTE_OFF:
		fm_key_off((int)arg1);
		break;
        case FM_IOCTL_SET_VOICE:
		fm_set_voice((sb_fm_character *)arg1);
		break;
        case FM_IOCTL_SET_OPCELL:
		fm_set_opcell((sb_fm_character *)arg1);
		break;
        case FM_IOCTL_SET_RHYTHM:
		fm_set_rhythm((int)arg1);
		break;
        default:
		break;
	}
}

 
/*
 * multiplex ioctl to different sub-devices (minor numbers)
 */
int
sbioctl(dev, cmd, arg1, arg2)
int dev;
int cmd;
caddr_t arg1, arg2;
{
    switch (minor (dev)) {
    case SB_CMS_NUM:
            printf("sbioctl cms\n");
            break;
    case SB_FM_NUM:
            fm_ioctl(cmd, arg1, arg2);
            break;
    case SB_DSP_NUM:
            dsp_ioctl(cmd, arg1, arg2);
            break;
    default:
            u.u_error = ENXIO;
	    break;
    }
    return(0);
}

