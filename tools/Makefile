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
libs = ../lib/libDDCB.a ../lib/libzHW.a ../lib/libcard.a $(libcxl_a) # static
LDLIBS += $(libs) -lpthread -lrt                                     # dynamic

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

projs = genwqe_update genwqe_gzip genwqe_gunzip zlib_mt_perf genwqe_memcopy \
	genwqe_echo genwqe_peek genwqe_poke genwqe_cksum \
	genwqe_vpdconv genwqe_vpdupdate genwqe_csv2vpd genwqe_loadtree \
	genwqe_ffdc

# Do this only on platforms where we have a libcxl available e.g. System p
ifneq ($(CONFIG_LIBCXL_PATH),)
projs += genwqe_maint
endif

all: $(projs)

genwqe_memcopy: force_cpu.o
genwqe_vpdconv genwqe_vpdupdate: genwqe_vpd_common.o

$(projs): $(libs)

objs = force_cpu.o genwqe_vpd_common.o $(projs:=.o)

manpages = $(projs:=.1)

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
	$(CC) -MM $(CPPFLAGS) $(CFLAGS) $< > $*.d

%.1: %
	$(HELP2MAN) -N --output=$@ --name "IBM Hardware Accelerator Tool." ./$<

#
# Tools for card maintenance
#
install_release_tools: all
	@mkdir -p $(DESTDIR)/bin
	cp -uv genwqe_peek genwqe_poke genwqe_memcopy genwqe_echo genwqe_update \
		$(DESTDIR)/bin

#
# Example tools and testcases
#   gzip and gunzip go into special directory, since they should not be
#   in regular path to be mixed up with distro version of those tools.
#   Nevertheless we need them to accelerate tar for example with our
#   hardware accelerated version of gzip/gunzip.
#
install_gzip_tools: all
	mkdir -p $(DESTDIR)/bin/genwqe
	cp -uv genwqe_gzip $(DESTDIR)/bin/genwqe/gzip
	cp -uv genwqe_gzip $(DESTDIR)/bin/genwqe/gunzip
	cp -uv zlib_mt_perf genwqe_mt_perf genwqe_test_gz $(DESTDIR)/bin

uninstall_gzip_tools:
	$(RM) $(DESTDIR)/bin/genwqe/gzip	\
	      $(DESTDIR)/bin/genwqe/gunzip	\
	      $(DESTDIR)/bin/zlib_mt_perf	\
	      $(DESTDIR)/bin/genwqe_mt_perf	\
	      $(DESTDIR)/bin/genwqe_test_gz

install_manpages: $(manpages)
	@mkdir -p $(DESTDIR)/man/man1
	cp -uv $(manpages) $(DESTDIR)/man/man1

install_release_manpages: $(manpages)
	@mkdir -p $(DESTDIR)/man/man1
	cp -uv genwqe_memcopy.1 genwqe_echo.1 genwqe_update.1 \
		$(DESTDIR)/man/man1

uninstall_manpages:
	@for f in $(manpages) ; do				\
		echo "removing $(DESTDIR)/man/man1/$$f ...";	\
		$(RM) $(DESTDIR)/man/man1/$$f;			\
	done

install: install_gzip_tools install_manpages
	@mkdir -p $(DESTDIR)/bin
	cp -uv $(projs) $(DESTDIR)/bin

uninstall: uninstall_gzip_tools uninstall_manpages
	@for f in $(projs) ; do					\
		echo "removing $(DESTDIR)/bin/$$f ...";		\
		$(RM) $(DESTDIR)/bin/$$f;			\
	done

clean distclean:
	$(RM) $(objs) $(projs) $(objs:.o=.d) $(manpages)

-include $(objs:.o=.d)
