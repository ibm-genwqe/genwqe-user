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

# Verbose level:
#   V=0 means completely silent
#   V=1 means brief output
#   V=2 means full output
V		?= 2

ifeq ($(V),0)
	Q		:= @
	MAKEFLAGS	+= --silent
	MAKE		+= -s
endif

ifeq ($(V),1)
	MAKEFLAGS	+= --silent
	MAKE		+= -s
endif

# VERSION string *cannot* be empty.  It must be provided either in the
# version.mk file, in the command line as VERSION= or loaded by using
# the git repository information.
ifneq ("$(wildcard version.mk)", "")
	include ./version.mk
else
	VERSION ?= $(shell git describe --abbrev=4 --dirty --always --tags)
	RPMVERSION ?= $(shell git describe --abbrev=0 --tags)
endif

instdir = /opt/genwqe

distro = $(shell lsb_release -d | cut -f2)
subdirs += lib tools
targets += zlib-1.2.8.log $(subdirs)

UDEV_RULES_D ?= /etc/udev/rules.d
MODPROBE_D ?= /etc/modprobe.d

HAS_WGET = $(shell which wget > /dev/null 2>&1 && echo y || echo n)
HAS_CURL = $(shell which curl > /dev/null 2>&1 && echo y || echo n)
OBJCOPY = @printf "\t[OBJCP]\t%s\n" `basename "$@"`; $(CROSS)objcopy

define Q
  @/bin/echo -e "	[$1]\t$(2)"
  @$(3)
endef

all: $(targets)

tools: lib

.PHONY: $(subdirs) distclean clean build_dist install uninstall \
	install_src uninstall_src copy_test_code \
	install_test_code build_testcode

zlib-1.2.8.log: zlib-1.2.8
	@touch zlib-1.2.8.log
	@/bin/echo -e "	[CFG]\tzlib-1.2.8"
	@(cd zlib-1.2.8 && CFLAGS=-O2 ./configure --prefix=/opt/genwqe) \
		1>&2 >> zlib-1.2.8.log
	@/bin/echo -e "	[BUILD]\tzlib-1.2.8"
	@$(MAKE) -C zlib-1.2.8 1>&2 >> zlib-1.2.8.log
	$(OBJCOPY) --prefix-symbols=z_ zlib-1.2.8/libz.a zlib-1.2.8/z_libz.a 
	@touch zlib-1.2.8.log

zlib-1.2.8: zlib-1.2.8.tar.gz
	@touch zlib-1.2.8.log
	@/bin/echo -e "	[TAR]\t$<"
	@tar xfz $< 1>&2 > zlib-1.2.8.log

zlib-1.2.8.tar.gz:
ifeq (${HAS_WGET},y)
	$(call Q,WGET,zlib-1.2.8.tar.gz, wget -O zlib-1.2.8.tar.gz -q http://www.zlib.net/zlib-1.2.8.tar.gz)
else ifeq (${HAS_CURL},y)
	$(call Q,CURL,zlib-1.2.8.tar.gz, curl -o zlib-1.2.8.tar.gz -s http://www.zlib.net/zlib-1.2.8.tar.gz)
endif

# Only build if the subdirectory is really existent
$(subdirs):
	@if [ -d $@ ]; then			\
		$(MAKE) -C $@ C=0 VERSION=$(VERSION) || exit 1; \
	fi

rpmbuild_setup:
	@mkdir -p ~/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	$(RM) ~/.rpmmacros
	echo '%_topdir %(echo $$HOME)/rpmbuild' >  ~/.rpmmacros

rpmbuild: genwqe-tools genwqe-libz genwqe-vpd

genwqe-tools genwqe-libz genwqe-vpd:
	@$(MAKE) -s distclean
	@rm version.mk
	@echo "VERSION:=$(VERSION)" > version.mk
	@echo "RPMVERSION:=$(RPMVERSION)" >> version.mk
	@rm -rf /tmp/genwqe-$(RPMVERSION)
	@mkdir -p /tmp/genwqe-$(RPMVERSION)
	@cp -ar .git /tmp/genwqe-$(RPMVERSION)/
	@cp -ar * /tmp/genwqe-$(RPMVERSION)/
	(cd /tmp && tar cfz genwqe-$(RPMVERSION).tar.gz genwqe-$(RPMVERSION))
	rpmbuild -ta -v --define 'srcVersion $(RPMVERSION)' \
		--define 'srcRelease 1'			\
		--define 'Version $(RPMVERSION)'	\
		/tmp/genwqe-$(RPMVERSION).tar.gz

# Install/Uninstall
install uninstall:
	@for dir in $(subdirs); do 			\
		if [ -d $$dir ]; then			\
			$(MAKE) -C $$dir $@ || exit 1;	\
		fi					\
	done

install_udev_rules:
	mkdir -p $(UDEV_RULES_D)
	cp etc/udev/rules.d/52-genwqedevices.rules $(UDEV_RULES_D)/

uninstall_udev_rules:
	$(RM) $(UDEV_RULES_D)/52-genwqedevices.rules

install_modprobe_d:
	mkdir -p $(MODPROBE_D)
	cp etc/modprobe.d/genwqe.conf $(MODPROBE_D)/

uninstall_modprobe_d:
	$(RM) $(MODPROBE_D)/genwqe.conf

# FIXME This is a problem which occurs on distributions which have a
# genwqe_card.h header file which is different from our local one.  If
# there is a better solution than renaming it to get it out of the way
# please fix this.
#
fixup_headerfile_problem:
	@if [ -f /usr/src/kernels/`uname -r`/include/uapi/linux/genwqe/genwqe_card.h ]; then \
		sudo mv /usr/src/kernels/`uname -r`/include/uapi/linux/genwqe/genwqe_card.h \
			/usr/src/kernels/`uname -r`/include/uapi/linux/genwqe/genwqe_card.h.orig ; \
	fi

distclean: clean
	@$(RM) -r sim_*	zlib-1.2.8 zlib-1.2.8.tar.gz

clean:
	@for dir in $(subdirs); do 			\
		if [ -d $$dir ]; then			\
			$(MAKE) -C $$dir $@ || exit 1;	\
		fi					\
	done
	@$(RM) *~ */*~ 					\
		genwqe-tools-$(RPMVERSION).tgz 		\
		genwqe-zlib-$(RPMVERSION).tgz		\
		zlib-1.2.8/z_libz.a zlib-1.2.8.log
	@if [ -d zlib-1.2.8 ]; then 			\
		$(MAKE) -s -C zlib-1.2.8 distclean;	\
	fi
	@find . -depth -name '*~'  -exec rm -rf '{}' \; -print
	@find . -depth -name '.#*' -exec rm -rf '{}' \; -print
