# Genwqe-user

The Generic Work Queue User code contains the User level source code for the
Generic Work Driver.

## Software Structure:
       ./lib
                README
                Makefile
                ddcb_card.c             Wrapper on top of ddcb_capi.c and libcard.c
                hardware.c              libz function's for both Hardware Cards.
                libddcb.c               Functions on top of ddcb_card.c
                libzHW.c                De/Compression supporting RFC1950, RFC1951 and RFC1952
                hw_defs.h
                wrapper.c               Wrapper for soft and Hardware zlib
                wrapper.h
                ddcb_capi.c             Low level API for CAPI accelerator (1)
                inflate.c               Zlib Inflate functions
                deflate.c               Zlib deflate functions
                libcard.c               Low level API for Genwqe Card.
                card_defs.h             Some Macros to debug libcard.c
                libzADC.map             Map file to build so files
                software.c              Interface to call system libz
        ./include
                ddcb.h
                deflate_ddcb.h
                deflate_fifo.h
                genwqe_vpd.h
                libcard.h               for libcard.c
                libddcb.h
                libzHW.h
                memcopy_ddcb.h
                zaddons.h
                linux/......            Some file to remove later
        ./tools
                Makefile
                genwqe_echo.c           Test program to send "echo" command to Genwqe
                genwqe_gzip.c           Sample program for gzip using the Genwqe
                genwqe_memcopy.c        Test Program for Memcopy using Genwqe
                genwqe_tools.h
                genwqe_update.c         Tool to update Genwqe Flash
                genwqe_vpd_common.c     Helper util. for genwqe_vpd_common.c
                zlib_mt_perf.c          Sample program for inflate / deflate.
                force_cpu.c             Helper util to select / force / pin CPU
                force_cpu.h

## (1) For PowerPC only:
You need to get the Capi API lib
[Libcxl on GitHub](https://github.com/ibm-capi/libcxl.git)
clone the files to the same directoy as you cloned genwqe-tools.
and run "make" in the libcxl dir before compile genwqe-tools.
Change to "cd ../genwqe-tools" and run "make".

see also:
        [IBM Knowledgecenter](
        http://www-01.ibm.com/support/knowledgecenter/linuxonibm/liabt/liabtkickoff.htm)
