# SoundBlaster driver for SCO Xenix 2.3.4

This is a SoundBlaster driver for SCO Xenix 2.3.4 based on the driver for various System V variants by Brian Smith and Lance Norskog (https://groups.google.com/g/alt.sources/c/AyJFP8bbj9A/m/4NXXNIQEHwAJ), ported by Claude.

The current implementation is "hardcoded" for IRQ 10 a (in the sense that the boot-time device table will always print that) and DMA channel 1 (`#define SB_DMA_CHAN 1` in `sb.c`).

For apps/play_mid to work, you need to have a synth connected to the midi port (a MT-32 works, but beware that midi channel 1 is by default not used and .mid files will sound wrong - call `play_mid` with `-h` to see a hack around that). Only tested on 86Box.

See also [the install guide](install.md)
