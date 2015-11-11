#
# Copyright 2015, International Business Machines
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include ../config.mk

instdir ?= /opt/genwqe
libversion = $(shell git describe --abbrev=4 --dirty --always --tags)
zlibver=1.2.8

CFLAGS += -fPIC -fno-strict-aliasing \
	-DCONFIG_ZLIB_PATH=\"/opt/genwqe/lib/libz.so\"

### Accelerated libz implemenation (Accelerated Data Compression/ADC)
libname=libzADC
projs = $(libname).a $(libname)-$(libversion).so $(libname).so
src = wrapper.c hardware.c software.c
objs = __libzHW.o __libcard.o __libDDCB.o $(src:.c=.o)

### libzHW
src0 = libzHW.c inflate.c deflate.c
libname0 = libzHW
proj0 = $(libname0).a $(libname0)-$(libversion).so $(libname0).so
objs0 = $(src0:.c=.o)

### libcard
src1 = libcard.c
libname1 = libcard
proj1 = $(libname1).a $(libname1)-$(libversion).so $(libname1).so
objs1 = $(src1:.c=.o)

###libDDCB
src2 += libddcb.c ddcb_card.c

ifneq ($(CONFIG_LIBCXL_PATH),)
src2 += ddcb_capi.c
endif

libname2 = libDDCB
proj2 = $(libname2).a $(libname2)-$(libversion).so $(libname2).so
objs2 = $(src2:.c=.o)

projs += $(proj0) $(proj1) $(proj2)

all: ..check_libcxl $(projs)

.PHONY: ..check_libcxl
..check_libcxl:
ifneq ($(CONFIG_LIBCXL_PATH),)
	@printf "\t[INFO]\tUsing $(libcxl_a).\n"
	@if [ ! -f $(libcxl_a) ]; then 					\
	  printf  "\t[ERROR]\t$(libcxl_a) not found!\n"; 		\
	  echo;								\
	  echo "  Please clone libcxl git before continuing. E.g.:";	\
	  echo "    git clone https://github.com/ibm-capi/libcxl";	\
	  echo;                                                         \
	  exit 1; \
	fi
else
	# @printf "\t[INFO]\tNot using libcxl.a.\n"
endif

### libzHW
__$(libname0).o: $(objs0)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname0).a: __$(libname0).o
	$(AR) rcs $(ARFLAGS) $@ $^

$(libname0).so: $(libname0)-$(libversion).so
	ln -sf $< $@

$(libname0)-$(libversion).so: __$(libname0).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ $^

### libcard
__$(libname1).o: $(objs1)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname1).a: __$(libname1).o
	$(AR) rcs $(ARFLAGS) $@ $^

$(libname1).so: $(libname1)-$(libversion).so
	ln -sf $< $@

$(libname1)-$(libversion).so: __$(libname1).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ $^

### libDDCB
__$(libname2).o: $(objs2) $(libcxl_a)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname2).a: __$(libname2).o
	$(AR) rcs $(ARFLAGS) $@ $^

$(libname2).so: $(libname2)-$(libversion).so
	ln -sf $< $@

$(libname2)-$(libversion).so: __$(libname2).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ $^

### libzADC
__$(libname).o: $(objs)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname).a: __$(libname).o
	$(AR) rcs $@ $^

$(libname).so: $(libname)-$(libversion).so
	ln -sf $< $@

$(libname)-$(libversion).so: __$(libname).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ \
		 -Wl,--version-script=libzADC.map -o $@ $^

install: all install_zlib
	mkdir -p $(instdir)/lib
	cp $(projs) $(instdir)/lib

install_zlib: all
	mkdir -p $(instdir)/lib/genwqe
	cp -a $(projs) $(instdir)/lib/genwqe/
	(cd $(instdir)/lib/genwqe/ && \
		ln -sf $(libname).a libz.a && \
		ln -sf $(libname)-$(libversion).so libz.so && \
		ln -sf $(libname)-$(libversion).so libz.so.1)
	mkdir -p $(instdir)/include/genwqe
	cp -a ../include/zaddons.h $(instdir)/include/genwqe/

uninstall:
	@for f in $(projs) ; do				\
		echo "removing $(instdir)/lib/$$f ...";	\
		$(RM) $(instdir)/lib/$$f;		\
		echo "removing $(instdir)/genwqe/lib/$$f ...";	\
		$(RM) $(instdir)/lib/genwqe/$$f;		\
	done
	$(RM) -r $(instdir)/include/genwqe $(instdir)/lib/genwqe


# Special install rules for internal users
install_src:
	@mkdir -p $(GENWQETOOLSSRC)/lib
	cp *.c *.h Makefile $(GENWQETOOLSSRC)/lib

uninstall_src:
	$(RM) $(GENWQETOOLSSRC)/lib/*.c $(GENWQETOOLSSRC)/lib/*.h \
		$(GENWQETOOLSSRC)/lib/Makefile

# general things
%.o: %.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $< -o $@
	$(CC) -MM $(CPPFLAGS) $(CFLAGS) $< > $*.d

clean distclean:
	$(RM) *.o *.d $(projs) *.so

-include $(objs:.o=.d) $(objs0:.o=.d) $(objs1:.o=.d) $(objs2:.o=.d)
