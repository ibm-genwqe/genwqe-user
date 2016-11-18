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

zlibver=1.2.8
libversion = $(VERSION)

CFLAGS += -fPIC -fno-strict-aliasing
LDLIBS += -lpthread -ldl -lrt

# If we use libcxl, we need to reflect that to avoid underlinkage
ifeq ($(WITH_LIBCXL),1)
ifneq ($(BUNDLE_LIBCXL),1)
LDLIBS += -lcxl
endif
endif

### Accelerated libz implementation (Accelerated Data Compression/ADC)
libname=libzADC
projA = $(libname).a \
	$(libname).so \
	$(libname).so.$(MAJOR_VERS) \
	$(libname).so.$(libversion)

src = wrapper.c hardware.c software.c
objs = __libzHW.o __libcard.o __libDDCB.o $(src:.c=.o)

### libzHW
src0 = libzHW.c inflate.c deflate.c
libname0 = libzHW
proj0 = $(libname0).a $(libname0).so.$(libversion) $(libname0).so
objs0 = $(src0:.c=.o)

### libcard
src1 = libcard.c
libname1 = libcard
proj1 = $(libname1).a $(libname1).so.$(libversion) $(libname1).so
objs1 = $(src1:.c=.o)

### libDDCB requires libcxl for CAPI support
src2 += libddcb.c ddcb_card.c

# ddcb_capi is only used with LIBCXL support.
ifdef WITH_LIBCXL
src2 += ddcb_capi.c
endif

libname2 = libDDCB
proj2 = $(libname2).a $(libname2).so.$(libversion) $(libname2).so
objs2 = $(src2:.c=.o)

projs += $(projA) $(proj0) $(proj1) $(proj2)

all: $(projs)

ifdef WITH_LIBCXL

ddcb_capi.o: $(libcxl_a)

$(libcxl_a): ..check_libcxl
	$(MAKE) -C $(CONFIG_LIBCXL_PATH)

.PHONY: ..check_libcxl
..check_libcxl:
	@if [ $(BUNDLE_LIBCXL) -eq 1 ]; then				\
	  if [ ! -d $(CONFIG_LIBCXL_PATH) ]; then 			\
	    printf  "\t[ERROR]\t$(CONFIG_LIBCXL_PATH) not found!\n";	\
	    echo;							\
	    echo "  Please clone libcxl git before continuing:";	\
	    echo "    git clone https://github.com/ibm-capi/libcxl";	\
	    echo;							\
	    exit 1;							\
	  fi;								\
	  if [ ! -f $(CONFIG_LIBCXL_PATH)/include/misc/cxl.h ]; then	\
	    printf  "\t[ERROR]\t$(CONFIG_LIBCXL_PATH)/include/misc/cxl.h not found!\n";\
	    echo;							\
	    echo "  Please ensure you are having up-todate";		\
	    echo "  kernel-headers when building the code.";		\
	    echo "  When using pslse, get a recent copy of the file";	\
	    echo "  from kernel.org and copy the file manually to";	\
	    echo "  $(CONFIG_LIBCXL_PATH)/include/misc/cxl.h.";		\
	    echo;							\
	    exit 1;							\
	  fi								\
	fi

endif	# WITH_LIBCXL

### libzHW
__$(libname0).o: $(objs0)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname0).a: __$(libname0).o
	$(AR) rcs $(ARFLAGS) $@ $^

$(libname0).so: $(libname0).so.$(libversion)
	ln -sf $< $@

$(libname0).so.$(libversion): __$(libname0).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ $^

### libcard
__$(libname1).o: $(objs1)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname1).a: __$(libname1).o
	$(AR) rcs $(ARFLAGS) $@ $^

$(libname1).so: $(libname1).so.$(libversion)
	ln -sf $< $@

$(libname1).so.$(libversion): __$(libname1).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ $^

### libDDCB
__$(libname2).o: $(objs2) $(libcxl_a)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname2).a: __$(libname2).o
	$(AR) rcs $(ARFLAGS) $@ $^

$(libname2).so: $(libname2).so.$(libversion)
	ln -sf $< $@

$(libname2).so.$(libversion): __$(libname2).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ $^

### libzADC
__$(libname).o: $(objs) $(libz_a)
	$(LD) $(XLDFLAGS) -r -o $@ $^

$(libname).a: __$(libname).o
	$(AR) rcs $@ $^

$(libname).so:  $(libname).so.$(libversion)
	ln -sf $< $@

$(libname).so.$(MAJOR_VERS): $(libname).so.$(libversion)
	ln -sf $< $@

$(libname).so.$(libversion): __$(libname).o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$(libname).so.$(MAJOR_VERS) \
		 -Wl,--version-script=libzADC.map -o $@ $^ $(LDLIBS)

# Produce z_ prefixed version of software zlib. We need this when we
# want to include libz statially instead of using dlopen/dlsym to use
# it.
#
# Special version of libz.a which has z_ prefixed function
# names. Required for software zlib fallback in case of small buffers
# and accelerator unavailability.
#
libz_prefixed.o:

zlib_objs = $(CONFIG_LIBZ_PATH)/adler32.lo  \
	    $(CONFIG_LIBZ_PATH)/infback.lo  \
	    $(CONFIG_LIBZ_PATH)/compress.lo \
	    $(CONFIG_LIBZ_PATH)/gzclose.lo  \
	    $(CONFIG_LIBZ_PATH)/inffast.lo  \
	    $(CONFIG_LIBZ_PATH)/trees.lo    \
	    $(CONFIG_LIBZ_PATH)/crc32.lo    \
	    $(CONFIG_LIBZ_PATH)/gzlib.lo    \
	    $(CONFIG_LIBZ_PATH)/inflate.lo  \
	    $(CONFIG_LIBZ_PATH)/uncompr.lo  \
	    $(CONFIG_LIBZ_PATH)/deflate.lo  \
	    $(CONFIG_LIBZ_PATH)/gzread.lo   \
	    $(CONFIG_LIBZ_PATH)/inftrees.lo \
	    $(CONFIG_LIBZ_PATH)/zutil.lo    \
	    $(CONFIG_LIBZ_PATH)/gzwrite.lo

libz_prefixed.o: libz.o
	$(OBJCOPY) --prefix-symbols=z_ $< $@

libz.o: $(CONFIG_LIBZ_PATH)/libz.so
	$(LD) $(XLDFLAGS) -r -o $@ $(zlib_objs)

install: install_zlib

install_zlib: all
	mkdir -p $(LIB_INSTALL_PATH)
	cp -auv $(projA) $(LIB_INSTALL_PATH)
	(cd $(LIB_INSTALL_PATH) && \
		ln -sf $(libname).a libz.a && \
		ln -sf $(libname).so.$(libversion) libz.so && \
		ln -sf $(libname).so.$(libversion) libz.so.1)
	mkdir -p $(INCLUDE_INSTALL_PATH)
	cp -uv ../include/zaddons.h $(INCLUDE_INSTALL_PATH)

uninstall:
	@echo "removing $(INCLUDE_INSTALL_PATH) $(LIB_INSTALL_PATH)"
	$(RM) -r $(INCLUDE_INSTALL_PATH) $(LIB_INSTALL_PATH)

# general things
%.o: %.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $< -o $@
	$(CC) -MM $(CPPFLAGS) $(CFLAGS) $< > $*.d

clean distclean:
	$(RM) *.o *.d $(projs) *.so *.so.*
ifneq ($(CONFIG_LIBCXL_PATH),)
	$(MAKE) -C $(CONFIG_LIBCXL_PATH) $@
endif

-include $(objs:.o=.d) $(objs0:.o=.d) $(objs1:.o=.d) $(objs2:.o=.d)
