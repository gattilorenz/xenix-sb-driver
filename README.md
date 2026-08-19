# SoundBlaster driver for SCO Xenix 2.3.4

This is a SoundBlaster driver for SCO Xenix 2.3.4 based on the driver for various System V variants by Brian Smith and Lance Norskog (https://groups.google.com/g/alt.sources/c/AyJFP8bbj9A/m/4NXXNIQEHwAJ), ported by Claude.

The current implementation is "hardcoded" for IRQ 10 a (in the sense that the boot-time device table will always print that) and DMA channel 1 (`#define SB_DMA_CHAN 1` in `sb.c`).

See also [the install guide](install.md)
