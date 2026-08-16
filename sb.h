/*
 * Copyrighted as an unpublished work.
 * (c) Copyright 1991 Brian Smith
 * All rights reserved.
 *
 * Read the LICENSE file for details on distribution and use.
 *
 */

/*
 * Xenix 386 port: INKERNEL is this driver's own private gating macro
 * (not a Xenix or system macro) used below to expose the kernel-only
 * struct/globals at the bottom of this file. We define it here rather
 * than via -DINKERNEL on the cc command line, and we drop the
 * <sys/immu.h> include entirely: NBPP is never actually referenced
 * anywhere else in this driver, and <sys/immu.h> does not exist on
 * this system (neither /usr/sys/h nor /usr/include/sys has it).
 */
#define INKERNEL 1

#if !defined(TRUE) || !defined(FALSE)
#define FALSE 0
#define TRUE  1
#endif

/* minor numbers */
#define SB_CMS_NUM  0
#define SB_FM_NUM   1
#define SB_DSP_NUM  2
#define SB_MIDI_NUM 3

/* These are hard wired for speed */
#define SB_DMA_CHAN 1
#define SB_IO_PORT 0x220

/*
 * Interrupt vector, as numbered in /usr/sys/conf/master (where the vector
 * column is OCTAL). Xenix numbers master-8259 IRQ0-7 as vectors 0-7, and
 * slave-8259 IRQ8-15 as vectors 24-31. The board is strapped to IRQ10 in
 * 86Box, so its vector is 24 + (10 - 8) = 26, written 32 in the master
 * file. This value is only used for the boot-time printcfg() line; the
 * actual interrupt binding comes from master via configure(ADM).
 */
#define SB_VECTOR 26

/* C/MS (not supported) */
#define CMS_DATA1   (SB_IO_PORT + 0x00)
#define CMS_REG1    (SB_IO_PORT + 0x01)
#define CMS_DATA2   (SB_IO_PORT + 0x02)
#define CMS_REG2    (SB_IO_PORT + 0x03)

/* FM Chips */
#define FM_SELECT   (SB_IO_PORT + 0x08)
#define FM_REG      (SB_IO_PORT + 0x09)
#define MAX_FM_NOTES 9

/* DSP (DAC and ADC) Chip(s) */
#define DSP_RESET   (SB_IO_PORT + 0x06)
#define DSP_RDDATA  (SB_IO_PORT + 0x0A)
#define DSP_WRDATA  (SB_IO_PORT + 0x0C)
#define DSP_COMMAND (SB_IO_PORT + 0x0C)
#define DSP_STATUS  (SB_IO_PORT + 0x0C)
#define DSP_RDAVAIL (SB_IO_PORT + 0x0E)

/* compression types */
#define ADCPM_8     0
#define ADCPM_4     1
#define ADCPM_2_6   2
#define ADCPM_2     3

/* ioctl numbers for DSP */
#define DSP_IOCTL_RESET 00
#define DSP_IOCTL_SPEED 01
#define DSP_IOCTL_VOICE 02
#define DSP_IOCTL_FLUSH 03

/* ioctl numbers for FM */
#define FM_IOCTL_RESET      00
#define FM_IOCTL_NOTE_ON    01
#define FM_IOCTL_NOTE_OFF   02
#define FM_IOCTL_SET_VOICE  03
#define FM_IOCTL_SET_OPCELL 04
#define FM_IOCTL_SET_RHYTHM 05

#define	DSPCMD_TIME	0x40
#define	DSPCMD_SPKON	0xd1
#define	DSPCMD_SPKOFF	0xd3
#define	DSPCMD_MIDIIN	0x31
#define	DSPCMD_MIDIOUT	0x38
#define DSPCMD_READ	0x24
#define DSPCMD_WRITE	0x14

/* struct for setting a note/voice/key on */
typedef int sb_fm_note;
#define note_num(X) (((unsigned char *)&X)[0])
#define fnum_low(X) (((unsigned char *)&X)[1])
#define keyon_blk_fnum(X) (((unsigned char *)&X)[2])

typedef struct {
    unsigned char   voice_num;
    unsigned char   data[16];
} sb_fm_character;

#ifdef INKERNEL
struct sb_stat_type {
    unsigned char   cms_open;           /* whether in read/write */
    unsigned char   cms_waiting;        /* number of procs waiting to open */
    unsigned char   fm_open;            /* whether in read/write */
    unsigned char   fm_waiting;         /* number of procs waiting to open */
    unsigned char   dsp_open_for_reading;
    unsigned char   dsp_open_for_writing;
    unsigned int    dsp_speed;          /* sample read/write HZ */
    unsigned char   dsp_compression;    /* compression protocol */
    unsigned char   midi_open_for_reading;
    unsigned char   midi_open_for_writing;
    

};

unsigned int midi_in_buf[256];
unsigned char midi_in_in, midi_in_out;
int midi_in_wanted;

int midi_in_waiting_for_first_io;
int midi_out_waiting_for_first_io;
int midi_in_last_time;
int midi_out_last_time;

#define MIDI_OUT_CHUNK 60
unsigned char midi_out_buf[MIDI_OUT_CHUNK];
int midi_out_used;

#endif

