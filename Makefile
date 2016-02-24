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
V ?= 2

ifeq ($(V),0)
Q		:= @
MAKEFLAGS	+= --silent
MAKE		+= -s
endif

ifeq ($(V),1)
MAKEFLAGS	+= --silent
MAKE		+= -s
endif

HAS_GIT = $(shell git --help > /dev/null 2>&1 && echo y || echo n)

ifeq (${HAS_GIT},y)
VERSION ?= $(shell git describe --abbrev=4 --dirty --always --tags)
RPMVERSION ?= $(shell git describe --abbrev=0 --tags)
else
VERSION=4.0.11
RPMVERSION=$(VERSION)
endif

PLATFORM ?= $(shell uname -i)

distro = $(shell lsb_release -d | cut -f2)
subdirs += lib tools
ifeq ($(PLATFORM),ppc64le)
subdirs += init
endif
targets += $(subdirs)

UDEV_RULES_D ?= /etc/udev/rules.d
MODPROBE_D ?= /etc/modprobe.d


all: $(targets)

tools: lib

# z_ prefixed version of libz, intended to be linked statically with
# our libz version to provide the software zlib functionality.
#
ifeq ($(CONFIG_DLOPEN_MECHANISM),0)

HAS_WGET = $(shell which wget > /dev/null 2>&1 && echo y || echo n)
HAS_CURL = $(shell which curl > /dev/null 2>&1 && echo y || echo n)
OBJCOPY = @printf "\t[OBJCP]\t%s\n" `basename "$@"`; $(CROSS)objcopy

define Q
  @/bin/echo -e "	[$1]\t$(2)"
  @$(3)
endef

lib: zlib-1.2.8/libz.so

zlib-1.2.8/libz.so: zlib-1.2.8.cfg
	@/bin/echo -e "	[BUILD]\tzlib-1.2.8"
	@$(MAKE) -C zlib-1.2.8 1>&2 > /dev/null

zlib-1.2.8.cfg: zlib-1.2.8.tar.gz
	@/bin/echo -e "	[TAR]\t$<"
	@tar xfz $<
	@/bin/echo -e "	[CFG]\tzlib-1.2.8"
	@(cd zlib-1.2.8 && CFLAGS=-O2 ./configure --prefix=/opt/genwqe) \
		1>&2 > /dev/null
	@touch zlib-1.2.8.cfg

zlib-1.2.8.tar.gz:
ifeq (${HAS_WGET},y)
	$(call Q,WGET,zlib-1.2.8.tar.gz, wget -O zlib-1.2.8.tar.gz -q http://www.zlib.net/zlib-1.2.8.tar.gz)
else ifeq (${HAS_CURL},y)
	$(call Q,CURL,zlib-1.2.8.tar.gz, curl -o zlib-1.2.8.tar.gz -s http://www.zlib.net/zlib-1.2.8.tar.gz)
endif

endif

# Only build if the subdirectory is really existent
.PHONY: $(subdirs) install
$(subdirs):
	@if [ -d $@ ]; then					\
		$(MAKE) -C $@ C=0 || exit 1; \
	fi

rpmbuild_setup:
	@mkdir -p ~/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	$(RM) ~/.rpmmacros
	echo '%_topdir %(echo $$HOME)/rpmbuild' >  ~/.rpmmacros

#
# Create required tar.gz archive and copy everything to the right
# places. Create version.mk since the Fedora build system requires
# running without git.
#
rpmbuild:
	@$(MAKE) -s distclean
	@rm -rf /tmp/genwqe-$(RPMVERSION)
	@mkdir -p /tmp/genwqe-$(RPMVERSION)
	@cp -ar * /tmp/genwqe-$(RPMVERSION)/
	(cd /tmp && tar cfz genwqe-$(RPMVERSION).tar.gz genwqe-$(RPMVERSION))
	rpmbuild -ta -v --define 'srcVersion $(RPMVERSION)' \
		--define 'srcRelease 1'			\
		--define 'Version $(RPMVERSION)'	\
		/tmp/genwqe-$(RPMVERSION).tar.gz

# Install/Uninstall
install uninstall:
	@for dir in $(subdirs); do 					  \
		if [ -d $$dir ]; then					  \
			$(MAKE) -C $$dir $@ || exit 1; \
		fi							  \
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

help:
	@echo "Build GenWQE/CAPI hardware accelerator tools"
	@echo
	@echo "Possible Makefile options:"
	@echo "  V=0 silent, 1 normal (default), 2 verbose"
	@echo "  FORCE_32BIT=0 64-bit (default), 1 32-bit"
	@echo "  BUILD_SIMCODE=1 use pslse version of libcxl, 0 use libcxl "
	@echo "      (default)"
	@echo "  CONFIG_DLOPEN_MECHANISM=0 statically link against private"
	@echo "      software zlib, 1 use dlopen to include software zlib"
	@echo "      (default)"
	@echo

distclean: clean
	@$(RM) -r sim_*	zlib-1.2.8 zlib-1.2.8.tar.gz

clean:
	@for dir in $(subdirs); do 			\
		if [ -d $$dir ]; then			\
			$(MAKE) -C $$dir $@ || exit 1;	\
		fi					\
	done
	@$(RM) genwqe-$(RPMVERSION).tgz	libz.o libz_prefixed.o zlib-1.2.8.cfg
	@if [ -d zlib-1.2.8 ]; then 			\
		$(MAKE) -s -C zlib-1.2.8 distclean;	\
	fi
	@find . -depth -name '*~'  -exec rm -rf '{}' \; -print
	@find . -depth -name '.#*' -exec rm -rf '{}' \; -print
