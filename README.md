Genwqe-user
===========

GenWQE is a PCIe accleration card. This repository contains the source code to 
build a generic user level work queue for zlib style compression/decompression 
according to RFC1950, RFC1951 and RFC1952.

    /lib
        card_defs.h         Some Macros to debug libcard.c
        ddcb_capi.c         Low level API for CAPI accelerator (PPC only, see note below)
        ddcb_card.c         Wrapper on top of ddcb_capi.c and libcard.c
        deflate.c           Zlib deflate functions
        hardware.c          libz function's for both Hardware Cards
        hw_defs.h
        inflate.c           Zlib Inflate functions
        libcard.c           Low level API for Genwqe Card
        libddcb.c           Functions on top of ddcb_card.c
        libzADC.map         Map file to build so files
        libzHW.c            De/Compression supporting RFC1950, RFC1951 and RFC1952
        software.c          Interface to call system libz
        wrapper.c           Wrapper for soft and Hardware zlib
        wrapper.h

    /include
        ddcb.h
        deflate_ddcb.h
        deflate_fifo.h
        genwqe_vpd.h
        libcard.h
        libddcb.h
        libzHW.h
        memcopy_ddcb.h
        zaddons.h

    /tools
        force_cpu.c         Helper util to select / force / pin CPU
        force_cpu.h
        genwqe_echo.c       Test program to send "echo" command to Genwqe
        genwqe_gzip.c       Sample program for gzip using the Genwqe
        genwqe_memcopy.c    Test Program for Memcopy using Genwqe
        genwqe_tools.h
        genwqe_update.c     Tool to update Genwqe flash
        genwqe_vpd_common.c Helper utilities
        zlib_mt_perf.c      Sample program for inflate / deflate


On modern PowerPC server the accelerator card can use the new CAPI interface.
Install the [Libcxl](https://github.com/ibm-capi/libcxl.git) library into the toplevel ````genwqe-user```` directory and build the library via ````make```` before compiling the genwqe tools.

Additional documentation can be found at the  [IBM Knowledgecenter](http://www-01.ibm.com/support/knowledgecenter/linuxonibm/liabt/liabtkickoff.htm).
