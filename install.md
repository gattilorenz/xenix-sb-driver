# SoundBlaster Driver for SCO Xenix 386 2.3.4 — Install Notes


### Build

Run `compile.sh` to produce `sb.o`. There should be no warnings.
Then

	cp sb.o /usr/sys/conf/sb.o

### Register the driver

#### Major number

Find the next major number available for a device:

	configure -j NEXTMAJOR

On my system, this was 67; it might be different on yours, adjust the rest of the instructions accordingly.

#### Create devices in `/dev`

Use the command
	`/etc/mknod /dev/sbcms c 67 0`
to create the first device node; add the other devices with more mknode
according to this scheme (using the appropriate filename, major and minor number):

| Node | Major | Minor | Purpose |
|------|-------|-------|---------|
| `/dev/sbcms`  | 67 | 0 | C/MS chip — *not implemented, `open` returns ENXIO* |
| `/dev/sbfm`   | 67 | 1 | OPL2 FM synth |
| `/dev/sbdsp`  | 67 | 2 | Digital audio (DAC/ADC) |
| `/dev/sbmidi` | 67 | 3 | MIDI I/O |

#### Find the correct interrupt vector

My board is configured on **IRQ10** in 86Box. 
In Xenix, IRQ0–7 are recorded as vectors 0–7, but slave-8259 IRQ8–15 as vectors **24–31**.

So, IRQ10 is vector 26. If you run `./configure -t -v 26` it should return with exit code 1, 
meaning it's free (if not: you have a conflict to resolve...).

#### Driver registration with configure

	cd /usr/sys/conf
	./configure -a sbopen sbclose sbread sbwrite sbioctl sbinit sbintr \
		-c -m 67 -v 26 -l 5 #note Major 67 and IRQ 26
                
This writes the `master` file entry:

	sb       1  0137 004     sb  0   0  67    1   5    32    0     0   0

and adds `sb 1` to `xenixconf`, regenerates `c.asm`/`space.inc`/`c.o`/
`space.o` so that `c.asm` contains `DD _sbinit` in the init table,
`DD _sbintr` in the interrupt-vector table, and the five `cdevsw` entries
at major 67. Verify with:

	grep sb /usr/sys/conf/master
	grep -n sb /usr/sys/conf/c.asm

### Link the driver into the kernel

Modify the `/usr/sys/conf/link_xenix` script, so that `sb.o` is added to the object list:

    ld -Rd 1000 -D 18 -B 20 -A 0 -i -u start -o xenix \
        start.o c.o ../inet/3comA/e501li.o ... ../inet/libinet.a \
        uts.o oem.o space.o tab.o kid.o class.o sb.o \
        ../ml/libml.a ../str/libstr.a ../mdep/libmdep.a ../sys/libsys.a \
        ../xnet/libxnstub.a ../io/libio.a ../io/libiostub.a

Notice that it must come before the `lib*.a` line. 

Then, link the new kernel:

    ./link_xenix

Verify the driver really made it in:

    nm xenix | grep ' _sb'      # entry points, all as T (text)
    nm xenix | grep -c ' U '    # must be 0 — no undefined symbols

The first should list `_sbinit _sbopen _sbclose _sbread _sbwrite _sbioctl
_sbintr` as `T`, plus the driver's own `D` data (`_sbuf`, `_sb_owns_dma`,
`_sb_didprep`, `_sb_didstart`). If `_sb*` is missing entirely, `sb.o` is not
in `link_xenix`; if the symbols show as `U`, the object list is ordered
wrongly.

### Install the new kernel

Copy the kernel just created to `/` with a new name:

    cp /usr/sys/conf/xenix /xenix.new

Boot it by typing `xenix.new` at the boot prompt. A power-cycle always falls back to `/xenix`.


### Boot test

If everything went well, the device table at boot now contains the `sb`
line:

    %sb       0x0220-0x022F  32      1      type=SoundBlaster unit=0

The vector prints as `32` because that column is octal — that is IRQ10.
The comment reads `type=SoundBlaster unit=0` rather than `not found`, which
means `dsp_reset()` got its `0xAA` back: the board is present and
responding. If it were absent, the driver would disable itself (`open`
returns ENXIO) rather than hang.

~~**If you have TCP/IP installed** you might need to give the root password and 
**boot in single-user/maintenance mode** to avoid a TCP-startup panic. This sporadically
occured in my system even before the sb driver, so it's probably not the driver itself,
but happens 100% of times with the driver installed.~~ The kernel panic is now elusive. See what works for you...

### Testing once it boots

You can compile `play_mid.c`, `play_wav.c` (requiring a midi or wav file as the first argument)
and `tst_fm_note.c` (run with no arguments) in the `tools` folder of this repo to test whether it works.
