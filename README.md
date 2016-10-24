genwqe-user
===========

GenWQE (Generic Work Queue Engine) software supports the IBM PCIe3 FPGA or CAPI Compression Accelerator Adapters to speed up processing of the DEFLATE compression algorithm. This repository contains the source code to test, maintain and update the GenWQE PCIe/CAPI cards. Furthermore a zlib version with hardware acceleration is provided to do zlib style compression/decompression according to [RFC1950](https://www.ietf.org/rfc/rfc1950.txt), [RFC1951](https://www.ietf.org/rfc/rfc1951.txt) and [RFC1952](https://www.ietf.org/rfc/rfc1952.txt). This can be used as alternative to the traditional software zlib.

The GenWQE PCIe or CAPI cards can currently be acquired as options to the
latest IBM System p machines:
+ EJ12 full-height: [PCIe3 FPGA Compression Accelerator Adapter (FC EJ12; CCIN 59AB)](http://www-01.ibm.com/support/knowledgecenter/POWER8/p8hcd/fcej12.htm?cp=POWER8%2F3-3-9-1-1-44)
+ EJ13 low-profile: [PCIe3 LP FPGA Compression Accelerator Adapter (FC EJ13; CCIN 59AB)](http://www-01.ibm.com/support/knowledgecenter/POWER8/p8hcd/fcej13.htm?cp=POWER8%2F1-2-9-1-1-50&lang=en)

And here the CAPI version of the adapter:
+ EJ1A full-heigh and EJ1B low-profile: [CAPI Compression Accelerator Adapter (FC EJ1A and EJ1B; CCIN 2CF0)](http://www.ibm.com/support/knowledgecenter/POWER8/p8hcd/fcej1a.htm)

If you like to contribute to this project, please fill out and sign one of our contributor license agreements to be found in /licenses and send this back to us before sending us contributions.

Additional documentation can be found at the  [IBM Knowledgecenter](http://www-01.ibm.com/support/knowledgecenter/linuxonibm/liabt/liabtkickoff.htm).

A programming and usage guide for the hardware accelerated zlib can be downloaded here: [Generic Work Queue Engine (GenWQE) Application Programming Guide](https://www.ibm.com/developerworks/community/blogs/fe313521-2e95-46f2-817d-44a4f27eba32/entry/Generic_Work_Queue_Engine_GenWQE_Application_Programming_Guide?lang=en).

The User's guide for the CAPI Compression Accelerator Adapter (FC EJ1A and EJ1B; CCIN 2CF0) can be found here: [CAPI accelerated GZIP Compression Adapter User’s guide](https://www.ibm.com/developerworks/community/wikis/home?lang=en#!/wiki/W51a7ffcf4dfd_4b40_9d82_446ebc23c550/page/CAPI%20accelerated%20GZIP%20Compression%20Adapter%20User’s%20guide).

Possible distributors: If you take a snapshot of this into one of your releases, please let us know such that we can sync up testing. Thanks.
