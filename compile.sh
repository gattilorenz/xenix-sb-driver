#	Flags below match CFLAGS in /usr/sys/conf/makefile, which is how
#	every other module in this kernel is built:
#	    CFLAGS = -O -M3 -Zp4 -Me -DM_KERNEL -DVPIX
#	-Me matters because cc's own default is -M3e, and passing a bare
#	-M3 would otherwise turn the extended keywords back off.
#	-K drops stack probes, which kernel code must not have.
cc -O -M3 -Me -Zp4 -K -DM_KERNEL -DVPIX -I/usr/sys/h -c sb.c
