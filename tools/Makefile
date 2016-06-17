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

DESTDIR ?= /usr
libs = ../lib/libDDCB.a ../lib/libzHW.a ../lib/libcard.a
LDLIBS += $(libs) -lpthread -lrt

genwqe_peek_objs = force_cpu.o
genwqe_poke_objs = force_cpu.o
genwqe_memcopy_objs = force_cpu.o
genwqe_cksum_objs = force_cpu.o
genwqe_echo_objs = force_cpu.o
genwqe_vpdupdate_objs = genwqe_vpd_common.o
genwqe_vpdconv_objs = genwqe_vpd_common.o

genwqe_memcopy_libs = -lz
genwqe_cksum_libs = -lz
genwqe_gzip_libs = ../lib/libzADC.a -ldl	# statically link our libz
genwqe_gunzip_libs = ../lib/libzADC.a -ldl	# statically link our libz
zlib_mt_perf_libs = ../lib/libzADC.a -ldl	# statically link our libz
gzFile_test_libs = -L../lib -lzADC -ldl		# dynamically link our libz

projs = genwqe_update genwqe_gzip genwqe_gunzip zlib_mt_perf genwqe_memcopy \
	genwqe_echo genwqe_peek genwqe_poke genwqe_cksum genwqe_vpdconv \
	genwqe_vpdupdate genwqe_csv2vpd genwqe_ffdc gzFile_test

ifdef WITH_LIBCXL
# genwqe_maint is only used with CAPI support.
projs += genwqe_maint genwqe_loadtree
CAPI_INSTALL=capi_install

# If we are bundling, we need to link statically.  Otherwise, go dynamic.
ifeq ($(BUNDLE_LIBCXL),1)
LDLIBS += $(libcxl_a)
else
LDLIBS += -lcxl
endif # !CONFIG_LIBCXL_PATH

endif # WITH_LIBCXL

all: $(projs)

genwqe_memcopy: force_cpu.o
genwqe_vpdconv genwqe_vpdupdate: genwqe_vpd_common.o

$(projs): $(libs)

objs = force_cpu.o genwqe_vpd_common.o $(projs:=.o)

manpages = $(projs:=.1.gz)

manpages: all $(manpages)

genwqe_gunzip.o: genwqe_gzip.c
	$(CC) -c $< $(CPPFLAGS) $(CFLAGS) -o $@

### Deactivate existing implicit rule
%: %.c
%: %.sh

### Generic rule to build a tool
%: %.o
	$(CC) $(LDFLAGS) $@.o $($(@)_objs) $($(@)_libs) $(LDLIBS) -o $@

%.o: %.c $(libs)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $< -o $@

### Setting LD_LIBRARY_PATH helps to try tools with dynamic linkage
%.1: %
	LD_LIBRARY_PATH=../lib $(HELP2MAN) -N --output=$@ \
		--name "IBM Hardware Accelerator Tool." ./$<

%.1.gz: %.1
	gzip --best -c $< > $@

#
# Tools for card maintenance
#
install_release_tools: all
	@mkdir -p $(DESTDIR)/bin
	cp -uv genwqe_peek genwqe_poke genwqe_memcopy genwqe_echo \
		genwqe_update \
		$(DESTDIR)/bin

#
# Example tools and testcases
#   gzip and gunzip go into special directory, since they should not be
#   in regular path to be mixed up with distro version of those tools.
#   Nevertheless we need them to accelerate tar for example with our
#   hardware accelerated version of gzip/gunzip.
#
install_gzip_tools: all
	install -D -m 755 genwqe_gzip    -T $(DESTDIR)/bin/genwqe_gzip
	install -D -m 755 genwqe_gunzip  -T $(DESTDIR)/bin/genwqe_gunzip
	install -D -m 755 zlib_mt_perf   -T $(DESTDIR)/bin/zlib_mt_perf
	install -D -m 755 genwqe_mt_perf -T $(DESTDIR)/bin/genwqe_mt_perf
	install -D -m 755 genwqe_test_gz -T $(DESTDIR)/bin/genwqe_test_gz

uninstall_gzip_tools:
	$(RM) $(DESTDIR)/bin/genwqe_gzip \
	      $(DESTDIR)/bin/genwqe_gunzip \
	      $(DESTDIR)/bin/zlib_mt_perf \
	      $(DESTDIR)/bin/genwqe_mt_perf \
	      $(DESTDIR)/bin/genwqe_test_gz

install_manpages: $(manpages)
	@mkdir -p $(MAN_INSTALL_PATH)
	cp -uv $(manpages) $(MAN_INSTALL_PATH)

install_release_manpages: $(manpages)
	@mkdir -p $(MAN_INSTALL_PATH)

	cp -uv genwqe_memcopy.1 genwqe_echo.1 genwqe_update.1 \
		$(DESTDIR)/man/man1
	cp -uv genwqe_memcopy.1 genwqe_echo.1 genwqe_update.1 \
		$(MAN_INSTALL_PATH)

uninstall_manpages:
	@for f in $(manpages) ; do				\
		echo "removing $(DESTDIR)/man/man1/$$f ...";	\
		$(RM) $(DESTDIR)/man/man1/$$f;			\
	done

capi_install: genwqe_maint
	install -D -m 755 genwqe_maint -T $(DESTDIR)/bin/genwqe_maint
	install -D -m 755 genwqe_loadtree -T $(DESTDIR)/bin/genwqe_loadtree

install: install_gzip_tools install_manpages $(CAPI_INSTALL)
	@mkdir -p $(DESTDIR)/bin
	install -D genwqe_update -T $(DESTDIR)/bin/genwqe_update
	install -D -m 755 genwqe_memcopy   -T $(DESTDIR)/bin/genwqe_memcopy
	install -D -m 755 genwqe_echo      -T $(DESTDIR)/bin/genwqe_echo
	install -D -m 755 genwqe_peek      -T $(DESTDIR)/bin/genwqe_peek
	install -D -m 755 genwqe_poke      -T $(DESTDIR)/bin/genwqe_poke
	install -D -m 755 genwqe_cksum     -T $(DESTDIR)/bin/genwqe_cksum
	install -D -m 755 genwqe_vpdconv   -T $(DESTDIR)/bin/genwqe_vpdconv
	install -D -m 755 genwqe_vpdupdate -T $(DESTDIR)/bin/genwqe_vpdupdate
	install -D -m 755 genwqe_csv2vpd   -T $(DESTDIR)/bin/genwqe_csv2vpd
	install -D -m 755 genwqe_ffdc      -T $(DESTDIR)/bin/genwqe_ffdc

uninstall: uninstall_gzip_tools uninstall_manpages
	@for f in $(projs) ; do					\
		echo "removing $(DESTDIR)/bin/$$f ...";		\
		$(RM) $(DESTDIR)/bin/$$f;			\
	done

clean distclean:
	$(RM) $(objs) $(projs) $(manpages)

