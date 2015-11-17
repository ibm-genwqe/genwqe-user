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
	genwqe_vpdconv genwqe_vpdupdate csv2bin genwqe_loadtree genwqe_ffdc \
	genwqe_maint

objs = force_cpu.o genwqe_vpd_common.o $(projs:=.o)

manpages = $(projs:=.1)

all: $(libs) $(objs) $(projs)

manpages: all $(manpages)

genwqe_gunzip.o: genwqe_gzip.c
	$(CC) -c $< $(CPPFLAGS) $(CFLAGS) -o $@

$(libs):
	$(MAKE) -C ../lib

### Deactivate existing implicit rule
%: %.c

### Generic rule to build a tool
%: %.o
	$(CC) $(LDFLAGS) $@.o $($(@)_objs) $($(@)_libs) $(LDLIBS) -o $@

%.o: %.c $(libs)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $< -o $@
	$(CC) -MM $(CPPFLAGS) $(CFLAGS) $< > $*.d

%.1: %
	$(HELP2MAN) -N --output=$@ --name "IBM Hardware Accelerator Tool." ./$<

# NOTE: Distribute just genwqe_echo and genwqe_update
#       Build them with restricted user-interface.
install_release_tools: all
	@mkdir -p $(instdir)/bin
	cp -uv genwqe_memcopy genwqe_echo genwqe_update $(instdir)/bin

install_gzip_tools: all
	mkdir -p $(instdir)/bin/genwqe
	cp genwqe_gzip $(instdir)/bin/genwqe/gzip
	cp genwqe_gzip $(instdir)/bin/genwqe/gunzip
	cp zlib_mt_perf $(instdir)/bin/genwqe/
	cp zlib_mt_perf.sh $(instdir)/bin/genwqe/
	cp zlib_test_gz.sh $(instdir)/bin/genwqe/

uninstall_gzip_tools:
	$(RM) $(instdir)/bin/genwqe/gzip		\
	      $(instdir)/bin/genwqe/gunzip		\
	      $(instdir)/bin/genwqe/zlib_mt_perf	\
	      $(instdir)/bin/genwqe/zlib_mt_perf.sh	\
	      $(instdir)/bin/genwqe/zlib_test_gz.sh

install_vpd_tools: all
	@mkdir -p $(instdir)/bin
	cp -uv genwqe_vpdconv genwqe_vpdupdate csv2bin \
		$(instdir)/bin

install_manpages: $(manpages)
	@mkdir -p $(instdir)/man/man1
	cp -uv $(manpages) $(instdir)/man/man1

install_release_manpages: $(manpages)
	@mkdir -p $(instdir)/man/man1
	cp -uv genwqe_memcopy.1 genwqe_echo.1 genwqe_update.1 \
		$(instdir)/man/man1

install_vpd_manpages: $(manpages)
	@mkdir -p $(instdir)/man/man1
	cp -uv genwqe_vpdconv.1 genwqe_vpdupdate.1 csv2bin.1 \
		$(instdir)/man/man1

uninstall_manpages:
	@for f in $(manpages) ; do				\
		echo "removing $(instdir)/man/man1/$$f ...";	\
		$(RM) $(instdir)/man/man1/$$f;			\
	done

install: all install_gzip_tools install_manpages
	@mkdir -p $(instdir)/bin
	cp -uv $(projs) $(instdir)/bin

uninstall: uninstall_gzip_tools uninstall_manpages
	@for f in $(projs) ; do					\
		echo "removing $(instdir)/bin/$$f ...";		\
		$(RM) $(instdir)/bin/$$f;			\
	done

clean distclean:
	$(RM) *.o $(projs) $(objs:.o=.d) $(manpages)

-include $(objs:.o=.d)
