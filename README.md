genwqe-user
===========

GenWQE (Generic Work Queue Engine) is a PCIe acceleration card. This
repository contains the source code to test, maintain and update the
GenWQE PCIe card. Furthermore a zlib version with hardware
acceleration is provided to do zlib style compression/decompression
according to RFC1950, RFC1951 and RFC1952. This can be used as
alternative to the traditional software zlib.

The GenWQE PCIe card can currently be aquired as an option to the
latest IBM System p machines:
+ EJ12 regular height: [PCIe3 FPGA Accelerator Adapter (FC EJ12; CCIN 59AB)](http://www-01.ibm.com/support/knowledgecenter/POWER8/p8hcd/fcej12.htm?cp=POWER8%2F3-3-9-1-1-44)
+ EJ13 low profile: [PCIe3 LP FPGA Accelerator Adapter (FC EJ13; CCIN 59AB)](http://www-01.ibm.com/support/knowledgecenter/POWER8/p8hcd/fcej13.htm?cp=POWER8%2F1-2-9-1-1-50&lang=en)

On modern IBM System p servers, the accelerator card can use the new
CAPI interface.  Install the [libcxl](https://github.com/ibm-capi/libcxl.git)
library into the toplevel ````genwqe-user```` directory and build the library
via ````make```` before compiling the genwqe tools.

If you like to contribute to this project, please fill out and sign
one of our contributor license agreements to be found in /licenses and
send this back to us before sending us contributions.

Additional documentation can be found at the  [IBM Knowledgecenter](http://www-01.ibm.com/support/knowledgecenter/linuxonibm/liabt/liabtkickoff.htm). A programming and usage guide for the hardware accelerated zlib can be downloaded here: [Generic Work Queue Engine (GenWQE) Application Programming Guide](https://www.ibm.com/developerworks/community/blogs/fe313521-2e95-46f2-817d-44a4f27eba32/entry/Generic_Work_Queue_Engine_GenWQE_Application_Programming_Guide?lang=en).

Possible distributors: If you take a snapshot of this into one of your
releases, please let us know such that we can sync up testing. Thanks.
