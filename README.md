# SoundBlaster driver for SCO Xenix 2.3.4

This is a SoundBlaster driver for SCO Xenix 2.3.4 based on the driver for various System V variants by Brian Smith and Lance Norskog (https://groups.google.com/g/alt.sources/c/AyJFP8bbj9A/m/4NXXNIQEHwAJ).

The current implementation is hardcoded for IRQ 10 and DMA 1. For midi to work, you need to have a synth connected to the midi port (a MT-32 works, but beware that midi channel 1 is by default not used). Only tested on 86Box.

Install instructions will follow...
