genwqe-user
===========

GenWQE (Generic Work Queue Engine) is a PCIe acceleration card. This
repository contains the source code to test, maintain and update the
GenWQE PCIe card. Furthermore a zlib version with hardware
acceleration is provided to do zlib style compression/decompression
according to RFC1950, RFC1951 and RFC1952. This can be used as
alternative to the traditional software zlib. The GenWQE PCIe card can
currently be aquired as an option to the latest IBM System p machines (see also here [PCIe3 LP FPGA Accelerator Adapter (FC EJ13; CCIN 59AB)](http://www-01.ibm.com/support/knowledgecenter/8247-22L/p8hcd/fcej13.htm?lang=en)).

    /lib
        card_defs.h         Some Macros to debug libcard.c
        ddcb_capi.c         Low level API for CAPI accelerator (PPC only, see note below)
        ddcb_card.c         Wrapper on top of ddcb_capi.c and libcard.c
        deflate.c           Zlib deflate functions
        hardware.c          libz functions
        hw_defs.h
        inflate.c           Zlib inflate functions
        libcard.c           Low level API for GenWQE card
        libddcb.c           Functions on top of ddcb_card.c
        libzADC.map         Map file to build so files
        libzHW.c            De/compression supporting RFC1950, RFC1951 and RFC1952
        software.c          Interface to call system libz
        wrapper.c           Wrapper for soft- and hardware-zlib
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
        force_cpu.c         Helper util to select/force/pin CPU
        force_cpu.h
        genwqe_echo.c       Test program to send "echo" command to GenWQE
        genwqe_gzip.c       Sample program for gzip using the GenWQE
        genwqe_memcopy.c    Test Program for Memcopy using GenWQE
        genwqe_tools.h
        genwqe_update.c     Tool to update GenWQE flash
        genwqe_vpd_common.c Helper utilities
        zlib_mt_perf.c      Sample program for inflate/deflate

    /licenses
        cla-corporate.txt
        cla-individual.txt

    /spec
        genwqe-libz.spec    Spec file for building the tools RPM
        genwqe-tools.spec   Spec file for building the libz RPM


On modern PowerPC server the accelerator card can use the new CAPI interface.
Install the [libcxl](https://github.com/ibm-capi/libcxl.git) library into the toplevel ````genwqe-user```` directory and build the library via ````make```` before compiling the genwqe tools.

If you like to contribute to this project, please fill out and sign
one of our contributor license agreements to be found in /licenses and
send this back to us before sending us contributions.

Additional documentation can be found at the  [IBM Knowledgecenter](http://www-01.ibm.com/support/knowledgecenter/linuxonibm/liabt/liabtkickoff.htm). A programming and usage guide for the hardware accelerated zlib can be downloaded here: [Generic Work Queue Engine (GenWQE) Application Programming Guide](https://www.ibm.com/developerworks/community/blogs/fe313521-2e95-46f2-817d-44a4f27eba32/entry/Generic_Work_Queue_Engine_GenWQE_Application_Programming_Guide?lang=en).
