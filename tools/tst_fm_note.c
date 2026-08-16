/*
 * Copyrighted as an unpublished work.
 * (c) Copyright 1991 Brian Smith
 * All rights reserved.
 *
 * Read the LICENSE file for details on distribution and use.
 *
 */

#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/sb.h>
#include <stdio.h>
#include <memory.h>

void handler()
{
    return;
}

int main(argc, argv)
int argc;
char **argv;
{
    int fd;
    int rc;
    int i;
    int note;
    int fnum;
    int block;
    static unsigned char instrument_buf[16] = {
        0x11, 0x01, 0x8a, 0x40,
        0xf1, 0xf1, 0x11, 0xb3,
        0x00, 0x00, 0x06, 0x00,
        0x00, 0x00, 0x00, 0x00 
    };
    sb_fm_character note_character;

    sigset(SIGINT, handler);

    fd = open("/dev/sbfm", O_WRONLY);
    if (fd == -1)
    {
        perror("opening fm device");
        return(-1);
    }

    /* test reset */
    rc = ioctl(fd, FM_IOCTL_RESET);
    if (rc == -1)
    {
        perror("fm chips reset");
        exit(-1);
    }

    /* test setting an instrument */
    for (i=0 ; i<9; i++)
    {
        /* set instrument characteristics */
        note_character.voice_num = i;
        memset(note_character.data, (char)0, 16);
        memcpy(note_character.data, instrument_buf, 16);
        rc = ioctl(fd, FM_IOCTL_SET_VOICE, (int)&note_character);
        if (rc == -1)
        {
            perror("fm chips voice set");
            exit(-1);
        }

        /* set note to play */
        fnum = 686;
        block = i;
        note_num(note) = i;
        fnum_low(note) = fnum & 0xFF;
        keyon_blk_fnum(note) = 0;
        keyon_blk_fnum(note) |= 1<<5;                   /* KEYON bit */
        keyon_blk_fnum(note) |= (block & 7) << 2;       /* block/octave */
        keyon_blk_fnum(note) |= (fnum & 0x3FF) >> 8;    /* top 2 bits of fnum */

        /* test note on/off */
        rc = ioctl(fd, FM_IOCTL_NOTE_ON, note);
        if (rc == -1)
        {
            perror("fm chips voice on");
            exit(-1);
        }
    }

    /* wait for ^C */
    sigpause(SIGINT);
    close(fd);
    return(0);
}
