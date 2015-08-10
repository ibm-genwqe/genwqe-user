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

libs = ../lib/libDDCB.a ../lib/libzHW.a ../lib/libcard.a

ifeq ($(PLATFORM),ppc64le)
	libs += ../../libcxl/libcxl.a
endif

LDLIBS += $(libs) -lpthread -lrt

genwqe_memcopy_objs = force_cpu.o
genwqe_echo_objs = force_cpu.o
genwqe_vpdupdate_objs = genwqe_vpd_common.o
genwqe_vpdconv_objs = genwqe_vpd_common.o

genwqe_memcopy_libs = -lz
genwqe_gzip_libs = ../lib/libzADC.a -ldl
genwqe_gunzip_libs = ../lib/libzADC.a -ldl
zlib_mt_perf_libs = ../lib/libzADC.a -ldl

projs = genwqe_update genwqe_gzip genwqe_gunzip zlib_mt_perf genwqe_memcopy genwqe_echo

objs = force_cpu.o genwqe_vpd_common.o $(projs:=.o)

manpages = $(projs:=.1)
manpages := $(filter-out zpipe.1 zpipe2.1 sglist_test.1, $(manpages))

all:  $(libs) $(objs) $(projs)

manpages: all $(manpages)

genwqe_gunzip.o: genwqe_gzip.c
	$(CC) -c $< $(CPPFLAGS) $(CFLAGS) -o $@

$(libs):
	$(MAKE) -C ../lib

### Deactivate existing implicit rule
% : %.c

### Generic rule to build a tool
%: %.o
	$(CC) $@.o $($(@)_objs) $($(@)_libs) $(LDLIBS) -o $@

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

uninstall_gzip_tools:
	$(RM) -r $(instdir)/bin/genwqe

install_manpages: $(manpages)
	@mkdir -p $(instdir)/man/man1
	cp -uv genwqe_echo genwqe_update $(instdir)/man/man1

uninstall_manpages:
	@for f in genwqe_echo genwqe_update ; do		\
		echo "removing $(instdir)/man/man1/$$f ...";	\
		$(RM) $(instdir)/man/man1/$$f;			\
	done

install_release_manpages: $(manpages)
	@mkdir -p $(instdir)/man/man1
	cp -uv $(man3) $(man4) $(instdir)/man/man1

install_src:
	@mkdir -p $(GENWQETOOLSSRC)/tools
	cp *.c *.h Makefile $(GENWQETOOLSSRC)/tools

uninstall_src:
	$(RM) $(GENWQETOOLSSRC)/tools/*.c \
		$(GENWQETOOLSSRC)/tools/*.h \
		$(GENWQETOOLSSRC)/tools/Makefile


install: $(projs)
	@mkdir -p $(instdir)/bin
	cp -uv $(projs) $(instdir)/bin

uninstall:
	@for f in $(projs) ; do					\
		echo "removing $(instdir)/bin/$$f ...";		\
		$(RM) $(instdir)/bin/$$f;			\
	done

clean:
	$(RM) *.o $(projs) $(objs:.o=.d) $(manpages) *.gcov *.gcda *.gcno gmon.out

-include $(objs:.o=.d)
