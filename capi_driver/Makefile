# Makefile for the GenWQE accelerator
#   physical and virtual function device drivers

ifneq ($(KERNELRELEASE),)

# kbuild part of makefile
include Kbuild

else

KDIR     ?= /lib/modules/$(shell uname -r)/build

all :
	@if [ ! -d $(KDIR) ]; then \
		echo "WARNING: Module build environment for $(shell uname -r) not installed." ; \
		echo "Directory $(KDIR) does not exist." ; \
		echo "GenWQE Driver could not be built!" ; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) SUBDIRS=$(shell pwd)

distclean clean:
	@$(RM) -r *.ko *.o .*.cmd .*.o.flags *.mod.c Module.symvers \
		modules.order *~ .tmp_versions
	$(MAKE) -C $(KDIR) SUBDIRS=$(shell pwd) clean

endif
